#pragma once
#define X86_MSR_APIC_BASE 0x1B
  #define X86_MSR_APIC_BASE_BSP    0x100
  #define X86_MSR_APIC_BASE_X2MODE 0x400
  #define X86_MSR_APIC_BASE_ENABLE 0x800

#define X86_MSR_FS_BASE             0xc0000100
#define X86_MSR_GS_BASE             0xc0000101
#define X86_MSR_KERNEL_GS_BASE      0xc0000102
#define X86_MSR_EFER                0xc0000080
   #define X86_MSR_EFER_SYSCALL     0x1

#define X86_MSR_STAR                0xC0000081
#define X86_MSR_LSTAR               0xC0000082
#define X86_MSR_SFMASK              0xC0000084

static inline void x86_64_rdmsr(uint32_t msr, uint32_t *lo, uint32_t *hi)
{
	asm volatile("rdmsr" : "=a"(*lo), "=d"(*hi) : "c"(msr));
}
 
static inline void x86_64_wrmsr(uint32_t msr, uint32_t lo, uint32_t hi)
{
	asm volatile("wrmsr" : : "a"(lo), "d"(hi), "c"(msr));
}

