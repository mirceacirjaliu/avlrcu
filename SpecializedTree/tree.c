// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2021 BitDefender
 * Written by Mircea Cirjaliu
 */

#define pr_fmt(fmt)	KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/rcupdate.h>
#include <linux/compiler.h>

#include "internal.h"

void avlrcu_init(struct avlrcu_root *root, struct avlrcu_ops *ops)
{
	root->ops = ops;
	root->root = NULL;
}

/**
 * avlrcu_free() - deletes all the nodes in the tree
 * @root	root of the tree
 *
 * This is a write-side call and must be protected by a lock.
 * All nodes are posted to RCU for deletion.
 */
void avlrcu_free(struct avlrcu_root *root)
{
	struct avlrcu_ops *ops = root->ops;
	struct avlrcu_node *node, *temp;
	struct avlrcu_root temp_root;

	/* cut access to the tree */
	temp_root.root = root->root;
	rcu_assign_pointer(root->root, NULL);

	/*
	 * schedule all the nodes for deletion
	 * use post-order walk to avoid nodes getting freed and links getting
	 * broken if this iteration intersects the end of a grace period
	 */
	avlrcu_for_each_po_safe(node, temp, &temp_root)
		ops->free_rcu(node);
}

#ifdef AVLRCU_DEBUG

// checks the AVL condition for subtree depths
static int validate_subtree_balancing(struct avlrcu_node *node, bool *valid)
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

bool validate_avl_balancing(struct avlrcu_root *root)
{
	bool valid = true;

	if (root->root)
		validate_subtree_balancing(root->root, &valid);

	return valid;
}

#endif /* AVLRCU_DEBUG */

/**
 * avlrcu_search() - search for an object in the tree
 * @root	root of the tree
 * @match	node to match against
 *
 * Uses the cmp() callback to compare the equivalent object to objects in the tree.
 * The cmp() callback has the same semantics as memcmp().
 * If a match is found, it is returned.
 */
struct avlrcu_node *avlrcu_search(struct avlrcu_root *root, const struct avlrcu_node *match)
{
	struct avlrcu_ops *ops = root->ops;
	struct avlrcu_node *crnt;
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
static struct avlrcu_node *avlrcu_leftmost(struct avlrcu_node *node)
{
	struct avlrcu_node *next;

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

static struct avlrcu_node *avlrcu_successor(struct avlrcu_node *node)
{
	struct avlrcu_node *next;

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

struct avlrcu_node *avlrcu_first(struct avlrcu_root *root)
{
	struct avlrcu_node *next = rcu_access_pointer(root->root);

	if (unlikely(!next))
		return NULL;

	return avlrcu_leftmost(next);
}

struct avlrcu_node *avlrcu_next(struct avlrcu_node *node)
{
	struct avlrcu_node *next;

	/* in-order LNR -> next is right */
	next = rcu_access_pointer(node->right);
	if (next)
		return avlrcu_leftmost(next);

	return avlrcu_successor(node);
}

struct avlrcu_node *avlrcu_first_filter(struct avlrcu_root *root, filter f, const void *arg)
{
	struct avlrcu_node *subroot = rcu_access_pointer(root->root);
	struct avlrcu_node *left, *right, *first = NULL;
	int result;

	if (unlikely(!subroot))
		return NULL;

	/* look for the first node that verifies f(arg, node) >= 0 */
	/* break if f(arg, subroot) >= 0 && left exists && f(arg, left) < 0 */
	do {
		left = rcu_access_pointer(subroot->left);
		right = rcu_access_pointer(subroot->right);
		result = f(subroot, arg);

		if (result >= 0) {
			if (result == 0)
				first = subroot;
			subroot = left;
		}
		else {
			subroot = right;
		}
	} while (subroot);

	/* must make sure that f(arg, first) == 0  */
	return first;
}

struct avlrcu_node *avlrcu_next_filter(struct avlrcu_node *node, filter f, const void *arg)
{
	struct avlrcu_node *next;

	ASSERT(node && f(node, arg) == 0);

	next = rcu_access_pointer(node->right);
	if (next) {
		next = avlrcu_leftmost(next);
		return (f(next, arg) == 0) ? next : NULL;
	}

	next = avlrcu_successor(node);
	if (next)
		return (f(next, arg) == 0) ? next : NULL;

	return NULL;
}


/* post-order iteration */
static struct avlrcu_node *avlrcu_left_deepest(struct avlrcu_node *node)
{
	for (;;) {
		if (node->left) {
			node = node->left;
			continue;
		}

		if (node->right) {
			node = node->right;
			continue;
		}

		return node;
	}
}

extern struct avlrcu_node *avlrcu_first_po(struct avlrcu_root *root)
{
	if (unlikely(!root->root))
		return NULL;

	return avlrcu_left_deepest(root->root);
}

extern struct avlrcu_node *avlrcu_next_po(struct avlrcu_node *node)
{
	struct avlrcu_node *parent;

	if (unlikely(!node))
		return NULL;

	parent = get_parent(node);
	if (!is_root(parent) && is_left_child(node->parent)) {
		if (parent->right)
			return avlrcu_left_deepest(parent->right);
		else
			return parent;
	}
	else
		return parent;
}


static struct avlrcu_node *fix_diff_height(struct avlrcu_ctxt *ctxt, struct avlrcu_node *prealloc)
{
	struct avlrcu_node *initial = prealloc;
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

static struct avlrcu_node *rotate_right_generic(struct avlrcu_ctxt *ctxt, struct avlrcu_node *target)
{
	struct avlrcu_node *pivot;

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

int avlrcu_test_ror(struct avlrcu_root *root, const struct avlrcu_node *match)
{
	struct avlrcu_node *target;
	struct avlrcu_node *pivot;
	struct avlrcu_node *prealloc;
	struct avlrcu_ctxt ctxt;

	target = avlrcu_search(root, match);
	if (!target)
		return -ENXIO;

	pivot = target->left;
	if (!pivot) {
		pr_err("%s: node "NODE_FMT" is too low\n", __func__, NODE_ARG(target));
		return -EINVAL;
	}

	avlrcu_ctxt_init(&ctxt, root);

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

static struct avlrcu_node *rotate_left_generic(struct avlrcu_ctxt *ctxt, struct avlrcu_node *target)
{
	struct avlrcu_node *pivot;

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

int avlrcu_test_rol(struct avlrcu_root *root, const struct avlrcu_node *match)
{
	struct avlrcu_node *target;
	struct avlrcu_node *pivot;
	struct avlrcu_node *prealloc;
	struct avlrcu_ctxt ctxt;

	target = avlrcu_search(root, match);
	if (!target)
		return -ENXIO;

	pivot = target->right;
	if (!pivot) {
		pr_err("%s: node "NODE_FMT" is too low\n", __func__, NODE_ARG(target));
		return -EINVAL;
	}

	avlrcu_ctxt_init(&ctxt, root);

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

static struct avlrcu_node *rotate_right_left_generic(struct avlrcu_ctxt *ctxt, struct avlrcu_node *target)
{
	struct avlrcu_node *left, *right;

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

int avlrcu_test_rrl(struct avlrcu_root *root, const struct avlrcu_node *match)
{
	struct avlrcu_node *target;
	struct avlrcu_node *prealloc;
	struct avlrcu_ctxt ctxt;

	target = avlrcu_search(root, match);
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

	avlrcu_ctxt_init(&ctxt, root);

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

static struct avlrcu_node *rotate_left_right_generic(struct avlrcu_ctxt *ctxt, struct avlrcu_node *target)
{
	struct avlrcu_node *left, *right;

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

int avlrcu_test_rlr(struct avlrcu_root *root, const struct avlrcu_node *match)
{
	struct avlrcu_node *target;
	struct avlrcu_node *prealloc;
	struct avlrcu_ctxt ctxt;

	target = avlrcu_search(root, match);
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

	avlrcu_ctxt_init(&ctxt, root);

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

int avlrcu_test_unwind(struct avlrcu_root *root, const struct avlrcu_node *match)
{
	struct avlrcu_node *target;
	struct avlrcu_node *prealloc;
	struct avlrcu_ctxt ctxt;

	target = avlrcu_search(root, match);
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

	avlrcu_ctxt_init(&ctxt, root);

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
