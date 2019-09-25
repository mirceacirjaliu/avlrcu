
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
static struct dentry *insert_file;
static struct dentry *delete_file;
static struct dentry *ror_file;
static struct dentry *rol_file;
static struct dentry *dump_file;

static void validate_greater(struct sptree_root *root)
{
	struct sptree_iterator iter;
	unsigned long prev;
	int result = 0;

	rcu_read_lock();

	prev = 0;
	sptree_for_each_node(&iter, root) {
		// TODO: remove
		//pr_info("%lx-%lx ", iter.node->start, iter.node->start + iter.node->length);

		if (prev >= iter.node->start) {
			result = -EINVAL;
			break;
		}

		prev = iter.node->start;
	}

	// TODO: remove
	//pr_info("\n");

	rcu_read_unlock();

	if (result)
		pr_err("%s: invalid order detected\n", __func__);
}

static void validate_nodes(struct sptree_root *root)
{
	struct sptree_iterator iter;
	int result = 0;

	rcu_read_lock();

	sptree_for_each_node(&iter, root) {
		struct sptree_node *node = iter.node;

		// a node can't be a segment
		if (!node->mapping && (node->left || node->right)) {
			result = -EINVAL;
			break;
		}
	}

	rcu_read_unlock();

	if (result)
		pr_err("%s: invalid node detected\n", __func__);
}

static int validator_func(void *arg)
{
	pr_info("validator started\n");

	do {
		// validate each element is greater than the last
		validate_greater(&sptree_range);

		// a leaf is part of the initial sptree_range (or whole if no mappings)
		// otherwise a node (has children) is a mapping
		// a leaf can also be a mapping if surrounded by other mappings
		// a node can't be a free-sptree_range
		validate_nodes(&sptree_range);

		msleep_interruptible(1000);

	} while (!kthread_should_stop());

	pr_info("validator stopped\n");

	return 0;
}

static ssize_t insert_map(struct file *file, const char __user *data, size_t count, loff_t *offs)
{
	unsigned long value;
	int result;

	result = kstrtoul_from_user(data, count, 16, &value);
	if (IS_ERR_VALUE((long)result))
		return result;

	pr_info("%s: at %lx\n", __func__, value);

	// check if alligned to page
	if (value & ~PAGE_MASK) {
		pr_err("%s: non-aligned value: %lx\n", __func__, value);
		return -EINVAL;
	}

	// check if inside the sptree_range
	if (value < sptree_range.start || value > sptree_range.start + sptree_range.length - PAGE_SIZE) {
		pr_err("%s: outside sptree_range: %lx-%lx\n", __func__, sptree_range.start, sptree_range.start + sptree_range.length);
		return -EINVAL;
	}

	spin_lock(&lock);
	result = sptree_insert(&sptree_range, value);
	spin_unlock(&lock);

	if (result == 0)
		pr_info("%s: success\n", __func__);
	else
		pr_err("%s: failed: %d\n", __func__, result);
	pr_info("-\n");

	*offs += count;
	return count;
}

static ssize_t delete_map(struct file *file, const char __user *data, size_t count, loff_t *offs)
{
	unsigned long value;
	int result;

	result = kstrtoul_from_user(data, count, 16, &value);
	if (IS_ERR_VALUE((long)result))
		return result;

	pr_info("%s: at %lx\n", __func__, value);

	// check if alligned to page
	if (value & ~PAGE_MASK) {
		pr_err("%s: non-aligned value: %lx\n", __func__, value);
		return -EINVAL;
	}

	// check if inside the sptree_range
	if (value < sptree_range.start || value > sptree_range.start + sptree_range.length - PAGE_SIZE) {
		pr_err("%s: outside sptree_range: %lx-%lx\n", __func__, sptree_range.start, sptree_range.start + sptree_range.length);
		return -EINVAL;
	}

	spin_lock(&lock);
	// TODO: delete node
	// ...
	spin_unlock(&lock);

	if (result == 0)
		pr_info("%s: success\n", __func__);
	else
		pr_err("%s: failed: %d\n", __func__, result);
	pr_info("-\n");

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

	pr_info("%s: at %lx\n", __func__, value);

	// check if alligned to page
	if (value & ~PAGE_MASK) {
		pr_err("%s: non-aligned value: %lx\n", __func__, value);
		return -EINVAL;
	}

	// check if inside the sptree_range
	if (value < sptree_range.start || value > sptree_range.start + sptree_range.length - PAGE_SIZE) {
		pr_err("%s: outside sptree_range: %lx-%lx\n", __func__, sptree_range.start, sptree_range.start + sptree_range.length);
		return -EINVAL;
	}

	spin_lock(&lock);
	result = sptree_ror(&sptree_range, value);
	spin_unlock(&lock);

	if (result == 0)
		pr_info("%s: success\n", __func__);
	else
		pr_err("%s: failed: %d\n", __func__, result);
	pr_info("-\n");

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

	pr_info("%s: at %lx\n", __func__, value);

	// check if alligned to page
	if (value & ~PAGE_MASK) {
		pr_err("%s: non-aligned value: %lx\n", __func__, value);
		return -EINVAL;
	}

	// check if inside the sptree_range
	if (value < sptree_range.start || value > sptree_range.start + sptree_range.length - PAGE_SIZE) {
		pr_err("%s: outside sptree_range: %lx-%lx\n", __func__, sptree_range.start, sptree_range.start + sptree_range.length);
		return -EINVAL;
	}

	spin_lock(&lock);
	result = sptree_rol(&sptree_range, value);
	spin_unlock(&lock);

	if (result == 0)
		pr_info("%s: success\n", __func__);
	else
		pr_err("%s: failed: %d\n", __func__, result);
	pr_info("-\n");

	*offs += count;
	return count;
}


static void *dump_start(struct seq_file *s, loff_t *pos)
{
	struct sptree_iterator *iter = s->private;

	// did we have overflow ??
	if (iter) {
		if (iter->node == NULL)
			return NULL;
		else
			return iter;
	}

	// alloc & init iterator
	iter = kmalloc(sizeof(*iter), GFP_KERNEL);
	if (!iter)
		return NULL;

	sptree_iter_first(&sptree_range, iter);
	s->private = iter;

	// print header
	seq_puts(s, "digraph G {\n");
	seq_puts(s, "\troot [label=\"ROOT\", shape=box]\n");

	return iter;
}

static int dump_show(struct seq_file *s, void *v)
{
	struct sptree_iterator *iter = s->private;
	struct sptree_node *node = iter->node;
	struct sptree_node *parent = node->parent;	// may contain L/R flags
	struct sptree_node *left = node->left;
	struct sptree_node *right = node->right;

	seq_printf(s, "\tn%lx [label=\"%lx - %lx\\n%c\", style=filled, fillcolor=%s]\n",
		(unsigned long)node, node->start, node->start + node->length,
		node_balancing(node), node->mapping ? "red" : "green");

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
			(unsigned long)node, (unsigned long)strip_flag(parent));
	}
	else {
		seq_printf(s, "\tn%lx -> n%lx [headport=e, tailport=n, style=dotted, color=lightgrey]\n",
			(unsigned long)node, (unsigned long)strip_flag(parent));
	}

	return 0;
}

static void *dump_next(struct seq_file *s, void *v, loff_t *pos)
{
	struct sptree_iterator *iter = s->private;

	sptree_iter_next(iter);
	if (iter->node == NULL)
		return NULL;

	(*pos)++;
	return iter;
}

static void dump_stop(struct seq_file *s, void *v)
{
	struct sptree_iterator *iter = s->private;

	// print footer
	if (iter->node == NULL)
		seq_puts(s, "}\n");
}

static struct seq_operations dump_seq_ops = {
	.start = dump_start,
	.next = dump_next,
	.show = dump_show,
	.stop = dump_stop,
};

int dump_open(struct inode *inode, struct file *file)
{
	// can't seek on a tree without a walk & ignore
	file->f_mode &= ~FMODE_LSEEK;

	return seq_open(file, &dump_seq_ops);
}

static struct file_operations insert_map_ops = {
	.owner = THIS_MODULE,
	.write = insert_map,
};

static struct file_operations delete_map_ops = {
	.owner = THIS_MODULE,
	.write = delete_map,
};

static struct file_operations ror_map_ops = {
	.owner = THIS_MODULE,
	.write = ror_map,
};

static struct file_operations rol_map_ops = {
	.owner = THIS_MODULE,
	.write = rol_map,
};

static struct file_operations dump_map_ops = {
	.owner = THIS_MODULE,
	.open = dump_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = seq_release_private
};

static int __init sptree_debugfs_init(void)
{
	int result;

	debugfs_dir = debugfs_create_dir("sptree", NULL);
	if (IS_ERR(debugfs_dir))
		return PTR_ERR(debugfs_dir);

	insert_file = debugfs_create_file("insert", S_IWUGO, debugfs_dir, NULL, &insert_map_ops);
	if (IS_ERR(insert_file)) {
		result = PTR_ERR(insert_file);
		goto error;
	}

	delete_file = debugfs_create_file("delete", S_IWUGO, debugfs_dir, NULL, &delete_map_ops);
	if (IS_ERR(delete_file)) {
		result = PTR_ERR(delete_file);
		goto error;
	}

	ror_file = debugfs_create_file("ror", S_IWUGO, debugfs_dir, NULL, &ror_map_ops);
	if (IS_ERR(ror_file)) {
		result = PTR_ERR(ror_file);
		goto error;
	}

	rol_file = debugfs_create_file("rol", S_IWUGO, debugfs_dir, NULL, &rol_map_ops);
	if (IS_ERR(rol_file)) {
		result = PTR_ERR(rol_file);
		goto error;
	}

	dump_file = debugfs_create_file("dump", S_IRUGO, debugfs_dir, NULL, &dump_map_ops);
	if (IS_ERR(dump_file)) {
		result = PTR_ERR(dump_file);
		goto error;
	}

	return 0;

error:
	debugfs_remove_recursive(debugfs_dir);

	return result;
}

static int __init sptree_test_init(void)
{
	int result;

	// create the sptree_range: 4KB - 1MB
	result = sptree_init(&sptree_range, 4 * KB, 1 * MB - 4 * KB);
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

	pr_info("bye bye\n");
}

module_init(sptree_test_init);
module_exit(sptree_test_exit);

MODULE_AUTHOR("Mircea Cirjaliu");
MODULE_LICENSE("GPL");
