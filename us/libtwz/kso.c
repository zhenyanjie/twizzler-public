#include <stdarg.h>
#include <stdio.h>
#include <twz/_kso.h>
#include <twz/obj.h>
#include <twz/persist.h>
#include <twz/thread.h>

int kso_set_name(twzobj *obj, const char *name, ...)
{
	va_list va;
	va_start(va, name);
	struct kso_hdr *hdr = obj ? twz_object_base(obj) : twz_thread_repr_base();
	int r = vsnprintf(hdr->name, KSO_NAME_MAXLEN, name, va);
	if(r < 0) {
		return r;
	}
	_clwb_len(hdr->name, r + 1);
	_pfence();
	va_end(va);
	return r;
}
