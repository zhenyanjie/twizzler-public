#pragma once

#include <interrupt.h>
#include <processor.h>

void instrument_disable(void);
void instrument_enable(void);

void instrument_print_results(void);

struct instr {
	const char *name;
	unsigned long long start, end;
};

static inline void __instr_end(struct instr *in)
{
	in->end = arch_processor_timestamp();
	printk("{instr %s: %lld %lld}\n", in->name, in->start, in->end);
}

#define instr_start(_name) \
	struct instr __concat(__instr_, _name) = { \
		.start = arch_processor_timestamp(), \
		.name = stringify(_name), \
	}; defer(__instr_end, & __concat(__instr_, _name));


