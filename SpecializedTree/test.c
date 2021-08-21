// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2021 BitDefender
 * Written by Mircea Cirjaliu
 */

#define pr_fmt(fmt)	KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/printk.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/debugfs.h>
#include <linux/stat.h>
#include <linux/uaccess.h>
#include <linux/string.h>
#include <linux/spinlock.h>
#include <linux/fault-inject.h>

#include "test.h"

// the object we test
static struct avlrcu_root avlrcu_range;
static DEFINE_SPINLOCK(lock);

// thread control
static struct task_struct *validator;

// access files
static struct dentry *debugfs_dir;

#ifdef CONFIG_FAULT_INJECTION
DECLARE_FAULT_ATTR(avlrcu_fault_attr);
#endif /* CONFIG_FAULT_INJECTION */

static struct avlrcu_node *test_alloc(void)
{
	struct test_avlrcu_node *container;

#ifdef CONFIG_FAULT_INJECTION
	/* trigger allocation failure */
	if (should_fail(&avlrcu_fault_attr, sizeof(struct test_avlrcu_node)))
		return NULL;
#endif /* CONFIG_FAULT_INJECTION */

	container = kzalloc(sizeof(struct test_avlrcu_node), GFP_ATOMIC);
	if (!container)
		return NULL;

	return &container->node;
}

static void test_free(struct avlrcu_node *node)
{
	struct test_avlrcu_node *container;
	container = avlrcu_entry(node, struct test_avlrcu_node, node);

	kfree(container);
}

static void test_free_rcu(struct avlrcu_node *node)
{
	struct test_avlrcu_node *container;
	container = avlrcu_entry(node, struct test_avlrcu_node, node);

	kfree_rcu(container, node.rcu);
}

static int test_cmp(const struct avlrcu_node *match, const struct avlrcu_node *crnt)
{
	struct test_avlrcu_node *container_match;
	struct test_avlrcu_node *container_crnt;

	container_match = avlrcu_entry(match, struct test_avlrcu_node, node);
	container_crnt = avlrcu_entry(crnt, struct test_avlrcu_node, node);

	// watch out for truncation !!
	return (int)((long)container_match->address - (long)container_crnt->address);
}

static void test_copy(struct avlrcu_node *to, struct avlrcu_node *from)
{
	struct test_avlrcu_node *container_to;
	struct test_avlrcu_node *container_from;

	container_to = avlrcu_entry(to, struct test_avlrcu_node, node);
	container_from = avlrcu_entry(from, struct test_avlrcu_node, node);

	memcpy(container_to, container_from, sizeof(struct test_avlrcu_node));
}

static struct avlrcu_ops test_ops = {
	.alloc = test_alloc,
	.free = test_free,
	.free_rcu = test_free_rcu,
	.cmp = test_cmp,
	.copy = test_copy,
};

static int prev_count = 0;
static void validate_greater(struct avlrcu_root *root)
{
	struct test_avlrcu_node *container;
	unsigned long prev;
	int count;
	int result = 0;

	/* these have to match with the allocation functions */
	rcu_read_lock();

	prev = 0;
	count = 0;
	avlrcu_for_each_entry(container, root, node) {
		if (prev >= container->address) {
			result = -EINVAL;
			break;
		}

		prev = container->address;
		count++;
	}

	rcu_read_unlock();

	if (result) {
		pr_err("%s: invalid order detected\n", __func__);
		return;
	}

	if (count > prev_count) {
		pr_debug("%s: found %d elements > %d\n", __func__, count, prev_count);
		prev_count = count;
	}

}

static int validator_func(void *arg)
{
	pr_debug("validator started\n");

	do {
		// validate each element is greater than the last
		validate_greater(&avlrcu_range);

		msleep_interruptible(10);

	} while (!kthread_should_stop());

	pr_debug("validator stopped\n");

	return 0;
}

static ssize_t insert_map(struct file *file, const char __user *data, size_t count, loff_t *offs)
{
	unsigned long value;
	struct test_avlrcu_node *container;
	int result;

	result = kstrtoul_from_user(data, count, 16, &value);
	if (IS_ERR_VALUE((long)result))
		return result;

	pr_debug("%s: at %lx\n", __func__, value);

	container = kzalloc(sizeof(struct test_avlrcu_node), GFP_ATOMIC);
	if (!container)
		return -ENOMEM;
	container->address = value;

	/* these have to match with the allocation functions */
	spin_lock(&lock);

	result = avlrcu_insert(&avlrcu_range, &container->node);

	spin_unlock(&lock);

	if (result == 0)
		pr_debug("%s: success\n", __func__);
	else
		pr_err("%s: failed: %d\n", __func__, result);
	pr_debug("-\n");

	*offs += count;
	return count;
}

static ssize_t delete_map(struct file *file, const char __user *data, size_t count, loff_t *offs)
{
	struct test_avlrcu_node match;
	struct avlrcu_node *node;
	struct test_avlrcu_node *container;
	int result;

	result = kstrtoul_from_user(data, count, 16, &match.address);
	if (IS_ERR_VALUE((long)result))
		return result;

	pr_debug("%s: at %lx\n", __func__, match.address);

	/* these have to match with the allocation functions */
	spin_lock(&lock);

	node = avlrcu_delete(&avlrcu_range, &match.node);

	spin_unlock(&lock);

	if (!IS_ERR(node)) {
		container = avlrcu_entry(node, struct test_avlrcu_node, node);
		kfree_rcu(container, node.rcu);
		// or
		//synchronize_rcu();
		// cleanup payload of container...
		//kfree(container);
	}
	else
		result = (int)PTR_ERR(node);

	if (result == 0)
		pr_debug("%s: success\n", __func__);
	else
		pr_err("%s: failed: %d\n", __func__, result);
	pr_debug("-\n");

	*offs += count;
	return count;
}

static ssize_t unwind_map(struct file *file, const char __user *data, size_t count, loff_t *offs)
{
	struct test_avlrcu_node match;
	int result;

	result = kstrtoul_from_user(data, count, 16, &match.address);
	if (IS_ERR_VALUE((long)result))
		return result;

	pr_debug("%s: at %lx\n", __func__, match.address);

	/* these have to match with the allocation functions */
	spin_lock(&lock);

	result = avlrcu_test_unwind(&avlrcu_range, &match.node);

	spin_unlock(&lock);

	if (result == 0)
		pr_debug("%s: success\n", __func__);
	else
		pr_err("%s: failed: %d\n", __func__, result);
	pr_debug("-\n");

	*offs += count;
	return count;
}

static ssize_t clear_map(struct file *file, const char __user *data, size_t count, loff_t *offs)
{
	/* these have to match with the allocation functions */
	spin_lock(&lock);

	avlrcu_free(&avlrcu_range);
	prev_count = 0;	/* reset validator counter */

	spin_unlock(&lock);

	*offs += count;
	return count;
}

static ssize_t ror_map(struct file *file, const char __user *data, size_t count, loff_t *offs)
{
	struct test_avlrcu_node match;
	int result;

	result = kstrtoul_from_user(data, count, 16, &match.address);
	if (IS_ERR_VALUE((long)result))
		return result;

	pr_debug("%s: at %lx\n", __func__, match.address);

	/* these have to match with the allocation functions */
	spin_lock(&lock);

	result = avlrcu_test_ror(&avlrcu_range, &match.node);

	spin_unlock(&lock);

	if (result == 0)
		pr_debug("%s: success\n", __func__);
	else
		pr_err("%s: failed: %d\n", __func__, result);
	pr_debug("-\n");

	*offs += count;
	return count;
}

static ssize_t rol_map(struct file *file, const char __user *data, size_t count, loff_t *offs)
{
	struct test_avlrcu_node match;
	int result;

	result = kstrtoul_from_user(data, count, 16, &match.address);
	if (IS_ERR_VALUE((long)result))
		return result;

	pr_debug("%s: at %lx\n", __func__, match.address);

	/* these have to match with the allocation functions */
	spin_lock(&lock);

	result = avlrcu_test_rol(&avlrcu_range, &match.node);

	spin_unlock(&lock);

	if (result == 0)
		pr_debug("%s: success\n", __func__);
	else
		pr_err("%s: failed: %d\n", __func__, result);
	pr_debug("-\n");

	*offs += count;
	return count;
}

static ssize_t rrl_map(struct file *file, const char __user *data, size_t count, loff_t *offs)
{
	struct test_avlrcu_node match;
	int result;

	result = kstrtoul_from_user(data, count, 16, &match.address);
	if (IS_ERR_VALUE((long)result))
		return result;

	pr_debug("%s: at %lx\n", __func__, match.address);

	/* these have to match with the allocation functions */
	spin_lock(&lock);

	result = avlrcu_test_rrl(&avlrcu_range, &match.node);

	spin_unlock(&lock);

	if (result == 0)
		pr_debug("%s: success\n", __func__);
	else
		pr_err("%s: failed: %d\n", __func__, result);
	pr_debug("-\n");

	*offs += count;
	return count;
}

static ssize_t rlr_map(struct file *file, const char __user *data, size_t count, loff_t *offs)
{
	struct test_avlrcu_node match;
	int result;

	result = kstrtoul_from_user(data, count, 16, &match.address);
	if (IS_ERR_VALUE((long)result))
		return result;

	pr_debug("%s: at %lx\n", __func__, match.address);

	/* these have to match with the allocation functions */
	spin_lock(&lock);

	result = avlrcu_test_rlr(&avlrcu_range, &match.node);

	spin_unlock(&lock);

	if (result == 0)
		pr_debug("%s: success\n", __func__);
	else
		pr_err("%s: failed: %d\n", __func__, result);
	pr_debug("-\n");

	*offs += count;
	return count;
}


/* even if tree is empty, return a valid value for header, root, footer
 * NULL means past end of file condition */
#define DUMMY_NODE_END_OF_TREE	((struct avlrcu_node *) 1)

static void *dump_gv_start(struct seq_file *s, loff_t *pos)
{
	struct avlrcu_node *node = s->private;

	/* new session */
	if (node)
		return node;

	/* past end of file condition */
	if (*pos)
		return NULL;

	node = avlrcu_first(&avlrcu_range);
	if (!node)
		node = DUMMY_NODE_END_OF_TREE;
	s->private = node;

	/* print header */
	seq_puts(s, "digraph G {\n");
	seq_puts(s, "\troot [label=\"ROOT\", shape=box]\n");

	return node;
}

static int dump_gv_show(struct seq_file *s, void *v)
{
	struct avlrcu_node *node = s->private;
	struct avlrcu_node *parent, *left, *right;
	struct test_avlrcu_node *container;

	if (unlikely(node == DUMMY_NODE_END_OF_TREE))
		return 0;

	parent = node->parent;
	left = node->left;
	right = node->right;
	container = avlrcu_entry(node, struct test_avlrcu_node, node);

	seq_printf(s, "\tn%lx [label=\"%lx\\n%ld\", style=filled, fillcolor=%s]\n",
		(unsigned long)node, container->address, (long)node->balance, "green");

	if (left)
		seq_printf(s, "\tn%lx -> n%lx [tailport=w]\n",
			(unsigned long)node, (unsigned long)left);

	if (right)
		seq_printf(s, "\tn%lx -> n%lx [tailport=e]\n",
			(unsigned long)node, (unsigned long)right);

	if (is_root(parent)) {
		seq_printf(s, "\troot -> n%lx [tailport=s]\n",
			(unsigned long)node);
		seq_printf(s, "\tn%lx -> root [headport=s, tailport=n, style=dotted, color=lightgrey]\n",
			(unsigned long)node);
	}
	else if (is_left_child(parent)) {
		seq_printf(s, "\tn%lx -> n%lx [headport=w, tailport=n, style=dotted, color=lightgrey]\n",
			(unsigned long)node, (unsigned long)strip_flags(parent));
	}
	else {
		seq_printf(s, "\tn%lx -> n%lx [headport=e, tailport=n, style=dotted, color=lightgrey]\n",
			(unsigned long)node, (unsigned long)strip_flags(parent));
	}

	return 0;
}

static void *dump_gv_next(struct seq_file *s, void *v, loff_t *pos)
{
	struct avlrcu_node *node = s->private;

	(*pos)++;
	if (node == DUMMY_NODE_END_OF_TREE)
		return NULL;

	node = avlrcu_next(node);
	if (!node) {
		s->private = DUMMY_NODE_END_OF_TREE;
		return NULL;
	}

	s->private = node;
	return node;
}

static void dump_gv_stop(struct seq_file *s, void *v)
{
	struct avlrcu_node *node = s->private;

	/* print footer, do not react on NULL */
	if (node == DUMMY_NODE_END_OF_TREE) {
		seq_puts(s, "}\n");
		s->private = NULL;
	}
}

static struct seq_operations dump_gv_seq_ops = {
	.start = dump_gv_start,
	.next = dump_gv_next,
	.show = dump_gv_show,
	.stop = dump_gv_stop,
};

int dump_gv_open(struct inode *inode, struct file *file)
{
	// can't seek on a tree without a walk & ignore
	file->f_mode &= ~FMODE_LSEEK;

	return seq_open(file, &dump_gv_seq_ops);
}


static void *dump_po_start(struct seq_file *s, loff_t *pos)
{
	struct avlrcu_node *node = s->private;

	/* new session */
	if (node)
		return node;

	/* past end of file condition */
	if (*pos)
		return NULL;

	node = avlrcu_first_po(&avlrcu_range);
	s->private = node;

	return node;
}

static int dump_po_show(struct seq_file *s, void *v)
{
	struct avlrcu_node *node = s->private;
	struct test_avlrcu_node *container = avlrcu_entry(node, struct test_avlrcu_node, node);

	seq_printf(s, "%lx\n", container->address);

	return 0;
}

static void *dump_po_next(struct seq_file *s, void *v, loff_t *pos)
{
	struct avlrcu_node *node = s->private;

	(*pos)++;
	node = avlrcu_next_po(node);
	s->private = node;

	return node;
}

static void dump_po_stop(struct seq_file *s, void *v)
{
	//struct avlrcu_node *node = s->private;

}

static struct seq_operations dump_po_seq_ops = {
	       .start = dump_po_start,
	       .next = dump_po_next,
	       .show = dump_po_show,
	       .stop = dump_po_stop,
};

int dump_po_open(struct inode *inode, struct file *file)
{
	 // can't seek on a tree without a walk & ignore
	file->f_mode &= ~FMODE_LSEEK;

	return seq_open(file, &dump_po_seq_ops);
}


static int find_args;
static unsigned long find_num1, find_num2;

static ssize_t find_write(struct file *file, const char __user *data, size_t count, loff_t *offs)
{
	int result;
	char buf[32];
	char *pos1 = NULL, *pos2 = NULL;
	char *space, *eol;

	pr_debug("%s: count = %d, offset = %d\n", __func__, (int)count, (int)*offs);

	// first parse input
	if (count > 32)
		return -E2BIG;
	memset(buf, 0, 32);
	if (copy_from_user(buf, data, count))
		return -EFAULT;
	// replace \n -> \0
	eol = strchr(buf, '\n');
	if (eol)
		*eol = '\0';

	if (buf[0] == '\0')
		find_args = 0;
	else
	{
		// find the space separator
		space = strchr(buf, ' ');
		if (!space) {
			find_args = 1;
			pos1 = buf;
		}
		else {
			find_args = 2;
			*space = '\0';
			pos1 = buf;
			pos2 = space + 1;
		}
	}

	if (pos1) {
		result = kstrtoul(pos1, 16, &find_num1);
		if (IS_ERR_VALUE((long)result))
			return result;
	}

	if (pos2) {
		result = kstrtoul(pos2, 16, &find_num2);
		if (IS_ERR_VALUE((long)result))
			return result;
	}

	if (find_args == 2 && find_num1 > find_num2)
		return -EINVAL;

	switch (find_args) {
	case 0:
		pr_debug("%s: will list all\n", __func__);
		break;
	case 1:
		pr_debug("%s: look for value %lx\n", __func__, find_num1);
		break;
	case 2:
		pr_debug("%s: look for interval %lx - %lx\n", __func__, find_num1, find_num2);
		break;
	}

	*offs += count;
	return count;
}

static int interval_filter(const struct avlrcu_node *crnt, const void *arg)
{
	const struct test_avlrcu_node *container = avlrcu_entry(crnt, struct test_avlrcu_node, node);
	const unsigned long *interval = arg;

	if (container->address < interval[0])
		return -1;
	else if (container->address >= interval[0] && container->address <= interval[1])
		return 0;
	else
		return 1;
}

static ssize_t find_read(struct file *f, char __user *buf, size_t size, loff_t *offset)
{
	unsigned long page;
	char *kbuf;
	int count = 0;
	struct test_avlrcu_node *container;
	struct avlrcu_node *node;

	pr_debug("%s: size = %d, offset = %d\n", __func__, (int)size, (int)*offset);

	if (*offset)
		return 0;

	page = get_zeroed_page(GFP_KERNEL);
	if (!page)
		return -ENOMEM;
	kbuf = (char *)page;

	rcu_read_lock();

	if (find_args == 0) {
		avlrcu_for_each_entry(container, &avlrcu_range, node) {
			count = sprintf(kbuf, "%lx ", container->address);
			kbuf += count;
		}
	}
	else if (find_args == 1) {
		struct test_avlrcu_node match = {
			.address = find_num1,
		};

		node = avlrcu_search(&avlrcu_range, &match.node);
		container = avlrcu_entry_safe(node, struct test_avlrcu_node, node);
		if (container) {
			count = sprintf(kbuf, "%lx ", container->address);
			kbuf += count;
		}
	}
	else {
		unsigned long interval[2] = {
			[0] = find_num1,
			[1] = find_num2,
		};

		avlrcu_for_each_entry_filter(container, &avlrcu_range, node, interval_filter, interval) {
			count = sprintf(kbuf, "%lx ", container->address);
			kbuf += count;
		}
	}

	rcu_read_unlock();

	/* nothing printed ? */
	if (kbuf == (char *)page) {
		free_page(page);
		return 0;
	}

	*kbuf = '\n';
	kbuf++;

	count = kbuf - (char *)page;
	kbuf = (char *)page;

	if (copy_to_user(buf, kbuf, count)) {
		free_page(page);
		return -EFAULT;
	}

	free_page(page);

	*offset += count;
	return count;
}

static struct file_operations insert_map_ops = {
	.owner = THIS_MODULE,
	.write = insert_map,
};

static struct file_operations delete_map_ops = {
	.owner = THIS_MODULE,
	.write = delete_map,
};

static struct file_operations unwind_map_ops = {
	.owner = THIS_MODULE,
	.write = unwind_map,
};

static struct file_operations clear_map_ops = {
	.owner = THIS_MODULE,
	.write = clear_map,
};

static struct file_operations ror_map_ops = {
	.owner = THIS_MODULE,
	.write = ror_map,
};

static struct file_operations rol_map_ops = {
	.owner = THIS_MODULE,
	.write = rol_map,
};

static struct file_operations rrl_map_ops = {
	.owner = THIS_MODULE,
	.write = rrl_map,
};

static struct file_operations rlr_map_ops = {
	.owner = THIS_MODULE,
	.write = rlr_map,
};

static struct file_operations dump_gv_map_ops = {
	.owner = THIS_MODULE,
	.open = dump_gv_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = seq_release
};

static struct file_operations dump_po_map_ops = {
	.owner = THIS_MODULE,
	.open = dump_po_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = seq_release,
};

static struct file_operations find_map_ops = {
	.owner = THIS_MODULE,
	.write = find_write,
	.read = find_read,
};

static int __init avlrcu_debugfs_init(void)
{
	static struct dentry *result;

	debugfs_dir = debugfs_create_dir("avlrcu", NULL);
	if (IS_ERR(debugfs_dir))
		return PTR_ERR(debugfs_dir);

	result = debugfs_create_file("insert", S_IWUGO, debugfs_dir, NULL, &insert_map_ops);
	if (IS_ERR(result))
		goto error;

	result = debugfs_create_file("delete", S_IWUGO, debugfs_dir, NULL, &delete_map_ops);
	if (IS_ERR(result))
		goto error;

	result = debugfs_create_file("unwind", S_IWUGO, debugfs_dir, NULL, &unwind_map_ops);
	if (IS_ERR(result))
		goto error;

	result = debugfs_create_file("clear", S_IWUGO, debugfs_dir, NULL, &clear_map_ops);
	if (IS_ERR(result))
		goto error;

	result = debugfs_create_file("ror", S_IWUGO, debugfs_dir, NULL, &ror_map_ops);
	if (IS_ERR(result))
		goto error;

	result = debugfs_create_file("rol", S_IWUGO, debugfs_dir, NULL, &rol_map_ops);
	if (IS_ERR(result))
		goto error;

	result = debugfs_create_file("rrl", S_IWUGO, debugfs_dir, NULL, &rrl_map_ops);
	if (IS_ERR(result))
		goto error;

	result = debugfs_create_file("rlr", S_IWUGO, debugfs_dir, NULL, &rlr_map_ops);
	if (IS_ERR(result))
		goto error;

	result = debugfs_create_file("dump_gv", S_IRUGO, debugfs_dir, NULL, &dump_gv_map_ops);
	if (IS_ERR(result))
		goto error;

	result = debugfs_create_file("dump_po", S_IRUGO, debugfs_dir, NULL, &dump_po_map_ops);
	if (IS_ERR(result))
		goto error;

	result = debugfs_create_file("find", S_IRUGO | S_IWUGO, debugfs_dir, NULL, &find_map_ops);
	if (IS_ERR(result))
		goto error;

#ifdef CONFIG_FAULT_INJECTION
	result = fault_create_debugfs_attr("fail_avlrcu", debugfs_dir, &avlrcu_fault_attr);
	if (IS_ERR(result))
		goto error;
#endif /* CONFIG_FAULT_INJECTION */

	return 0;

error:
	debugfs_remove_recursive(debugfs_dir);

	return PTR_ERR(result);
}

static int __init avlrcu_test_init(void)
{
	int result;

	avlrcu_init(&avlrcu_range, &test_ops);

	// create access files
	result = avlrcu_debugfs_init();
	if (result)
		goto out_tree;

	// create a thread that validates the tree in the background
	validator = kthread_run(validator_func, NULL, "avlrcu-validator");
	if (IS_ERR(validator)) {
		result = PTR_ERR(validator);
		goto out_debugfs;
	}

	return 0;

out_debugfs:
	debugfs_remove_recursive(debugfs_dir);
out_tree:
	avlrcu_free(&avlrcu_range);

	return result;
}

static void __exit avlrcu_test_exit(void)
{
	debugfs_remove_recursive(debugfs_dir);

	kthread_stop(validator);

	avlrcu_free(&avlrcu_range);

	pr_debug("bye bye\n");
}

module_init(avlrcu_test_init);
module_exit(avlrcu_test_exit);

MODULE_AUTHOR("Mircea Cirjaliu");
MODULE_LICENSE("GPL");
