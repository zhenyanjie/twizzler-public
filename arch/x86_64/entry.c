#include <processor.h>
#include <thread.h>
#include <syscall.h>
#include <arch/x86_64-msr.h>
#include <arch/x86_64-vmx.h>
#include <secctx.h>

void x86_64_signal_eoi(void);

void x86_64_ipi_tlb_shootdown(void)
{
	asm volatile("mov %%cr3, %%rax; mov %%rax, %%cr3; mfence;" ::: "memory", "rax");
	processor_ipi_finish();
	x86_64_signal_eoi();
}

void x86_64_ipi_resume(void)
{
	printk("%d: RESUME\n", current_processor->id);
	x86_64_signal_eoi();
}

void x86_64_ipi_halt(void)
{
	processor_shutdown();
	asm volatile("cli; jmp .;" ::: "memory");
}

static void x86_64_change_fpusse_allow(bool enable)
{
	register uint64_t tmp;
	asm volatile("mov %%cr0, %0" : "=r"(tmp));
	tmp = enable ? (tmp & ~(1 << 2)) : (tmp | (1 << 2));
	asm volatile("mov %0, %%cr0" :: "r"(tmp));
	asm volatile("mov %%cr4, %0" : "=r"(tmp));
	tmp = enable ? (tmp | (1 << 9)) : (tmp & ~(1 << 9));
	asm volatile("mov %0, %%cr4" :: "r"(tmp));
}

__noinstrument
void x86_64_exception_entry(struct x86_64_exception_frame *frame, bool was_userspace)
{
	//if(frame->int_no != 36 && frame->int_no != 32) {
	//	printk(":: %ld\n", frame->int_no);
	//}
	if(was_userspace) {
		current_thread->arch.was_syscall = false;
	}
	
	if((frame->int_no == 6 || frame->int_no == 7) && current_thread
				&& !current_thread->arch.usedfpu) {
		if(!was_userspace) {
			panic("floating-point operations used in kernel-space");
		}
		/* we're emulating FPU instructions / disallowing SSE. Set a flag,
		 * and allow the thread to do its thing */
		current_thread->arch.usedfpu = true;
		x86_64_change_fpusse_allow(true);
		asm volatile ("finit"); /* also, we may need to clear the FPU state */
	} else if(frame->int_no == 14) {
			/* page fault */
			uint64_t cr2;
			asm volatile("mov %%cr2, %0" : "=r"(cr2) :: "memory");
			int flags = 0;
			if(frame->err_code & 1) {
				flags |= FAULT_ERROR_PERM;
			} else {
				flags |= FAULT_ERROR_PRES;
			}
			if(frame->err_code & (1 << 1)) {
				flags |= FAULT_WRITE;
			}
			if(frame->err_code & (1 << 2)) {
				flags |= FAULT_USER;
			}
			if(frame->err_code & (1 << 4)) {
				flags |= FAULT_EXEC;
			}
			if(!was_userspace) {
				panic("kernel-mode page fault to address %lx\n"
						"    from %lx: %s while in %s-mode: %s\n", cr2, frame->rip,
					flags & FAULT_EXEC ? "ifetch" : (flags & FAULT_WRITE ? "write" : "read"),
					flags & FAULT_USER ? "user" : "kernel",
					flags & FAULT_ERROR_PERM ? "present" : "non-present");
			}
			kernel_fault_entry(frame->rip, cr2, flags);
	} else if(frame->int_no == 20) {
		/* #VE */
		x86_64_virtualization_fault(current_processor);
	} else if(frame->int_no < 32) {
		if(was_userspace) {
			struct fault_exception_info info = {
				.ip = frame->rip,
				.code = frame->int_no,
				.arg0 = frame->err_code,
			};
			thread_raise_fault(current_thread, FAULT_EXCEPTION, &info, sizeof(info));
		} else {
			if(frame->int_no == 3) {
				printk("[debug]: recv debug interrupt\n");
				kernel_debug_entry();
			}
			panic("kernel exception: %ld, from %lx\n", frame->int_no, frame->rip);
		}
	} else {
		kernel_interrupt_entry(frame->int_no);
	}

	x86_64_signal_eoi();
	if(was_userspace) {
		thread_schedule_resume();
	}
	/* if we aren't in userspace, we just return and the kernel_exception handler will
	 * properly restore the frame and continue */
}

extern long (*syscall_table[])();
__noinstrument
void x86_64_syscall_entry(struct x86_64_syscall_frame *frame)
{
	current_thread->arch.was_syscall = true;
	arch_interrupt_set(true);
#if CONFIG_PRINT_SYSCALLS
	if(frame->rax != SYS_DEBUG_PRINT)
		printk("%ld: SYSCALL %ld (%lx)\n", current_thread->id, frame->rax, frame->rcx);
#endif
	if(frame->rax < NUM_SYSCALLS) {
		frame->rax = syscall_table[frame->rax](frame->rdi, frame->rsi, frame->rdx, frame->r8, frame->r9, frame->r10);
		frame->rdx = 0;
	} else {
		frame->rax = -EINVAL;
		frame->rdx = 0;
	}
	
	arch_interrupt_set(false);
	thread_schedule_resume();
}

extern void x86_64_resume_userspace(void *);
extern void x86_64_resume_userspace_interrupt(void *);
__noinstrument
void arch_thread_resume(struct thread *thread)
{
	//printk("resume %ld\n", thread->id);
	struct thread *old = current_thread;
	thread->processor->arch.curr = thread;
	thread->processor->arch.tcb = (void *)((uint64_t)&thread->arch.syscall + sizeof(struct x86_64_syscall_frame));
	thread->processor->arch.tss.esp0 = ((uint64_t)&thread->arch.exception + sizeof(struct x86_64_exception_frame));

	/* restore segment bases for new thread */
	x86_64_wrmsr(X86_MSR_FS_BASE, thread->arch.fs & 0xFFFFFFFF, (thread->arch.fs >> 32) & 0xFFFFFFFF);
	x86_64_wrmsr(X86_MSR_KERNEL_GS_BASE, thread->arch.gs & 0xFFFFFFFF, (thread->arch.gs >> 32) & 0xFFFFFFFF);

	/* TODO (sec): remove lazy FPU switching */
	/* TODO (perf): we can be smarter about FPU state:
	 *  - we should allocate a separate (slab-based) structure to hold
	 *    FPU state, making thread allocations cheaper.
	 *  - we can store more info than "did thread use FPU",
	 *    such as how long has it been since it was used... and
	 *    then we can 're-disable' the FPU for that thread, and possibly
	 *    even free the allocated FPU state space.
	 */
	if(old && old->arch.usedfpu) {
		/* store FPU state */
		asm volatile ("fxsave (%0)" 
				:: "r" (old->arch.fpu_data) : "memory");
	}

	x86_64_change_fpusse_allow(thread->arch.usedfpu);
	if(thread->arch.usedfpu) {
		/* restore FPU state */
		asm volatile ("fxrstor (%0)" 
				:: "r" (thread->arch.fpu_data) : "memory");
	}
	if((!old || old->ctx != thread->ctx) && thread->ctx) {
		arch_mm_switch_context(thread->ctx);
	}
	if((!old || old->active_sc != thread->active_sc) && thread->active_sc) {
		x86_64_secctx_switch(thread->active_sc);
	}
	if(thread->arch.was_syscall) {
		x86_64_resume_userspace(&thread->arch.syscall);
	} else {
		x86_64_resume_userspace_interrupt(&thread->arch.exception);
	}
}

