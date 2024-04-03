#include <linux/module.h>
#include <linux/kthread.h>
#include <linux/hazptr.h>

static void simple_func(struct callback_head *head)
{
	printk("callback called %px\n", head);
	kfree(head);
}

static void simple(void)
{
	struct hazptr_context ctx;
	struct callback_head *dummy, *tmp, *other;
	hazptr_t *hptr;
	hazptr_t *hptr2;

	dummy = kzalloc(sizeof(*dummy), GFP_KERNEL);
	other = kzalloc(sizeof(*dummy), GFP_KERNEL);

	if (!dummy || !other) {
		printk("allocation failed, skip test\n");
		return;
	}

	init_hazptr_context(&ctx);
	hptr = hazptr_alloc(&ctx);
	BUG_ON(!hptr);

	// Get a second hptr.
	hptr2 = hazptr_alloc(&ctx);
	BUG_ON(!hptr2);

	// No one is modifying 'dummy', protection must succeed.
	BUG_ON(!__hazptr_tryprotect(hptr, (void **)&dummy, 0));

	// Simulate changing a global pointer.
	tmp = dummy;
	WRITE_ONCE(dummy, other);

	// Callback will run after no active readers.
	printk("callback added, %px\n", tmp);
	call_hazptr(tmp, simple_func);

	// Simulate changing a global pointer.
	tmp = dummy;
	WRITE_ONCE(dummy, other);

	// No one is modifying 'dummy', protection must succeed.
	BUG_ON(!__hazptr_tryprotect(hptr2, (void **)&dummy, 0));

	// The above callback should run after this.
	hazptr_clear(hptr);
	printk("first reader is out\n");

	for (int i = 0; i < 10; i++)
		schedule(); // yield a few times.

	printk("callback added, %px\n", tmp);
	call_hazptr(tmp, simple_func);

	cleanup_hazptr_context(&ctx);
	printk("no reader here\n");

	for (int i = 0; i < 10; i++)
		schedule(); // yield a few times.
}

static int hazptr_test(void)
{
	simple();
	printk("hello hazptr\n");
	return 0;
}
module_init(hazptr_test);
