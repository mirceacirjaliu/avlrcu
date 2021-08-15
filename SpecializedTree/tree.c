
#define pr_fmt(fmt)	KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/rcupdate.h>
#include <linux/compiler.h>

#include "internal.h"

void sptree_init(struct sptree_root *root, struct sptree_ops *ops)
{
	root->ops = ops;
	root->root = NULL;
}

/**
 * sptree_free() - deletes all the nodes in the tree
 * @root	root of the tree
 *
 * This is a write-side call and must be protected by a lock.
 * All nodes are posted to RCU for deletion.
 */
void sptree_free(struct sptree_root *root)
{
	struct sptree_ops *ops = root->ops;
	struct sptree_node *node, *temp;
	struct sptree_root temp_root;

	/* cut access to the tree */
	temp_root.root = root->root;
	rcu_assign_pointer(root->root, NULL);

	/*
	 * schedule all the nodes for deletion
	 * use post-order walk to avoid nodes getting freed and links getting
	 * broken if this iteration intersects the end of a grace period
	 */
	sptree_for_each_po_safe(node, temp, &temp_root)
		ops->free_rcu(node);
}

#ifdef SPTREE_DEBUG

// checks the AVL condition for subtree depths
static int validate_subtree_balancing(struct sptree_node *node, bool *valid)
{
	int left_depth = 0;
	int right_depth = 0;
	int diff;

	if (node->left)
		left_depth = validate_subtree_balancing(node->left, valid);

	if (node->right)
		right_depth = validate_subtree_balancing(node->right, valid);

	diff = right_depth - left_depth;

	// check for balance outside [-1..1]
	if (diff < -1 || diff > 1) {
		pr_warn("%s: excessive balance on "NODE_FMT", left depth %d, right depth %d\n",
			__func__, NODE_ARG(node), left_depth, right_depth);
		*valid = false;
	}

	// check if the algorithm computed the balance right
	if (diff != node->balance) {
		pr_err("%s: wrong balance factor on "NODE_FMT", left depth %d, right depth %d\n",
			__func__, NODE_ARG(node), left_depth, right_depth);
		*valid = false;
	}

	// return the depth of the subtree rooted at node
	return max(left_depth, right_depth) + 1;
}

bool validate_avl_balancing(struct sptree_root *root)
{
	bool valid = true;

	if (root->root)
		validate_subtree_balancing(root->root, &valid);

	return valid;
}

#endif /* SPTREE_DEBUG */

/**
 * search() - search for an object in the tree
 * @root	root of the tree
 * @match	node to match against
 *
 * Uses the cmp() callback to compare the equivalent object to objects in the tree.
 * The cmp() callback has the same semantics as memcmp().
 * If a match is found, it is returned.
 */
struct sptree_node *search(struct sptree_root *root, const struct sptree_node *match)
{
	struct sptree_ops *ops = root->ops;
	struct sptree_node *crnt;
	int result;

	crnt = rcu_access_pointer(root->root);
	while (crnt) {
		result = ops->cmp(match, crnt);

		if (result == 0)
			break;
		else if (result < 0)
			crnt = rcu_access_pointer(crnt->left);
		else
			crnt = rcu_access_pointer(crnt->right);
	}

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

static struct sptree_node *fix_diff_height(struct sptree_ctxt *ctxt, struct sptree_node *prealloc)
{
	struct sptree_node *initial = prealloc;
	int i, num, diff;

	pr_debug("%s: overall increase in height: %d\n", __func__, ctxt->diff);
	ASSERT(is_new_branch(prealloc));

	/* extend the preallocated branch up to the root */
	while (!is_root(prealloc->parent)) {
		prealloc = prealloc_parent(ctxt, prealloc);
		if (!prealloc)
			goto error;
	}

	/* we have positive or negative diff ? */
	if (ctxt->diff > 0) {
		diff = 1;
		num = ctxt->diff;
	}
	else {
		diff = -1;
		num = -ctxt->diff;
	}

	/* call this func with diff +-1, num times */
	for (i = 0; i < num; i++)
		prealloc_propagate_change(ctxt, initial, diff);

	return prealloc;

error:
	_delete_prealloc(ctxt, prealloc);
	return NULL;
}

static struct sptree_node *rotate_right_generic(struct sptree_ctxt *ctxt, struct sptree_node *target)
{
	struct sptree_node *pivot;

	target = prealloc_replace(ctxt, target);
	if (!target)
		return NULL;

	pivot = prealloc_child(ctxt, target, LEFT_CHILD);
	if (!pivot)
		goto error;

	return prealloc_ror(ctxt, target);

error:
	_delete_prealloc(ctxt, target);
	return NULL;
}

int test_ror(struct sptree_root *root, const struct sptree_node *match)
{
	struct sptree_node *target;
	struct sptree_node *pivot;
	struct sptree_node *prealloc;
	struct sptree_ctxt ctxt;

	target = search(root, match);
	if (!target)
		return -ENXIO;

	pivot = target->left;
	if (!pivot) {
		pr_err("%s: node "NODE_FMT" is too low\n", __func__, NODE_ARG(target));
		return -EINVAL;
	}

	sptree_ctxt_init(&ctxt, root);

	prealloc = rotate_right_generic(&ctxt, target);
	if (!prealloc)
		return -ENOMEM;

	if (ctxt.diff != 0)
		prealloc = fix_diff_height(&ctxt, prealloc);

	prealloc_connect(root, prealloc);

	if (!llist_empty(&ctxt.old))
		prealloc_remove_old(&ctxt);

	validate_avl_balancing(root);

	return 0;
}

static struct sptree_node *rotate_left_generic(struct sptree_ctxt *ctxt, struct sptree_node *target)
{
	struct sptree_node *pivot;

	target = prealloc_replace(ctxt, target);
	if (!target)
		return NULL;

	pivot = prealloc_child(ctxt, target, RIGHT_CHILD);
	if (!pivot)
		goto error;

	return prealloc_rol(ctxt, target);

error:
	_delete_prealloc(ctxt, target);
	return NULL;
}

int test_rol(struct sptree_root *root, const struct sptree_node *match)
{
	struct sptree_node *target;
	struct sptree_node *pivot;
	struct sptree_node *prealloc;
	struct sptree_ctxt ctxt;

	target = search(root, match);
	if (!target)
		return -ENXIO;

	pivot = target->right;
	if (!pivot) {
		pr_err("%s: node "NODE_FMT" is too low\n", __func__, NODE_ARG(target));
		return -EINVAL;
	}

	sptree_ctxt_init(&ctxt, root);

	prealloc = rotate_left_generic(&ctxt, target);
	if (!prealloc)
		return -ENOMEM;

	if (ctxt.diff != 0)
		prealloc = fix_diff_height(&ctxt, prealloc);

	prealloc_connect(root, prealloc);

	if (!llist_empty(&ctxt.old))
		prealloc_remove_old(&ctxt);

	validate_avl_balancing(root);

	return 0;
}

static struct sptree_node *rotate_right_left_generic(struct sptree_ctxt *ctxt, struct sptree_node *target)
{
	struct sptree_node *left, *right;

	target = prealloc_replace(ctxt, target);
	if (!target)
		return NULL;

	right = prealloc_child(ctxt, target, RIGHT_CHILD);
	if (!right)
		goto error;

	left = prealloc_child(ctxt, right, LEFT_CHILD);
	if (!left)
		goto error;

	return prealloc_rrl(ctxt, target);

error:
	_delete_prealloc(ctxt, target);
	return NULL;
}

int test_rrl(struct sptree_root *root, const struct sptree_node *match)
{
	struct sptree_node *target;
	struct sptree_node *prealloc;
	struct sptree_ctxt ctxt;

	target = search(root, match);
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

	if (ctxt.diff != 0)
		prealloc = fix_diff_height(&ctxt, prealloc);

	prealloc_connect(root, prealloc);

	if (!llist_empty(&ctxt.old))
		prealloc_remove_old(&ctxt);

	validate_avl_balancing(root);

	return 0;
}

static struct sptree_node *rotate_left_right_generic(struct sptree_ctxt *ctxt, struct sptree_node *target)
{
	struct sptree_node *left, *right;

	target = prealloc_replace(ctxt, target);
	if (!target)
		return NULL;

	left = prealloc_child(ctxt, target, LEFT_CHILD);
	if (!left)
		goto error;

	right = prealloc_child(ctxt, left, RIGHT_CHILD);
	if (!right)
		goto error;

	return prealloc_rlr(ctxt, target);

error:
	_delete_prealloc(ctxt, target);
	return NULL;
}

int test_rlr(struct sptree_root *root, const struct sptree_node *match)
{
	struct sptree_node *target;
	struct sptree_node *prealloc;
	struct sptree_ctxt ctxt;

	target = search(root, match);
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

	if (ctxt.diff != 0)
		prealloc = fix_diff_height(&ctxt, prealloc);

	prealloc_connect(root, prealloc);

	if (!llist_empty(&ctxt.old))
		prealloc_remove_old(&ctxt);

	validate_avl_balancing(root);

	return 0;
}

int test_unwind(struct sptree_root *root, const struct sptree_node *match)
{
	struct sptree_node *target;
	struct sptree_node *prealloc;
	struct sptree_ctxt ctxt;

	target = search(root, match);
	if (!target)
		return -ENXIO;

	if (is_leaf(target)) {
		pr_err("%s: node "NODE_FMT" already a leaf\n", __func__, NODE_ARG(target));
		return -EINVAL;
	}

	if (!validate_avl_balancing(root)) {
		pr_err("%s: the tree is not in AVL shape\n", __func__);
		return -EINVAL;
	}

	sptree_ctxt_init(&ctxt, root);

	// the unwind function returns the bottom of the preallocated branch
	prealloc = prealloc_unwind(&ctxt, target);
	if (!prealloc)
		return -ENOMEM;
	prealloc = prealloc_top(&ctxt, prealloc);

	if (ctxt.diff != 0)
		prealloc = fix_diff_height(&ctxt, prealloc);

	prealloc_connect(root, prealloc);

	// this will remove the replaced nodes
	if (!llist_empty(&ctxt.old))
		prealloc_remove_old(&ctxt);

	validate_avl_balancing(root);

	return 0;
}
