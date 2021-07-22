
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
#include <linux/spinlock.h>

#include "main.h"

// the object we test
static struct sptree_root sptree_range;
static DEFINE_SPINLOCK(lock);

// thread control
static struct task_struct *validator;

// access files
static struct dentry *debugfs_dir;


static struct sptree_node *test_alloc(void)
{
	struct test_sptree_node *container;

	container = kzalloc(sizeof(struct test_sptree_node), GFP_ATOMIC);
	if (!container)
		return NULL;

	return &container->node;
}

static void test_free(struct sptree_node *node)
{
	struct test_sptree_node *container;
	container = sptree_entry(node, struct test_sptree_node, node);

	kfree(container);
}

static void test_free_rcu(struct sptree_node *node)
{
	struct test_sptree_node *container;
	container = sptree_entry(node, struct test_sptree_node, node);

	kfree_rcu(container, node.rcu);

}

static unsigned long test_get_key(struct sptree_node *node)
{
	struct test_sptree_node *container;
	container = sptree_entry(node, struct test_sptree_node, node);

	return container->address;
}

static void test_copy(struct sptree_node *to, struct sptree_node *from)
{
	struct test_sptree_node *container_to;
	struct test_sptree_node *container_from;

	container_to = sptree_entry(to, struct test_sptree_node, node);
	container_from = sptree_entry(from, struct test_sptree_node, node);

	memcpy(container_to, container_from, sizeof(struct test_sptree_node));
}

static struct sptree_ops test_ops = {
	.alloc = test_alloc,
	.free = test_free,
	.free_rcu = test_free_rcu,
	.get_key = test_get_key,
	.copy = test_copy,
};

static int prev_count = 0;
static void validate_greater(struct sptree_root *root)
{
	struct test_sptree_node *container;
	unsigned long prev;
	int count;
	int result = 0;

	/* these have to match with the allocation functions */
	rcu_read_lock();

	prev = 0;
	count = 0;
	sptree_for_each_entry(container, root, node) {
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
		validate_greater(&sptree_range);

		msleep_interruptible(10);

	} while (!kthread_should_stop());

	pr_debug("validator stopped\n");

	return 0;
}

static ssize_t prealloc_insert_map(struct file *file, const char __user *data, size_t count, loff_t *offs)
{
	unsigned long value;
	struct test_sptree_node *container;
	int result;

	result = kstrtoul_from_user(data, count, 16, &value);
	if (IS_ERR_VALUE((long)result))
		return result;

	pr_debug("%s: at %lx\n", __func__, value);

	// check if aligned to page
	if (value & ~PAGE_MASK) {
		pr_err("%s: non-aligned value: %lx\n", __func__, value);
		return -EINVAL;
	}

	container = kzalloc(sizeof(struct test_sptree_node), GFP_ATOMIC);
	if (!container)
		return -ENOMEM;
	container->address = value;

	/* these have to match with the allocation functions */
	spin_lock(&lock);

	result = prealloc_insert(&sptree_range, &container->node);

	spin_unlock(&lock);

	if (result == 0)
		pr_debug("%s: success\n", __func__);
	else
		pr_err("%s: failed: %d\n", __func__, result);
	pr_debug("-\n");

	*offs += count;
	return count;
}

static ssize_t prealloc_delete_map(struct file *file, const char __user *data, size_t count, loff_t *offs)
{
	unsigned long value;
	int result;

	result = kstrtoul_from_user(data, count, 16, &value);
	if (IS_ERR_VALUE((long)result))
		return result;

	pr_debug("%s: at %lx\n", __func__, value);

	// check if alligned to page
	if (value & ~PAGE_MASK) {
		pr_err("%s: non-aligned value: %lx\n", __func__, value);
		return -EINVAL;
	}

	/* these have to match with the allocation functions */
	spin_lock(&lock);

	result = prealloc_delete(&sptree_range, value);

	spin_unlock(&lock);

	if (result == 0)
		pr_debug("%s: success\n", __func__);
	else
		pr_err("%s: failed: %d\n", __func__, result);
	pr_debug("-\n");

	*offs += count;
	return count;
}

static ssize_t prealloc_unwind_map(struct file *file, const char __user *data, size_t count, loff_t *offs)
{
	unsigned long value;
	int result;

	result = kstrtoul_from_user(data, count, 16, &value);
	if (IS_ERR_VALUE((long)result))
		return result;

	pr_debug("%s: at %lx\n", __func__, value);

	// check if alligned to page
	if (value & ~PAGE_MASK) {
		pr_err("%s: non-aligned value: %lx\n", __func__, value);
		return -EINVAL;
	}

	/* these have to match with the allocation functions */
	spin_lock(&lock);

	result = prealloc_unwind(&sptree_range, value);

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
	sptree_free(&sptree_range);
	prev_count = 0;	/* reset validator counter */

	*offs += count;
	return count;
}

static ssize_t ror_map(struct file *file, const char __user *data, size_t count, loff_t *offs)
{
	unsigned long value;
	int result;

	result = kstrtoul_from_user(data, count, 16, &value);
	if (IS_ERR_VALUE((long)result))
		return result;

	pr_debug("%s: at %lx\n", __func__, value);

	// check if alligned to page
	if (value & ~PAGE_MASK) {
		pr_err("%s: non-aligned value: %lx\n", __func__, value);
		return -EINVAL;
	}

	/* these have to match with the allocation functions */
	spin_lock(&lock);

	result = sptree_ror(&sptree_range, value);

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
	unsigned long value;
	int result;

	result = kstrtoul_from_user(data, count, 16, &value);
	if (IS_ERR_VALUE((long)result))
		return result;

	pr_debug("%s: at %lx\n", __func__, value);

	// check if alligned to page
	if (value & ~PAGE_MASK) {
		pr_err("%s: non-aligned value: %lx\n", __func__, value);
		return -EINVAL;
	}

	/* these have to match with the allocation functions */
	spin_lock(&lock);

	result = sptree_rol(&sptree_range, value);

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
	unsigned long value;
	int result;

	result = kstrtoul_from_user(data, count, 16, &value);
	if (IS_ERR_VALUE((long)result))
		return result;

	pr_debug("%s: at %lx\n", __func__, value);

	// check if alligned to page
	if (value & ~PAGE_MASK) {
		pr_err("%s: non-aligned value: %lx\n", __func__, value);
		return -EINVAL;
	}

	/* these have to match with the allocation functions */
	spin_lock(&lock);

	result = sptree_rrl(&sptree_range, value);

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
	unsigned long value;
	int result;

	result = kstrtoul_from_user(data, count, 16, &value);
	if (IS_ERR_VALUE((long)result))
		return result;

	pr_debug("%s: at %lx\n", __func__, value);

	// check if alligned to page
	if (value & ~PAGE_MASK) {
		pr_err("%s: non-aligned value: %lx\n", __func__, value);
		return -EINVAL;
	}

	/* these have to match with the allocation functions */
	spin_lock(&lock);

	result = sptree_rlr(&sptree_range, value);

	spin_unlock(&lock);

	if (result == 0)
		pr_debug("%s: success\n", __func__);
	else
		pr_err("%s: failed: %d\n", __func__, result);
	pr_debug("-\n");

	*offs += count;
	return count;
}


// TODO: this crap doesn't create the first file when the tree is empty
// TODO: must add it as a corner case !!

static void *dump_gv_start(struct seq_file *s, loff_t *pos)
{
	struct sptree_node *node = s->private;

	/* new session */
	if (node)
		return node;

	/* past end of file condition */
	if (*pos)
		return NULL;

	node = sptree_first(&sptree_range);
	s->private = node;

	// print header
	seq_puts(s, "digraph G {\n");
	seq_puts(s, "\troot [label=\"ROOT\", shape=box]\n");

	return node;
}

static int dump_gv_show(struct seq_file *s, void *v)
{
	struct sptree_node *node = s->private;
	struct sptree_node *parent = node->parent;
	struct sptree_node *left = node->left;
	struct sptree_node *right = node->right;
	struct test_sptree_node *container = sptree_entry(node, struct test_sptree_node, node);

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
	struct sptree_node *node = s->private;

	(*pos)++;
	node = sptree_next(node);
	s->private = node;

	return node;
}

static void dump_gv_stop(struct seq_file *s, void *v)
{
	struct sptree_node *node = s->private;

	// print footer
	if (node == NULL)
		seq_puts(s, "}\n");
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


static struct file_operations prealloc_insert_map_ops = {
	.owner = THIS_MODULE,
	.write = prealloc_insert_map,
};

static struct file_operations prealloc_delete_map_ops = {
	.owner = THIS_MODULE,
	.write = prealloc_delete_map,
};

static struct file_operations prealloc_unwind_map_ops = {
	.owner = THIS_MODULE,
	.write = prealloc_unwind_map,
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

static int __init sptree_debugfs_init(void)
{
	static struct dentry *result;

	debugfs_dir = debugfs_create_dir("sptree", NULL);
	if (IS_ERR(debugfs_dir))
		return PTR_ERR(debugfs_dir);

	result = debugfs_create_file("prealloc_insert", S_IWUGO, debugfs_dir, NULL, &prealloc_insert_map_ops);
	if (IS_ERR(result))
		goto error;

	result = debugfs_create_file("prealloc_delete", S_IWUGO, debugfs_dir, NULL, &prealloc_delete_map_ops);
	if (IS_ERR(result))
		goto error;

	result = debugfs_create_file("prealloc_unwind", S_IWUGO, debugfs_dir, NULL, &prealloc_unwind_map_ops);
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

	return 0;

error:
	debugfs_remove_recursive(debugfs_dir);

	return PTR_ERR(result);
}

static int __init sptree_test_init(void)
{
	int result;

	// create the sptree_range: 4KB - 1MB
	result = sptree_init(&sptree_range, &test_ops);
	if (result)
		return result;

	// create access files
	result = sptree_debugfs_init();
	if (result)
		goto out_tree;

	// create a thread that validates the tree in the background
	validator = kthread_run(validator_func, NULL, "sptree-validator");
	if (IS_ERR(validator)) {
		result = PTR_ERR(validator);
		goto out_debugfs;
	}

	return 0;

out_debugfs:
	debugfs_remove_recursive(debugfs_dir);
out_tree:
	sptree_free(&sptree_range);

	return result;
}

static void __exit sptree_test_exit(void)
{
	debugfs_remove_recursive(debugfs_dir);

	kthread_stop(validator);

	sptree_free(&sptree_range);

	pr_debug("bye bye\n");
}

module_init(sptree_test_init);
module_exit(sptree_test_exit);

MODULE_AUTHOR("Mircea Cirjaliu");
MODULE_LICENSE("GPL");
