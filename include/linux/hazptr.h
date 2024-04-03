#include <linux/list.h>
#include <linux/spinlock.h>

typedef void* hazptr_t;

#define HAZPTR_UNUSED (1ul)
#define HAZPTR_SLOT_PER_CTX 8

struct hazptr_context {
	// The lock of the percpu context lists.
	spinlock_t *lock;

	struct list_head list;
	____cacheline_aligned unsigned long slots[HAZPTR_SLOT_PER_CTX];
};

void init_hazptr_context(struct hazptr_context *hzcp);
void cleanup_hazptr_context(struct hazptr_context *hzcp);
hazptr_t *hazptr_alloc(struct hazptr_context *hzcp);
void hazptr_free(struct hazptr_context *hzcp, hazptr_t *hzp);

#define hazptr_tryprotect(hzp, p, field)	__hazptr_tryprotect(hzp, p, offset_of(__typeof__(*p), field)

static inline bool __hazptr_tryprotect(hazptr_t *hzp, void **p, unsigned long head_offset)
{
	void *ptr;
	struct callback_head *head;

	ptr = READ_ONCE(*p);

	if (ptr == NULL)
		return false;

	head = (struct callback_head *)ptr + head_offset;

	WRITE_ONCE(*hzp, head);
	smp_mb();

	ptr = READ_ONCE(*p); // read again

	if (ptr + head_offset != *hzp) { // pointer changed
		WRITE_ONCE(*hzp, NULL);  // reset hazard pointer
		return false;
	} else
		return true;
}

static inline void hazptr_clear(hazptr_t *hzp)
{
	WRITE_ONCE(*hzp, NULL);
}

void call_hazptr(struct callback_head *head, rcu_callback_t func);
