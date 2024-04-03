// SPDX-License-Identifier: GPL-2.0+

#include <linux/spinlock.h>
#include <linux/cleanup.h>
#include <linux/hazptr.h>
#include <linux/percpu.h>
#include <linux/workqueue.h>

struct hazptr_context_list {
	struct list_head list;
	spinlock_t lock;
};

DEFINE_PER_CPU(struct hazptr_context_list, hzc_list);

void init_hazptr_context(struct hazptr_context *hzcp)
{
	struct hazptr_context_list *this_hzc_list = this_cpu_ptr(&hzc_list);

	for (int i = 0; i < HAZPTR_SLOT_PER_CTX; i++) {
		hzcp->slots[i] = HAZPTR_UNUSED;
	}

	guard(spinlock)(&this_hzc_list->lock);
	list_add(&hzcp->list, &this_hzc_list->list);
	hzcp->lock = &this_hzc_list->lock;
}

void cleanup_hazptr_context(struct hazptr_context *hzcp)
{
	if (hzcp->lock) {
		guard(spinlock)(hzcp->lock);
		list_del(&hzcp->list);
		hzcp->lock = NULL;
	}
}

hazptr_t *hazptr_alloc(struct hazptr_context *hzcp)
{
	for (int i = 0; i < HAZPTR_SLOT_PER_CTX; i++) {
		if (READ_ONCE(hzcp->slots[i]) == HAZPTR_UNUSED) {
			if (cmpxchg_relaxed(&hzcp->slots[i], HAZPTR_UNUSED, (unsigned long)NULL) == HAZPTR_UNUSED) {
				return (hazptr_t *)&hzcp->slots[i];
			}
		}
	}

	return NULL;
}

void hazptr_free(struct hazptr_context *hzcp, hazptr_t *hzp)
{
	WARN_ON(((unsigned long)*hzp) == HAZPTR_UNUSED);

	WRITE_ONCE(*hzp, (void *)HAZPTR_UNUSED);
}

struct hazptr_struct {
	struct work_struct work;
	bool scheduled;
	struct callback_head *queued;
	spinlock_t lock;

};

struct hazptr_struct hazptr_struct;

static void check_readers_range(unsigned long *start, unsigned long *end)
{
	int cpu;

	*start = ULONG_MAX;
	*end = 0;

	for_each_possible_cpu(cpu) {
		struct hazptr_context_list *hzctxs = per_cpu_ptr(&hzc_list, cpu);
		struct hazptr_context *ctx;

		guard(spinlock)(&hzctxs->lock);
		list_for_each_entry(ctx, &hzctxs->list, list) {
			for (int i = 0; i < HAZPTR_SLOT_PER_CTX; i++) {
				unsigned long slot = READ_ONCE(ctx->slots[i]);

				if (slot == HAZPTR_UNUSED || slot == (unsigned long)NULL)
					continue;
				if (slot < *start)
					*start = slot;
				if (slot > *end)
					*end = slot;
			}
		}
	}
}

static void kick_hazptr_work(void)
{
	if (hazptr_struct.scheduled)
		return;

	queue_work(system_wq, &hazptr_struct.work);
	hazptr_struct.scheduled = true;
}

/*
 * Check if which callbacks are ready to call.
 *
 * Return: a callback list that no reader is referencing the corresponding
 * objects.
 */
static struct callback_head *do_hazptr(struct hazptr_struct *hzst)
{
	struct callback_head *new, *tmp;
	struct callback_head **curr = &hzst->queued;
	unsigned long start;
	unsigned long end;

	check_readers_range(&start, &end);

	// Find the first callback whose address is in the reader range.
	while ((tmp = *curr) && (unsigned long)tmp < start)
		curr = &tmp->next;

	// All callbacks are out of the reader range, return the whole list.
	if (tmp == NULL) {
		// reuse 'tmp' for return value.
		tmp = hzst->queued;
		hzst->queued = NULL;
		return tmp;
	}

	// Point 'new' to the first callback.
	new = tmp;

	// Find the last callback whose address is in the reader range.
	while (tmp->next && (unsigned long)tmp->next <= end)
		tmp = tmp->next;

	// List from 'new' to 'tmp' are callbacks within the reader range.
	// 'curr' points to the locatioin of 'new'.

	// Remove list from 'new' to 'tmp'.
	*curr = tmp->next;
	tmp->next = NULL;

	// reuse 'tmp' for return value.
	tmp = hzst->queued;
	hzst->queued = new;

	kick_hazptr_work();
	return tmp;
}

static void hazptr_work_func(struct work_struct *work)
{
	struct hazptr_struct *hzst = container_of(work, struct hazptr_struct, work);
	struct callback_head *todo, *next;
	void (*func)(struct callback_head*);

	scoped_guard(spinlock, &hzst->lock) {
		hzst->scheduled = false;
		todo = do_hazptr(hzst);
	}

	while (todo) {
		next = todo->next;
		todo->next = NULL;
		func = todo->func;
		func(todo);
		todo = next;
	}
}

void call_hazptr(struct callback_head *head, rcu_callback_t func)
{
	struct callback_head **curr;
	struct callback_head *tmp;

	smp_mb(); // pairs with the smp_mb() on the reader side.

	head->func = func;
	head->next = NULL;

	guard(spinlock)(&hazptr_struct.lock);

	curr = &hazptr_struct.queued;

	while ((tmp = *curr)) {
		if (((unsigned long)tmp) < ((unsigned long)head)) {
			curr = &tmp->next;
		}
	}

	*curr = head;
	head->next = tmp;

	kick_hazptr_work();
}

static int init_hazptr_struct(void)
{
	int cpu;

	INIT_WORK(&hazptr_struct.work, hazptr_work_func);
	spin_lock_init(&hazptr_struct.lock);

	for_each_possible_cpu(cpu) {
		struct hazptr_context_list *hzctxs = per_cpu_ptr(&hzc_list, cpu);
		spin_lock_init(&hzctxs->lock);
		INIT_LIST_HEAD(&hzctxs->list);
	}

	return 0;
}
early_initcall(init_hazptr_struct);
