#pragma once

#include <spinlock.h>

#define LINKEDLIST_LOCKLESS 1
struct linkedlist;
struct linkedentry {
	void *obj;
	struct linkedlist *list;
	struct linkedentry *next, *prev;
};

#define linkedentry_obj(entry) ((entry) ? (entry)->obj : NULL)

struct linkedlist {
	struct linkedentry * _Atomic head;
	struct linkedentry sentry;
	struct spinlock lock;
	_Atomic ssize_t count;
	int flags;
	bool slflag;
};

#define linkedlist_iter_end(list) &(list)->sentry
#define linkedlist_iter_start(list) (list)->head->next
#define linkedlist_iter_next(entry) (entry)->next

#define linkedlist_back_iter_end(list) &(list)->sentry
#define linkedlist_back_iter_start(list) (list)->head->prev
#define linkedlist_back_iter_next(entry) (entry)->prev

static inline void linkedlist_lock(struct linkedlist *list)
{
	if(likely(!(list->flags & LINKEDLIST_LOCKLESS))) {
		list->slflag = spinlock_acquire(&list->lock);
	}
}

static inline void linkedlist_unlock(struct linkedlist *list)
{
	if(likely(!(list->flags & LINKEDLIST_LOCKLESS))) {
		spinlock_release(&list->lock, list->slflag);
	}
}

void *linkedlist_head(struct linkedlist *list);
void *linkedlist_remove_head(struct linkedlist *list);
void *linkedlist_remove_tail(struct linkedlist *list);
void linkedlist_create(struct linkedlist *list, int flags);
void linkedlist_insert(struct linkedlist *list, struct linkedentry *entry, void *obj);
void linkedlist_remove(struct linkedlist *list, struct linkedentry *entry);
void linkedlist_insert_sorted(struct linkedlist *list, int (*compar)(void *, void *), struct linkedentry *entry, void *obj);
void __linkedlist_remove_unlocked(struct linkedlist *list, struct linkedentry *entry);
struct linkedentry *linkedlist_find(struct linkedlist *list, bool (*fn)(struct linkedentry *, void *data), void *data);

