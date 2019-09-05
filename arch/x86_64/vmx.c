#include <arch/x86_64-msr.h>
#include <arch/x86_64-vmx.h>
#include <memory.h>
#include <object.h>
#include <processor.h>
#include <secctx.h>
static uint32_t revision_id;
static uintptr_t ept_root;
static _Atomic long vmexits_count = 0;

static bool support_ept_switch_vmfunc = false;
static bool support_virt_exception = false;

static void x86_64_vmenter(struct processor *proc);

static const char *vm_errs[] = { "Success",
	"VMCALL in vmx-root",
	"VMCLEAR with invalid address",
	"VMCLEAR with VMXON ptr",
	"VMLAUNCH with non-clear VMCS",
	"VMRESUME non-launched VMCS",
	"VMRESUME after VMXOFF",
	"VM entry with invalid control fields",
	"VM entry with invalid host-state fields",
	"VMPTRLD with invalid address",
	"VMPTRLD with VMXON ptr",
	"VMPTRLD with incorrect revision ID",
	"VM read/write from/to unsupported component",
	"VM write to read-only component",
	"[reserved]",
	"VMXON in vmx-root",
	"VM entry with invalid executive VMCS pointer",
	"VM entry with non-launched VMCS",
	"VM entry something?",
	"VMCALL with non-clear VMCS",
	"VMCALL with invalid VM exit control fields",
	"[reserved]",
	"VMCALL with incorrect revision ID",
	"VMXOFF in dual monitor",
	"VMCALL with invalid SMM features",
	"VM entry with invalid VM execution control fields",
	"VM entry with events blocked by mov ss",
	"[reserved]",
	"invalid operand to INVEPT/INVVPID" };

static inline unsigned long vmcs_readl(unsigned long field)
{
	unsigned long val;
	asm volatile("vmread %%rdx, %%rax" : "=a"(val) : "d"(field) : "cc");
	return val;
}

static inline const char *vmcs_read_vminstr_err(void)
{
	uint32_t err = vmcs_readl(VMCS_VM_INSTRUCTION_ERROR);
	if(err >= array_len(vm_errs)) {
		panic("invalid error code from vm instruction error field");
	}
	return vm_errs[err];
}

static inline void vmcs_writel(unsigned long field, unsigned long val)
{
	uint8_t err;
	asm volatile("vmwrite %%rax, %%rdx; setna %0" : "=q"(err) : "a"(val), "d"(field) : "cc");
	if(unlikely(err)) {
		panic("vmwrite error: %s", vmcs_read_vminstr_err());
	}
}

static inline void vmcs_write32_fixed(uint32_t msr, uint32_t vmcs_field, uint32_t val)
{
	uint32_t msr_high, msr_low;

	x86_64_rdmsr(msr, &msr_low, &msr_high);
	val &= msr_high;
	val |= msr_low;
	vmcs_writel(vmcs_field, val);
}
static bool x86_64_ept_map(uintptr_t ept_phys,
  uintptr_t virt,
  uintptr_t phys,
  int level,
  uint64_t flags,
  bool remap);

static void x86_64_enable_vmx(void)
{
	uint32_t ecx = x86_64_cpuid(1, 0, 2);
	if(!(ecx & (1 << 5))) {
		panic("VMX extensions not available (not supported");
	}

	/* TODO: use this info */
	uint32_t eax = x86_64_cpuid(0x80000008, 0, 0);

	uint32_t lo, hi;
	x86_64_rdmsr(X86_MSR_FEATURE_CONTROL, &lo, &hi);
	if(!(lo & 1)) {
		lo |= (1 << 2) | 1;
		x86_64_wrmsr(X86_MSR_FEATURE_CONTROL, lo, hi);
	}
	x86_64_rdmsr(X86_MSR_FEATURE_CONTROL, &lo, &hi);
	if(!(lo & (1 << 2)) /* enable-outside-smx bit */) {
		panic("VMX extensions not available (not enabled)");
	}

	/* okay, now try to enable VMX instructions. */
	uint64_t cr4;
	asm volatile("mov %%cr4, %0" : "=r"(cr4));
	cr4 |= (1 << 13); // enable VMX
	uintptr_t vmxon_region = mm_physical_alloc(0x1000, PM_TYPE_DRAM, true);

	/* get the revision ID. This is needed for several VM data structures. */
	x86_64_rdmsr(X86_MSR_VMX_BASIC, &lo, &hi);
	revision_id = lo & 0x7FFFFFFF;
	uint32_t *vmxon_revid = (uint32_t *)mm_ptov(vmxon_region);
	*vmxon_revid = revision_id;

	uint8_t error;
	/* enable VT-x by setting the enable bit in cr4 and executing the 'on' vmx instr. */
	asm volatile("mov %1, %%cr4; vmxon %2; setc %0"
	             : "=g"(error)
	             : "r"(cr4), "m"(vmxon_region)
	             : "cc");
	if(error) {
		panic("failed to enter VMX operation");
	}

	x86_64_rdmsr(X86_MSR_VMX_PROCBASED_CTLS, &lo, &hi);
	if(!(hi & (1ul << 31))) {
		panic("VMX doesn't support secondary controls");
	}

	x86_64_rdmsr(X86_MSR_VMX_PROCBASED_CTLS2, &lo, &hi);

	if((hi & (1 << 18))) {
		support_virt_exception = true;
	}

	if((hi & (1 << 13))) {
		x86_64_rdmsr(X86_MSR_VMX_VMFUNC, &lo, &hi);
		if(lo & 1) {
			support_ept_switch_vmfunc = true;
		}
	}

	printk("processor %d entered vmx-root (VE=%d, VMF=%d)\n",
	  current_processor->id,
	  support_virt_exception,
	  support_ept_switch_vmfunc);
}

__attribute__((used)) static void x86_64_vmentry_failed(struct processor *proc)
{
	panic("vmentry failed on processor %d: %s\n", proc->id, vmcs_read_vminstr_err());
}

static void vmx_queue_exception(unsigned int nr)
{
	vmcs_writel(
	  VMCS_ENTRY_INTR_INFO, (nr & 0xff) | INTR_TYPE_HARD_EXCEPTION | INTR_INFO_VALID_MASK);
}

/* Some vmexits leave the RIP pointing to the current instruction,
 * so we must advance by the instruction length or else that instruction
 * will just be re-executed. But we don't always want to do this (eg if we
 * exit due to ept violation) */
static void vm_instruction_advance(void)
{
	unsigned len = vmcs_readl(VMCS_VM_INSTRUCTION_LENGTH);
	uint64_t grip = vmcs_readl(VMCS_GUEST_RIP);
	vmcs_writel(VMCS_GUEST_RIP, grip + len);
}

static void vmx_handle_rootcall(struct processor *proc)
{
	long fn = proc->arch.vcpu_state_regs[REG_RDI];
	long a0 = proc->arch.vcpu_state_regs[REG_RSI];
	// long a1 = proc->vcpu_state_regs[REG_RDX];
	// long a2 = proc->vcpu_state_regs[REG_RCX];

	/* Make sure we only accept calls from kernel-mode.
	 * Linux looks at the AR bytes instead of the selector DPL.
	 * Why? I don't know. AFAIK there is no reason not to do this
	 * test with the selector itself. */
	uint16_t sel = vmcs_readl(VMCS_GUEST_CS_SEL);
	if(sel & 3) {
		/* Call from user-mode. Not allowed! */
		proc->arch.vcpu_state_regs[REG_RAX] = -1;
		return;
	}

	/* set the return value, 0 by default */
	proc->arch.vcpu_state_regs[REG_RAX] = 0;
	switch(fn) {
		case VMX_RC_SWITCHEPT:
			vmcs_writel(VMCS_EPT_PTR, (uintptr_t)a0 | (3 << 3) | 6);
			break;
		default:
			panic("Unknown root call %ld", fn);
	}
}

static void vmx_handle_ept_violation(struct processor *proc)
{
	if(proc->arch.veinfo->lock != 0) {
		panic("virtualization information area lock is not 0:\n(%lx %lx %lx) -> (%lx %lx %lx) at "
		      "%ld:%lx (%x)",
		  proc->arch.veinfo->qual,
		  proc->arch.veinfo->physical,
		  proc->arch.veinfo->linear,
		  vmcs_readl(VMCS_EXIT_QUALIFICATION),
		  vmcs_readl(VMCS_GUEST_PHYSICAL_ADDRESS),
		  vmcs_readl(VMCS_GUEST_LINEAR_ADDRESS),
		  current_thread->id,
		  vmcs_readl(VMCS_GUEST_RIP),
		  proc->arch.veinfo->lock);
	}
	proc->arch.veinfo->reason = VMEXIT_REASON_EPT_VIOLATION;
	proc->arch.veinfo->qual = vmcs_readl(VMCS_EXIT_QUALIFICATION);
	proc->arch.veinfo->physical = vmcs_readl(VMCS_GUEST_PHYSICAL_ADDRESS);
	proc->arch.veinfo->linear = vmcs_readl(VMCS_GUEST_LINEAR_ADDRESS);
	proc->arch.veinfo->eptidx = 0; /* TODO (minor) */
	proc->arch.veinfo->lock = 0xFFFFFFFF;
	vmx_queue_exception(20); /* #VE */
}

extern uint64_t clksrc_get_interrupt_countdown();
void x86_64_vmexit_handler(struct processor *proc)
{
	/* so, in theory we now have a valid pointer to a vstate struct, and a stack
	 * to work in (see assembly code for vmexit point below).  */
	proc->arch.launched = 1; /* we must have launched if we are here */
	unsigned long reason = vmcs_readl(VMCS_EXIT_REASON);
	unsigned long qual = vmcs_readl(VMCS_EXIT_QUALIFICATION);
	unsigned long grip = vmcs_readl(VMCS_GUEST_RIP);
	unsigned long iinfo = vmcs_readl(VMCS_VM_INSTRUCTION_INFO);
	/*
	    if(reason != VMEXIT_REASON_CPUID
	            && reason != VMEXIT_REASON_VMCALL
	            && reason != VMEXIT_REASON_EPT_VIOLATION
	            && reason != VMEXIT_REASON_INVEPT)
	            */
	// printk("VMEXIT occurred at %lx: reason=%ld, qual=%lx, iinfo=%lx\n", grip, reason, qual,
	// iinfo);

	// printk(":: %lx\n", clksrc_get_interrupt_countdown());

	// uint32_t x, y;
	// x86_64_rdmsr(X86_MSR_GS_BASE, &x, &y);
	// printk("GS: %x %x\nPROC: %p", y, x, &proc->arch);

	atomic_fetch_add(&vmexits_count, 1);
	switch(reason) {
		uintptr_t val;
		case VMEXIT_REASON_CPUID:
			/* just execute cpuid using the guest's registers */
			asm volatile(
			  "push %%rbx; cpuid; mov %%rbx, %0; pop %%rbx"
			  : "=a"(proc->arch.vcpu_state_regs[REG_RAX]),
			  "=g"(proc->arch.vcpu_state_regs[REG_RBX]),
			  "=c"(proc->arch.vcpu_state_regs[REG_RCX]),
			  "=d"(proc->arch.vcpu_state_regs[REG_RDX])
			  : "a"(proc->arch.vcpu_state_regs[REG_RAX]), "c"(proc->arch.vcpu_state_regs[REG_RCX])
			  : "memory");
			vm_instruction_advance();
			break;
		case VMEXIT_REASON_VMCALL:
			vmx_handle_rootcall(proc);
			vm_instruction_advance();
			break;
		case VMEXIT_REASON_EPT_VIOLATION:
			vmx_handle_ept_violation(proc);
			/* don't instruction advance, since we want to retry the access. */
			break;
		case VMEXIT_REASON_INVEPT:
			/* TODO (perf): don't invalidate all mappings */
			val = vmcs_readl(VMCS_EPT_PTR);
			unsigned __int128 eptp = val;
			asm volatile("invept %0, %%rax" ::"m"(eptp), "a"(proc->arch.vcpu_state_regs[REG_RAX])
			             : "memory");
			vm_instruction_advance();
			break;
		default:
			panic("Unhandled VMEXIT: %ld %lx %lx", reason, qual, grip);
			break;
	}

	x86_64_vmenter(proc);
}

__noinstrument static void x86_64_vmenter(struct processor *proc)
{
	/* VMCS does not deal with CPU registers, so we must save and restore them. */

	/* any time we trap back to the "hypervisor" we need this state reset */
	vmcs_writel(VMCS_HOST_RSP, (uintptr_t)proc->arch.vcpu_state_regs);
	asm volatile(
	  "cli;"
	  "pushf;"
	  "push %%rcx;"
	  "cmp $0, %0;"
	  /* load the "guest" registers into the cpu. We can trash the host regs because
	   * we are also the guest. Wheee. */
	  "mov %c[cr2](%%rcx), %%rax;"
	  "mov %%rax, %%cr2;" /* CR2 must be loaded from a register */
	  "mov %c[rax](%%rcx), %%rax;"
	  "mov %c[rbx](%%rcx), %%rbx;"
	  "mov %c[rdx](%%rcx), %%rdx;"
	  "mov %c[rdi](%%rcx), %%rdi;"
	  "mov %c[rsi](%%rcx), %%rsi;"
	  "mov %c[rbp](%%rcx), %%rbp;"
	  "mov %c[r8](%%rcx),  %%r8;"
	  "mov %c[r9](%%rcx),  %%r9;"
	  "mov %c[r10](%%rcx), %%r10;"
	  "mov %c[r11](%%rcx), %%r11;"
	  "mov %c[r12](%%rcx), %%r12;"
	  "mov %c[r13](%%rcx), %%r13;"
	  "mov %c[r14](%%rcx), %%r14;"
	  "mov %c[r15](%%rcx), %%r15;"
	  "mov %c[rcx](%%rcx), %%rcx;" /* this kills rcx */
	  /* okay, now try to start the "vm". This means either vmlaunch (if we're initializing), or
	     vmresume otherwise. */
	  "jne launched;"
	  "vmlaunch; jmp failed;"
	  "launched: vmresume; failed:"
	  "pop %%rcx;"
	  "mov %c[procptr](%%rcx), %%rdi;" /* we destroyed our registers, so lets load a pointer to the
	                                      proc structure. */
	  "popf;"                          /* restore our flags */
	  "call x86_64_vmentry_failed;"
	  /* okay, let's have an assembly stub for a VM exit */
	  ".global vmexit_point;"
	  "vmexit_point:;"
	  /* save the guest registers for reloading later. Again, we don't need to reload host regs
	   * because we're treating this like any other "kernel entry", so we can just start from a
	   * stack with a pointer to a processor. */
	  "mov %%rax, %c[rax](%%rsp);" /* help me im lost in assembly land */
	  "mov %%rbx, %c[rbx](%%rsp);"
	  "mov %%rcx, %c[rcx](%%rsp);"
	  "mov %%rdx, %c[rdx](%%rsp);"
	  "mov %%rdi, %c[rdi](%%rsp);"
	  "mov %%rsi, %c[rsi](%%rsp);"
	  "mov %%rbp, %c[rbp](%%rsp);"
	  "mov %%r8,  %c[r8](%%rsp);"
	  "mov %%r9,  %c[r9](%%rsp);"
	  "mov %%r10, %c[r10](%%rsp);"
	  "mov %%r11, %c[r11](%%rsp);"
	  "mov %%r12, %c[r12](%%rsp);"
	  "mov %%r13, %c[r13](%%rsp);"
	  "mov %%r14, %c[r14](%%rsp);"
	  "mov %%r15, %c[r15](%%rsp);"
	  /* okay, load the processor pointer and the _actual_ "host" stack pointer (hyper stack) */
	  "mov %c[procptr](%%rsp), %%rdi;"
	  "mov %c[hrsp](%%rsp), %%rsp;"
	  /* and go back to C */
	  "jmp x86_64_vmexit_handler;" ::"r"(proc->arch.launched),
	  "c"(proc->arch.vcpu_state_regs),
	  [ guest_cs_sel ] "i"(VMCS_GUEST_CS_SEL),
	  [ rax ] "i"(REG_RAX * 8),
	  [ rbx ] "i"(REG_RBX * 8),
	  [ rcx ] "i"(REG_RCX * 8),
	  [ rdx ] "i"(REG_RDX * 8),
	  [ rdi ] "i"(REG_RDI * 8),
	  [ rsi ] "i"(REG_RSI * 8),
	  [ rbp ] "i"(REG_RBP * 8),
	  [ r8 ] "i"(REG_R8 * 8),
	  [ r9 ] "i"(REG_R9 * 8),
	  [ r10 ] "i"(REG_R10 * 8),
	  [ r11 ] "i"(REG_R11 * 8),
	  [ r12 ] "i"(REG_R12 * 8),
	  [ r13 ] "i"(REG_R13 * 8),
	  [ r14 ] "i"(REG_R14 * 8),
	  [ r15 ] "i"(REG_R15 * 8),
	  [ cr2 ] "i"(REG_CR2 * 8),
	  [ hrsp ] "i"(HOST_RSP * 8),
	  [ procptr ] "i"(PROC_PTR * 8));
}

/* we cannot just specify a C function as an entry point, because
 * function preludes assume a mis-aligned stack (so they can push rbp). Thus we
 * need to enter to an assembly stub which calls the C code for correct
 * alignment. */
__attribute__((used)) static void vmx_entry_point_c(struct processor *proc)
{
	printk("processor %d entered vmx-non-root mode\n", proc->id);
	x86_64_processor_post_vm_init(proc);
}

asm(".global vmx_entry_point\n"
    ".extern vmx_entry_point_c\n"
    "vmx_entry_point:\n"
    "call vmx_entry_point_c\n");

static uint64_t read_cr(int n)
{
	uint64_t v = 0;
	switch(n) {
		case 0:
			asm volatile("mov %%cr0, %%rax" : "=a"(v));
			break;
		/* cr1 is not accessible */
		case 2:
			asm volatile("mov %%cr2, %%rax" : "=a"(v));
			break;
		case 3:
			asm volatile("mov %%cr3, %%rax" : "=a"(v));
			break;
		case 4:
			asm volatile("mov %%cr4, %%rax" : "=a"(v));
			break;
		default:
			panic("invalid control register");
	}
	return v;
}

static uint64_t read_efer(void)
{
	uint32_t lo, hi;
	x86_64_rdmsr(X86_MSR_EFER, &lo, &hi);
	return ((uint64_t)hi << 32) | lo;
}

static inline void test_and_allocate(uintptr_t *loc, uint64_t attr)
{
	if(!*loc) {
		*loc = (uintptr_t)mm_physical_alloc(0x1000, PM_TYPE_DRAM, true) | (attr & RECUR_ATTR_MASK);
	}
}

static bool x86_64_ept_getmap(uintptr_t ept_phys,
  uintptr_t virt,
  uintptr_t *phys,
  int *level,
  uint64_t *flags)
{
	int pml4_idx = PML4_IDX(virt);
	int pdpt_idx = PDPT_IDX(virt);
	int pd_idx = PD_IDX(virt);
	int pt_idx = PT_IDX(virt);

	uintptr_t *pml4 = GET_VIRT_TABLE(ept_phys);
	if(!pml4[pml4_idx])
		return false;

	uintptr_t *pdpt = GET_VIRT_TABLE(pml4[pml4_idx]);

	if(!pdpt[pdpt_idx])
		return false;
	else if(pdpt[pdpt_idx] & PAGE_LARGE) {
		if(phys)
			*phys = pdpt[pdpt_idx] & VM_PHYS_MASK;
		if(flags)
			*flags = pdpt[pdpt_idx] & ~VM_PHYS_MASK;
		if(level)
			*level = 2;
		return true;
	}

	uintptr_t *pd = GET_VIRT_TABLE(pdpt[pdpt_idx]);

	if(!pd[pd_idx])
		return false;
	else if(pd[pd_idx] & PAGE_LARGE) {
		if(phys)
			*phys = pd[pd_idx] & VM_PHYS_MASK;
		if(flags)
			*flags = pd[pd_idx] & ~VM_PHYS_MASK;
		if(level)
			*level = 1;
		return true;
	}

	uintptr_t *pt = GET_VIRT_TABLE(pd[pd_idx]);

	if(!pt[pt_idx])
		return false;
	if(phys)
		*phys = pt[pt_idx] & VM_PHYS_MASK;
	if(flags)
		*flags = pt[pt_idx] & ~VM_PHYS_MASK;
	if(level)
		*level = 0;
	return true;
}

static bool x86_64_ept_map(uintptr_t ept_phys,
  uintptr_t virt,
  uintptr_t phys,
  int level,
  uint64_t flags,
  bool remap)
{
	int pml4_idx = PML4_IDX(virt);
	int pdpt_idx = PDPT_IDX(virt);
	int pd_idx = PD_IDX(virt);
	int pt_idx = PT_IDX(virt);

	uintptr_t *pml4 = GET_VIRT_TABLE(ept_phys);
	test_and_allocate(&pml4[pml4_idx], flags);

	uintptr_t *pdpt = GET_VIRT_TABLE(pml4[pml4_idx]);
	if(level == 2) {
		if(pdpt[pdpt_idx] && (!remap || phys != (pdpt[pdpt_idx] & VM_PHYS_MASK))) {
			return false;
		}
		pdpt[pdpt_idx] = phys | flags | PAGE_LARGE;
	} else {
		test_and_allocate(&pdpt[pdpt_idx], flags);
		uintptr_t *pd = GET_VIRT_TABLE(pdpt[pdpt_idx]);

		if(level == 1) {
			if(pd[pd_idx] && (!remap || phys != (pd[pd_idx] & VM_PHYS_MASK))) {
				return false;
			}
			pd[pd_idx] = phys | flags | PAGE_LARGE;
		} else {
			test_and_allocate(&pd[pd_idx], flags);
			uintptr_t *pt = GET_VIRT_TABLE(pd[pd_idx]);
			if(pt[pt_idx] && (!remap || phys != (pt[pt_idx] & VM_PHYS_MASK))) {
				return false;
			}
			pt[pt_idx] = phys | flags;
		}
	}
	return true;
}

bool arch_objspace_map(uintptr_t v, uintptr_t p, int level, uint64_t flags)
{
	uint64_t ef = 0;
	if(flags & OBJSPACE_READ)
		ef |= EPT_READ;
	if(flags & OBJSPACE_WRITE)
		ef |= EPT_WRITE;
	if(flags & OBJSPACE_EXEC_U)
		ef |= EPT_EXEC;

	if(flags & OBJSPACE_WT)
		ef |= EPT_MEMTYPE_WT;
	else if(flags & OBJSPACE_UC)
		ef |= EPT_MEMTYPE_UC;
	else if(flags & OBJSPACE_WC)
		ef |= EPT_MEMTYPE_WC;
	else if(flags & OBJSPACE_WP)
		ef |= EPT_MEMTYPE_WP;
	else
		ef |= EPT_MEMTYPE_WB;
	ef |= EPT_IGNORE_PAT;
	return x86_64_ept_map(
	  current_thread->active_sc->arch.ept_root, v, p, level, ef, flags & OBJSPACE_SET_FLAGS);
}

bool arch_objspace_getmap(uintptr_t v, uintptr_t *p, int *level, uint64_t *flags)
{
	uint64_t ef = 0;
	bool r = x86_64_ept_getmap(current_thread->active_sc->arch.ept_root, v, p, level, &ef);
	if(flags) {
		*flags = 0;
		if(ef & EPT_READ)
			*flags |= OBJSPACE_READ;
		if(ef & EPT_WRITE)
			*flags |= OBJSPACE_WRITE;
		if(ef & EPT_EXEC)
			*flags |= OBJSPACE_EXEC_U;
	}
	return r;
}

uintptr_t init_ept(void)
{
	/* identity map. TODO (major): map all physical memory */
	uintptr_t pml4phys = mm_physical_alloc(0x1000, PM_TYPE_DRAM, true);
	for(uintptr_t phys = 0; phys < 8 * 1024 * 1024 * 1024ull; phys += 2 * 1024ul * 1024) {
		uint64_t flags = EPT_READ | EPT_WRITE | EPT_EXEC;
		/* TODO: this is ugly */
		if((phys >= 0xC0000000 && phys < 0x100000000)) {
			flags |= EPT_MEMTYPE_UC;
		} else {
			flags |= EPT_MEMTYPE_WB;
		}
		x86_64_ept_map(pml4phys, phys, phys, 1, flags, false);
	}

	return pml4phys;
}

void arch_secctx_init(struct sctx *sc)
{
	sc->arch.ept_root = init_ept();
}

void arch_secctx_destroy(struct sctx *sc)
{
	/* TODO (major,mem) */
	(void)sc;
}

__initializer static void __init_ept_root(void)
{
	ept_root = init_ept();
}

void vmexit_point(void);
void vmx_entry_point(void);

void vtx_setup_vcpu(struct processor *proc)
{
	/* we have to set-up the vcpu state to "mirror" our physical CPU.
	 * Strap yourself in, it's gonna be a long ride. */

	/* segment selectors */
	vmcs_writel(VMCS_GUEST_CS_SEL, 0x8);
	vmcs_writel(VMCS_GUEST_CS_BASE, 0);
	vmcs_writel(VMCS_GUEST_CS_LIM, 0xffffffff);
	vmcs_writel(VMCS_GUEST_CS_ARBYTES, 0xA09B);
	vmcs_writel(VMCS_GUEST_ES_SEL, 0x10);
	vmcs_writel(VMCS_GUEST_DS_SEL, 0x10);
	vmcs_writel(VMCS_GUEST_FS_SEL, 0x10);
	vmcs_writel(VMCS_GUEST_GS_SEL, 0x10);
	vmcs_writel(VMCS_GUEST_SS_SEL, 0x10);
	vmcs_writel(VMCS_GUEST_ES_BASE, 0);
	vmcs_writel(VMCS_GUEST_DS_BASE, 0);
	vmcs_writel(VMCS_GUEST_FS_BASE, 0);
	vmcs_writel(VMCS_GUEST_GS_BASE, 0);
	vmcs_writel(VMCS_GUEST_SS_BASE, 0);
	vmcs_writel(VMCS_GUEST_ES_LIM, 0xffff);
	vmcs_writel(VMCS_GUEST_DS_LIM, 0xffff);
	vmcs_writel(VMCS_GUEST_FS_LIM, 0xffff);
	vmcs_writel(VMCS_GUEST_GS_LIM, 0xffff);
	vmcs_writel(VMCS_GUEST_SS_LIM, 0xffff);
	vmcs_writel(VMCS_GUEST_ES_ARBYTES, 0xA093);
	vmcs_writel(VMCS_GUEST_DS_ARBYTES, 0xA093);
	vmcs_writel(VMCS_GUEST_FS_ARBYTES, 0xA093);
	vmcs_writel(VMCS_GUEST_GS_ARBYTES, 0xA093);
	vmcs_writel(VMCS_GUEST_SS_ARBYTES, 0xA093);
	vmcs_writel(VMCS_GUEST_TR_SEL, 0x28);
	vmcs_writel(VMCS_GUEST_TR_BASE, (uintptr_t)&proc->arch.tss);
	vmcs_writel(VMCS_GUEST_TR_LIM, 0xffff);
	vmcs_writel(VMCS_GUEST_TR_ARBYTES, 0x00eb);
	vmcs_writel(VMCS_GUEST_LDTR_SEL, 0);
	vmcs_writel(VMCS_GUEST_LDTR_BASE, 0);
	vmcs_writel(VMCS_GUEST_LDTR_LIM, 0xffff);
	vmcs_writel(VMCS_GUEST_LDTR_ARBYTES, 0x0082);

	uint32_t gslo, gshi;
	x86_64_rdmsr(X86_MSR_GS_BASE, &gslo, &gshi);
	uint64_t gsbase = (uint64_t)gslo | (uint64_t)gshi << 32;
	vmcs_writel(VMCS_GUEST_GS_BASE, gsbase);

	/* GDT and IDT */
	vmcs_writel(VMCS_GUEST_GDTR_BASE, (uintptr_t)&proc->arch.gdt);
	vmcs_writel(VMCS_GUEST_GDTR_LIM, sizeof(struct x86_64_gdt_entry) * 8 - 1);

	vmcs_writel(VMCS_GUEST_IDTR_BASE, (uintptr_t)idt);
	vmcs_writel(VMCS_GUEST_IDTR_LIM, 256 * sizeof(struct idt_entry) - 1);

	/* CPU control info and stack */
	vmcs_writel(VMCS_GUEST_RFLAGS, 0x02);
	vmcs_writel(VMCS_GUEST_RIP, (uintptr_t)vmx_entry_point);
	vmcs_writel(VMCS_GUEST_RSP, (uintptr_t)proc->arch.kernel_stack + KERNEL_STACK_SIZE);

	vmcs_writel(VMCS_GUEST_CR0, read_cr(0));
	vmcs_writel(VMCS_GUEST_CR3, read_cr(3));
	vmcs_writel(VMCS_GUEST_CR4, read_cr(4));
	vmcs_writel(VMCS_GUEST_EFER, read_efer());
	vmcs_writel(VMCS_CR4_READ_SHADOW, 0);
	vmcs_writel(VMCS_CR0_READ_SHADOW, 0);
	vmcs_writel(VMCS_CR4_MASK, 0);
	vmcs_writel(VMCS_CR0_MASK, 0);

	/* TODO (minor): debug registers? */

	vmcs_writel(VMCS_GUEST_ACTIVITY_STATE, 0);
	vmcs_writel(VMCS_GUEST_INTRRUPTIBILITY_INFO, 0);
	vmcs_writel(VMCS_GUEST_PENDING_DBG_EXCEPTIONS, 0);
	vmcs_writel(VMCS_GUEST_IA32_DEBUGCTL, 0);

	/* I/O - allow it all */
	vmcs_writel(VMCS_IO_BITMAP_A, 0);
	vmcs_writel(VMCS_IO_BITMAP_B, 0);

	vmcs_writel(VMCS_LINK_POINTER, ~0ull);

	/* VM control fields. */

	vmcs_write32_fixed(X86_MSR_VMX_TRUE_PINBASED_CTLS, VMCS_PINBASED_CONTROLS, 0);
	vmcs_write32_fixed(X86_MSR_VMX_TRUE_PROCBASED_CTLS,
	  VMCS_PROCBASED_CONTROLS,
	  (1ul << 31) | (1 << 28) /* Use MSR bitmaps */);

	vmcs_write32_fixed(X86_MSR_VMX_PROCBASED_CTLS2,
	  VMCS_PROCBASED_CONTROLS_SECONDARY,
	  (1 << 1)                                        /* EPT */
	    | (1 << 3)                                    /* allow RDTSCP */
	    | (support_ept_switch_vmfunc ? (1 << 13) : 0) /* enable VMFUNC */
	    | (1 << 12)                                   /* allow invpcid */
	    | (support_virt_exception ? (1 << 18) : 0)    /* guest handles EPT violations */
	);

	vmcs_writel(VMCS_EXCEPTION_BITMAP, 0);
	vmcs_writel(VMCS_PF_ERROR_CODE_MASK, 0);
	vmcs_writel(VMCS_PF_ERROR_CODE_MATCH, 0);

	vmcs_writel(VMCS_HOST_CR0, read_cr(0));
	vmcs_writel(VMCS_HOST_CR3, read_cr(3));
	vmcs_writel(VMCS_HOST_CR4, read_cr(4));
	vmcs_writel(VMCS_HOST_EFER, read_efer());

	vmcs_writel(VMCS_HOST_CS_SEL, 0x8);
	vmcs_writel(VMCS_HOST_DS_SEL, 0x10);
	vmcs_writel(VMCS_HOST_ES_SEL, 0x10);
	vmcs_writel(VMCS_HOST_FS_SEL, 0x10);
	vmcs_writel(VMCS_HOST_GS_SEL, 0x10);
	vmcs_writel(VMCS_HOST_SS_SEL, 0x10);
	vmcs_writel(VMCS_HOST_TR_SEL, 0x28);

	vmcs_writel(VMCS_HOST_GS_BASE, gsbase);

	vmcs_writel(VMCS_HOST_GDTR_BASE, (uintptr_t)proc->arch.gdtptr.base);
	vmcs_writel(VMCS_HOST_TR_BASE, (uintptr_t)&proc->arch.tss);
	vmcs_writel(VMCS_HOST_IDTR_BASE, (uintptr_t)idt);

	vmcs_write32_fixed(X86_MSR_VMX_TRUE_EXIT_CTLS, VMCS_EXIT_CONTROLS, (1 << 9) /* IA-32e host */);

	vmcs_write32_fixed(
	  X86_MSR_VMX_TRUE_ENTRY_CTLS, VMCS_ENTRY_CONTROLS, (1 << 9) /* IA-32e guest */);

	vmcs_writel(VMCS_ENTRY_INTR_INFO, 0);
	vmcs_writel(VMCS_APIC_VIRT_ADDR, 0);
	vmcs_writel(VMCS_TPR_THRESHOLD, 0);

	/* we actually have to use these, and they should be all zero (none owned by host) */
	vmcs_writel(VMCS_MSR_BITMAPS_ADDR, (uintptr_t)mm_physical_alloc(0x1000, PM_TYPE_DRAM, true));

	if(support_ept_switch_vmfunc) {
		vmcs_writel(VMCS_VMFUNC_CONTROLS, 1 /* enable EPT-switching */);
		proc->arch.eptp_list = (void *)mm_virtual_alloc(0x1000, PM_TYPE_DRAM, true);
		proc->arch.eptp_list[0] = ept_root;
		vmcs_writel(VMCS_EPTP_LIST, mm_vtop(proc->arch.eptp_list));
	}

	/* TODO (minor): veinfo needs to be page-aligned, but we're over-allocating here */
	proc->arch.veinfo = (void *)mm_virtual_alloc(0x1000, PM_TYPE_DRAM, true);
	if(support_virt_exception) {
		vmcs_writel(VMCS_EPTP_INDEX, 0);
		vmcs_writel(VMCS_VIRT_EXCEPTION_INFO_ADDR, mm_vtop(proc->arch.veinfo));
	}

	vmcs_writel(VMCS_HOST_RIP, (uintptr_t)vmexit_point);
	vmcs_writel(VMCS_HOST_RSP, (uintptr_t)proc->arch.vcpu_state_regs);

	/* TODO (minor): these numbers probably do something useful. */
	vmcs_writel(VMCS_EPT_PTR, (uintptr_t)ept_root | (3 << 3) | 6);
	proc->arch.veinfo->lock = 0;
}

void x86_64_start_vmx(struct processor *proc)
{
	x86_64_enable_vmx();

	proc->arch.launched = 0;
	memset(proc->arch.vcpu_state_regs, 0, sizeof(proc->arch.vcpu_state_regs));
	proc->arch.hyper_stack = (void *)mm_virtual_alloc(KERNEL_STACK_SIZE, PM_TYPE_DRAM, true);
	proc->arch.vcpu_state_regs[HOST_RSP] = (uintptr_t)proc->arch.hyper_stack + KERNEL_STACK_SIZE;
	proc->arch.vcpu_state_regs[PROC_PTR] = (uintptr_t)proc;
	/* set initial argument to vmx_entry_point */
	proc->arch.vcpu_state_regs[REG_RDI] = (uintptr_t)proc;
	proc->arch.vmcs = mm_physical_alloc(VMCS_SIZE, PM_TYPE_DRAM, true);
	uint32_t *vmcs_rev = (uint32_t *)mm_ptov(proc->arch.vmcs);
	*vmcs_rev = revision_id & 0x7FFFFFFF;

	uint8_t error;
	asm volatile("vmptrld (%%rax); setna %0" : "=g"(error) : "a"(&proc->arch.vmcs) : "cc");
	if(error) {
		panic("failed to load VMCS region: %s", vmcs_read_vminstr_err());
	}

	vtx_setup_vcpu(proc);
	x86_64_vmenter(proc);
	panic("error occurred during vmlaunch");
}

static long x86_64_rootcall(long fn, long a0, long a1, long a2)
{
	long ret;
	asm volatile("vmcall" : "=a"(ret) : "D"(fn), "S"(a0), "d"(a1), "c"(a2) : "memory");
	return ret;
}

void x86_64_switch_ept(uintptr_t root)
{
	if(support_ept_switch_vmfunc) {
		int index = -1;
		/* TODO (perf): better than just a loop, man! */
		for(int i = 0; i < 512; i++) {
			if(current_processor->arch.eptp_list[i] == root) {
				index = i;
				break;
			}
		}
		if(index != -1) {
			asm volatile("vmfunc" ::"a"(VM_FUNCTION_SWITCH_EPTP), "c"(index) : "memory");
		} else {
			x86_64_rootcall(VMX_RC_SWITCHEPT, root, 0, 0);
			/* TODO (perf): add to trusted list */
		}
	} else {
		x86_64_rootcall(VMX_RC_SWITCHEPT, root, 0, 0);
	}
}

void x86_64_secctx_switch(struct sctx *s)
{
	if(s) {
		x86_64_switch_ept(s->arch.ept_root);
	} else {
		x86_64_switch_ept(ept_root);
	}
}

void x86_64_virtualization_fault(struct processor *proc)
{
	uint32_t flags = 0;
	if(proc->arch.veinfo->qual & EQ_EPTV_READ) {
		flags |= OBJSPACE_FAULT_READ;
	}
	if(proc->arch.veinfo->qual & EQ_EPTV_WRITE) {
		flags |= OBJSPACE_FAULT_WRITE;
	}
	if(proc->arch.veinfo->qual & EQ_EPTV_EXEC) {
		flags |= OBJSPACE_FAULT_EXEC;
	}

	kernel_objspace_fault_entry(current_thread->arch.exception.rip,
	  proc->arch.veinfo->physical,
	  proc->arch.veinfo->linear,
	  flags);

	proc->arch.veinfo->lock = 0;
}
