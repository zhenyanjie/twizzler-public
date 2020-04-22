#pragma once

#include <arch/secctx.h>
#include <krc.h>
#include <object.h>

#include <twz/_objid.h>

struct sctx {
	struct object_space space;
	struct object *obj;
	struct krc refs;
	bool superuser;
};

void arch_secctx_init(struct sctx *sc);
void arch_secctx_destroy(struct sctx *sc);

struct sctx *secctx_alloc(struct object *);
void secctx_free(struct sctx *s);
void secctx_switch(int i);
struct thread;
int secctx_fault_resolve(struct thread *t,
  uintptr_t ip,
  uintptr_t loaddr,
  uintptr_t vaddr,
  objid_t target,
  uint32_t flags,
  uint64_t *perms);
struct object;
int secctx_check_permissions(struct thread *t, uintptr_t ip, struct object *to, uint64_t flags);
