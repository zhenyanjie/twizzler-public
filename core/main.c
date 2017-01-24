#include <debug.h>
#include <memory.h>
#include <init.h>
#include <arena.h>
#include <processor.h>
#include <time.h>

static struct arena post_init_call_arena;
static struct init_call *post_init_call_head = NULL;

void post_init_call_register(void (*fn)(void *), void *data)
{
	if(post_init_call_head == NULL) {
		arena_create(&post_init_call_arena);
	}

	struct init_call *ic = arena_allocate(&post_init_call_arena, sizeof(struct init_call));
	ic->fn = fn;
	ic->data = data;
	ic->next = post_init_call_head;
	post_init_call_head = ic;
}

static void post_init_calls_execute(void)
{
	for(struct init_call *call = post_init_call_head; call != NULL; call = call->next) {
		call->fn(call->data);
	}
	arena_destroy(&post_init_call_arena);
	post_init_call_head = NULL;
}

/* functions called from here expect virtual memory to be set up. However, functions
 * called from here cannot rely on global contructors having run, as those are allowed
 * to use memory management routines, so they are run after this.
 */
void kernel_early_init(void)
{
	mm_init();
}

/* at this point, memory management, interrupt routines, global constructors, and shared
 * kernel state between nodes have been initialized. Now initialize all application processors
 * and per-node threading.
 */

long _syscall(long num, long a1, long a2, long a3, long a4, long a5, long a6)
{
	register long r8 asm("r10") = a4;
	register long r9 asm("r10") = a5;
	register long r10 asm("r10") = a6;
	long ret;
	asm volatile("syscall" : "=a"(ret) : "a"(num), "D"(a1), "S"(a2), "d"(a3), "r"(r8), "r"(r9), "r"(r10) : "rcx", "r11", "memory");
	return ret;
}

void user_test(void *arg)
{
	for(;;)
		_syscall((long)arg, 1, 2, 3, 4, 5, 6);
}

struct thread t1, t2, t3, t4;
char us1[0x1000];
char us2[0x1000];
char us3[0x1000];
char us4[0x1000];

void kernel_init(void)
{
	post_init_calls_execute();
	processor_perproc_init(NULL);
}

void kernel_main(struct processor *proc)
{
	printk("processor %ld reached resume state\n", proc->id);
	
	t1.id = 1;
	t2.id = 2;
	arch_thread_init(&t1, user_test, (void *)1, us1 + 0x1000);
	arch_thread_init(&t2, user_test, (void *)2, us2 + 0x1000);
	arch_thread_init(&t3, user_test, (void *)3, us3 + 0x1000);
	arch_thread_init(&t4, user_test, (void *)4, us4 + 0x1000);

	if(proc->flags & PROCESSOR_BSP) {
		processor_attach_thread(proc, &t1);
		processor_attach_thread(proc, &t2);
		processor_attach_thread(proc, &t3);
	} else {
		processor_attach_thread(proc, &t4);
	}

	thread_schedule_resume_proc(proc);
}

