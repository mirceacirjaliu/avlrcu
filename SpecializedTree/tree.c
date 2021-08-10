
#define pr_fmt(fmt)	KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/rcupdate.h>
#include <linux/compiler.h>

#include "internal.h"

int sptree_init(struct sptree_root *root, struct sptree_ops *ops)
{
	root->ops = ops;
	root->root = NULL;

	pr_debug("%s: created empty root\n", __func__);

	return 0;
}

void sptree_free(struct sptree_root *root)
{
	struct sptree_ops *ops = root->ops;
	struct sptree_node *node;
	struct sptree_root temp_root;

	/* cut access to the tree */
	temp_root.root = root->root;
	rcu_assign_pointer(root->root, NULL);

	/* schedule all the nodes for deletion */
	sptree_for_each(node, &temp_root)
		ops->free_rcu(node);
}



// TODO: debug purposes only, remove once code is stable
// checks the AVL condition for subtree depths
static int validate_subtree_balancing(struct sptree_node *node)
{
	int left_depth = 0;
	int right_depth = 0;
	int diff;

	if (node->left)
		left_depth = validate_subtree_balancing(node->left);

	if (node->right)
		right_depth = validate_subtree_balancing(node->right);

	diff = right_depth - left_depth;

	// check for balance outside [-1..1]
	if (diff < -1 || diff > 1)
		pr_warn("%s: excessive balance on "NODE_FMT", left depth %d, right depth %d\n",
			__func__, NODE_ARG(node), left_depth, right_depth);

	// check if the algorithm computed the balance right
	if (diff != node->balance)
		pr_err("%s: wrong balance factor on "NODE_FMT", left depth %d, right depth %d\n",
			__func__, NODE_ARG(node), left_depth, right_depth);

	// return the depth of the subtree rooted at node
	return max(left_depth, right_depth) + 1;
}

void validate_avl_balancing(struct sptree_root *root)
{
	if (root->root)
		validate_subtree_balancing(root->root);
}


// search for the node containing this address
struct sptree_node *search(struct sptree_root *root, unsigned long key)
{
	struct sptree_ops *ops = root->ops;
	struct sptree_node *crnt;
	unsigned long crnt_key;

	pr_debug("%s: looking for %lx\n", __func__, key);

	crnt = rcu_access_pointer(root->root);
	while (crnt) {
		crnt_key = ops->get_key(crnt);

		if (key == crnt_key)
			break;
		else if (key < crnt_key)
			crnt = rcu_access_pointer(crnt->left);
		else
			crnt = rcu_access_pointer(crnt->right);
	}

	if (crnt == NULL)
		pr_debug("%s: found nothing\n", __func__);
	else
		pr_debug("%s: found inside "NODE_FMT"\n", __func__, NODE_ARG(crnt));

	return crnt;
}


/* in-order iteration */
static struct sptree_node *sptree_leftmost(struct sptree_node *node)
{
	struct sptree_node *next;

	/* descend along the left branch */
	for (;;) {
		next = rcu_access_pointer(node->left);
		if (next) {
			node = next;
			continue;
		}

		return node;
	}
}

static struct sptree_node *sptree_successor(struct sptree_node *node)
{
	struct sptree_node *next;

	/* ascend along the right branch */
	for (;;) {
		next = rcu_access_pointer(node->parent);

		if (is_left_child(next))
			return strip_flags(next);

		if (is_root(next))
			return NULL;

		node = strip_flags(next);
	}
}

struct sptree_node *sptree_first(struct sptree_root *root)
{
	struct sptree_node *next = rcu_access_pointer(root->root);

	if (unlikely(!next))
		return NULL;

	return sptree_leftmost(next);
}

struct sptree_node *sptree_next(struct sptree_node *node)
{
	struct sptree_node *next;

	/* in-order LNR -> next is right */
	next = rcu_access_pointer(node->right);
	if (next)
		return sptree_leftmost(next);

	return sptree_successor(node);
}


/* post-order iteration */
static struct sptree_node *sptree_left_deepest(struct sptree_node *node)
{
	struct sptree_node *next;

	for (;;) {
		next = rcu_access_pointer(node->left);
		if (next) {
			node = next;
			continue;
		}

		next = rcu_access_pointer(node->right);
		if (next) {
			node = next;
			continue;
		}

		return node;
	}
}

extern struct sptree_node *sptree_first_po(struct sptree_root *root)
{
	struct sptree_node *next = rcu_access_pointer(root->root);

	if (unlikely(!next))
		return NULL;

	return sptree_left_deepest(next);
}

extern struct sptree_node *sptree_next_po(struct sptree_node *node)
{
	struct sptree_node *parent, *next;

	if (!node)
		return NULL;

	parent = rcu_access_pointer(node->parent);
	if (!is_root(parent) && is_left_child(parent)) {
		parent = strip_flags(parent);
		next = rcu_access_pointer(parent->right);
		if (next)
			return sptree_left_deepest(next);
		else
			return parent;
	}
	else
		return strip_flags(parent);
}


static struct sptree_node *rotate_right_generic(struct sptree_ctxt *ctxt, struct sptree_node *target)
{
	struct sptree_node *prealloc;
	struct sptree_node *pivot;

	target = prealloc_replace(ctxt, target);
	if (!target)
		return NULL;

	pivot = prealloc_child(ctxt, target, LEFT_CHILD);
	if (!pivot)
		goto error;

	prealloc = prealloc_ror(ctxt, target);

	if (ctxt->diff == 0)
		return prealloc;

	// otherwise extend the prealloc tree up & propagate diff
	// ...

	return prealloc;

error:
	_delete_prealloc(ctxt, target);
	return NULL;
}

int test_ror(struct sptree_root *root, unsigned long addr)
{
	struct sptree_node *target;
	struct sptree_node *pivot;
	struct sptree_node *prealloc;
	struct sptree_ctxt ctxt;

	target = search(root, addr);
	if (!target)
		return -ENXIO;

	pivot = target->left;
	if (!pivot) {
		pr_err("%s: we don't have a pivot for "NODE_FMT"\n", __func__, NODE_ARG(target));
		return -EINVAL;
	}

	sptree_ctxt_init(&ctxt, root);

	prealloc = rotate_right_generic(&ctxt, target);
	if (!prealloc)
		return -ENOMEM;

	prealloc_connect(root, prealloc);

	if (!llist_empty(&ctxt.old))
		prealloc_remove_old(&ctxt);

	validate_avl_balancing(root);

	return 0;
}

static struct sptree_node *rotate_left_generic(struct sptree_ctxt *ctxt, struct sptree_node *target)
{
	struct sptree_node *prealloc;
	struct sptree_node *pivot;

	target = prealloc_replace(ctxt, target);
	if (!target)
		return NULL;

	pivot = prealloc_child(ctxt, target, RIGHT_CHILD);
	if (!pivot)
		goto error;

	prealloc = prealloc_rol(ctxt, target);

	if (ctxt->diff == 0)
		return prealloc;

	// otherwise extend the prealloc tree up & propagate diff
	// ...

	return prealloc;

error:
	_delete_prealloc(ctxt, target);
	return NULL;
}

int test_rol(struct sptree_root *root, unsigned long addr)
{
	struct sptree_node *target;
	struct sptree_node *pivot;
	struct sptree_node *prealloc;
	struct sptree_ctxt ctxt;

	target = search(root, addr);
	if (!target)
		return -ENXIO;

	pivot = target->right;
	if (!pivot) {
		pr_err("%s: we don't have a pivot for "NODE_FMT"\n", __func__, NODE_ARG(target));
		return -EINVAL;
	}

	sptree_ctxt_init(&ctxt, root);

	prealloc = rotate_left_generic(&ctxt, target);
	if (!prealloc)
		return -ENOMEM;

	prealloc_connect(root, prealloc);

	if (!llist_empty(&ctxt.old))
		prealloc_remove_old(&ctxt);

	validate_avl_balancing(root);

	return 0;
}

static struct sptree_node *rotate_right_left_generic(struct sptree_ctxt *ctxt, struct sptree_node *target)
{
	struct sptree_node *left, *right;
	struct sptree_node *prealloc;

	target = prealloc_replace(ctxt, target);
	if (!target)
		return NULL;

	right = prealloc_child(ctxt, target, RIGHT_CHILD);
	if (!right)
		goto error;

	left = prealloc_child(ctxt, right, LEFT_CHILD);
	if (!left)
		goto error;

	prealloc = prealloc_rrl(ctxt, target);

	if (ctxt->diff == 0)
		return prealloc;

	// otherwise extend the prealloc tree up & propagate diff
	// ...

	return prealloc;

error:
	_delete_prealloc(ctxt, target);
	return NULL;
}

int test_rrl(struct sptree_root *root, unsigned long addr)
{
	struct sptree_node *target;
	struct sptree_node *prealloc;
	struct sptree_ctxt ctxt;

	target = search(root, addr);
	if (!target)
		return -ENXIO;

	if (!target->right) {
		pr_err("%s: node "NODE_FMT" is too low\n", __func__, NODE_ARG(target));
		return -EINVAL;
	}

	if (!target->right->left) {
		pr_err("%s: node "NODE_FMT" is too low\n", __func__, NODE_ARG(target));
		return -EINVAL;
	}

	sptree_ctxt_init(&ctxt, root);

	prealloc = rotate_right_left_generic(&ctxt, target);
	if (!prealloc)
		return -ENOMEM;

	prealloc_connect(root, prealloc);

	if (!llist_empty(&ctxt.old))
		prealloc_remove_old(&ctxt);

	validate_avl_balancing(root);

	return 0;
}

static struct sptree_node *rotate_left_right_generic(struct sptree_ctxt *ctxt, struct sptree_node *target)
{
	struct sptree_node *left, *right;
	struct sptree_node *prealloc;

	target = prealloc_replace(ctxt, target);
	if (!target)
		return NULL;

	left = prealloc_child(ctxt, target, LEFT_CHILD);
	if (!left)
		goto error;

	right = prealloc_child(ctxt, left, RIGHT_CHILD);
	if (!right)
		goto error;

	prealloc = prealloc_rlr(ctxt, target);

	if (ctxt->diff == 0)
		return prealloc;

	// otherwise extend the prealloc tree up & propagate diff
	// ...

	return prealloc;

error:
	_delete_prealloc(ctxt, target);
	return NULL;
}

int test_rlr(struct sptree_root *root, unsigned long addr)
{
	struct sptree_node *target;
	struct sptree_node *prealloc;
	struct sptree_ctxt ctxt;

	target = search(root, addr);
	if (!target)
		return -ENXIO;

	if (!target->left) {
		pr_err("%s: node "NODE_FMT" is too low\n", __func__, NODE_ARG(target));
		return -EINVAL;
	}

	if (!target->left->right) {
		pr_err("%s: node "NODE_FMT" is too low\n", __func__, NODE_ARG(target));
		return -EINVAL;
	}

	sptree_ctxt_init(&ctxt, root);

	prealloc = rotate_left_right_generic(&ctxt, target);
	if (!prealloc)
		return -ENOMEM;

	prealloc_connect(root, prealloc);

	if (!llist_empty(&ctxt.old))
		prealloc_remove_old(&ctxt);

	validate_avl_balancing(root);

	return 0;
}

int test_unwind(struct sptree_root *root, unsigned long key)
{
	struct sptree_node *target;
	struct sptree_node *prealloc;
	struct sptree_ctxt ctxt;

	target = search(root, key);
	if (!target)
		return -ENXIO;

	if (is_leaf(target)) {
		pr_err("%s: node "NODE_FMT" already a leaf\n", __func__, NODE_ARG(target));
		return -EINVAL;
	}

	sptree_ctxt_init(&ctxt, root);

	// the unwind function returns the bottom of the preallocated branch
	prealloc = prealloc_unwind(&ctxt, target);
	if (!prealloc)
		return -ENOMEM;
	prealloc = prealloc_top(prealloc);

	pr_debug("%s: overall increase in height %d\n", __func__, ctxt.diff);

	prealloc_connect(root, prealloc);

	// this will remove the replaced nodes
	if (!llist_empty(&ctxt.old))
		prealloc_remove_old(&ctxt);

	// TODO: remove once code stable
	validate_avl_balancing(root);

	return 0;
}
