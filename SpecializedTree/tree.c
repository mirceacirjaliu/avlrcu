
#define pr_fmt(fmt)	KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/rcupdate.h>
#include <linux/compiler.h>

#include "tree.h"

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


/**
 * rotate_right_rcu() - core of the rotate_right_...() family
 * @root:	The node rooting a subtree
 * @proot:	Address of pointer to @root (for changing parent)
 *		(root->root / parent->left / parent->right)
 *
 * Rotates the subtree rooted by @root in a RCU-safe manner.
 * The rotation does not break search or in-order walk operations.
 * Does not do balance factor fixing.
 * Can fail if allocation fails, but the tree is not altered.
 *
 * Returns pointer to the new root of the subtree or NULL.
 */
static struct sptree_node *rotate_right_rcu(struct sptree_node *root, struct sptree_node **proot)
{
	struct sptree_node *pivot = root->left;
	struct sptree_node *new_root;
	struct sptree_node *new_pivot;

	pr_debug("%s: rotate right at "NODE_FMT", pivot at "NODE_FMT"\n",
		__func__, NODE_ARG(root), NODE_ARG(pivot));

	BUG_ON(*proot != root);
	BUG_ON(!pivot);

	// alloc new nodes
	new_root = kzalloc(sizeof(*new_root), GFP_ATOMIC);
	if (!new_root)
		return NULL;

	new_pivot = kzalloc(sizeof(*new_pivot), GFP_ATOMIC);
	if (!new_pivot) {
		kfree(new_root);
		return NULL;
	}

	// copy to new nodes (nodes change role)
	memcpy(new_root, pivot, sizeof(struct sptree_node));
	memcpy(new_pivot, root, sizeof(struct sptree_node));

	// link direct pointers (new nodes not reachable yet)
	new_root->left = pivot->left;
	new_root->right = new_pivot;
	new_root->parent = root->parent;	// may contail L/R flags or NULL

	new_pivot->parent = make_right(new_root);
	new_pivot->left = pivot->right;
	new_pivot->right = root->right;

	// link root (of subtree)
	rcu_assign_pointer(*proot, new_root);

	// link parent pointers
	if (pivot->left)
		rcu_assign_pointer(pivot->left->parent, make_left(new_root));
	if (pivot->right)
		rcu_assign_pointer(pivot->right->parent, make_left(new_pivot));
	if (root->right)
		rcu_assign_pointer(root->right->parent, make_right(new_pivot));

	// free old nodes
	kfree_rcu(root, rcu);
	kfree_rcu(pivot, rcu);

	return new_root;
}

/**
 * rotate_left_rcu() - core of the rotate_left_...() family
 * @root:	The node rooting a subtree
 * @proot:	Address of pointer to @root (for changing parent)
 *		(root->root / parent->left / parent->right)
 *
 * Rotates the subtree rooted by @root in a RCU-safe manner.
 * The rotation does not break search or in-order walk operations.
 * Does not do balance factor fixing.
 * Can fail if allocation fails, but the tree is not altered.
 *
 * Returns pointer to the new root of the subtree or NULL.
 */
static struct sptree_node *rotate_left_rcu(struct sptree_node *root, struct sptree_node **proot)
{
	struct sptree_node *pivot = root->right;
	struct sptree_node *new_root;
	struct sptree_node *new_pivot;

	pr_debug("%s: rotate left at "NODE_FMT", pivot at "NODE_FMT"\n",
		__func__, NODE_ARG(root), NODE_ARG(pivot));

	BUG_ON(*proot != root);
	BUG_ON(!pivot);

	// alloc new nodes
	new_root = kzalloc(sizeof(*new_root), GFP_ATOMIC);
	if (!new_root)
		return NULL;

	new_pivot = kzalloc(sizeof(*new_pivot), GFP_ATOMIC);
	if (!new_pivot) {
		kfree(new_root);
		return NULL;
	}

	// copy to new nodes
	memcpy(new_root, pivot, sizeof(struct sptree_node));
	memcpy(new_pivot, root, sizeof(struct sptree_node));

	// link direct pointers to new nodes
	new_root->left = new_pivot;
	new_root->right = pivot->right;
	new_root->parent = root->parent;	// may contail L/R flags

	new_pivot->parent = make_left(new_root);
	new_pivot->left = root->left;
	new_pivot->right = pivot->left;

	// link root (of subtree)
	rcu_assign_pointer(*proot, new_root);

	// link parent pointers
	if (root->left)
		rcu_assign_pointer(root->left->parent, make_left(new_pivot));
	if (pivot->left)
		rcu_assign_pointer(pivot->left->parent, make_right(new_pivot));
	if (pivot->right)
		rcu_assign_pointer(pivot->right->parent, make_right(new_root));

	// free old nodes
	kfree_rcu(root, rcu);
	kfree_rcu(pivot, rcu);

	return new_root;
}

/**
 * ror_height_diff() - check if height of tree is modified by rotation
 * @root:	The node rooting a subtree
 *
 * Sometimes a simple rotation causes the overall height of a tree to
 * increase/decrease (by maximum 1).
 * This is important in compound rotations, since the balance of the root has
 * to be modified after the first rotation.
 *
 * Returns -1/0/+1 depending of the modification of the height.
 */
static int ror_height_diff(struct sptree_node *root)
{
	// root = X
	struct sptree_node *pivot = root->left;		// Y
	int diff = 0;

	if (pivot->balance >= 0) {
		if (root->balance >= 0)
			diff = 1;
	}
	else {
		if (root->balance >= 0)
			diff = 1;
		else if (root->balance < -1)
			diff = -1;
	}

	if (diff == -1)
		pr_debug("%s: height of "NODE_FMT" decreases\n", __func__, NODE_ARG(root));
	else if (diff == 1)
		pr_debug("%s: height of "NODE_FMT" increases\n", __func__, NODE_ARG(root));
	else
		pr_debug("%s: height of "NODE_FMT" stays the same\n", __func__, NODE_ARG(root));

	return diff;
}

/**
 * rol_height_diff() - check if height of tree is modified by rotation
 * @root:	The node rooting a subtree
 *
 * Sometimes a simple rotation causes the overall height of a tree to
 * increase/decrease (by maximum 1).
 * This is important in compound rotations, since the balance of the root has
 * to be modified after the first rotation.
 *
 * Returns -1/0/+1 depending of the modification of the height.
 */
static int rol_height_diff(struct sptree_node *root)
{
	// root = X
	struct sptree_node *pivot = root->right;	// Y
	int diff = 0;

	if (pivot->balance <= 0) {
		if (root->balance <= 0)
			diff = 1;
	}
	else {
		if (root->balance <= 0)
			diff = 1;
		else if (root->balance > 1)
			diff = -1;
	}

	if (diff == -1)
		pr_debug("%s: height of "NODE_FMT" decreases\n", __func__, NODE_ARG(root));
	else if (diff == 1)
		pr_debug("%s: height of "NODE_FMT" increases\n", __func__, NODE_ARG(root));
	else
		pr_debug("%s: height of "NODE_FMT" stays the same\n", __func__, NODE_ARG(root));

	return diff;
}

/**
 * propagate_height_diff() - go up the branch & change balance
 * @subtree:	The subtree whose height has changed following a rotation/deletion.
 *
 * Under certain operations, the height of the subtree changes, and
 * the changes must be propagated up to the root.
 *
 */
static void propagate_height_diff(struct sptree_node *subtree, int diff)
{
	struct sptree_node *parent;
	bool left;

	// parent may contain L/R flags or NULL, strip flags before using as pointer
	for (parent = subtree->parent; !is_root(parent); parent = parent->parent) {
		left = is_left_child(parent);
		parent = strip_flags(parent);

		if (left)
			parent->balance -= diff;
		else
			parent->balance += diff;

		pr_debug("%s: updated balance factor for "NODE_FMT"\n",
			__func__, NODE_ARG(parent));
	}
}


static struct sptree_node *rotate_right_generic(struct sptree_node *root, struct sptree_node **proot)
{
	// root = X
	struct sptree_node *pivot = root->left;		// Y
	struct sptree_node *new_root;			// new Y
	struct sptree_node *new_pivot;			// new X
	int height_diff;

	pr_debug("%s: rotate right at "NODE_FMT", pivot at "NODE_FMT"\n",
		__func__, NODE_ARG(root), NODE_ARG(pivot));

	height_diff = ror_height_diff(root);

	new_root = rotate_right_rcu(root, proot);
	if (!new_root)
		return NULL;

	new_pivot = new_root->right;

	// fix balance factors
	if (pivot->balance >= 0) {
		new_pivot->balance = root->balance + 1;
		if (root->balance <= -1)
			new_root->balance = pivot->balance + 1;
		else
			new_root->balance = root->balance + pivot->balance + 2;
	}
	else {
		new_pivot->balance = root->balance - pivot->balance + 1;
		if (root->balance <= pivot->balance - 1)
			new_root->balance = pivot->balance + 1;
		else
			new_root->balance = root->balance + 2;
	}

	// propagate change in height up the tree
	if (height_diff)
		propagate_height_diff(new_root, height_diff);

	pr_debug("%s: rotated right, new root is "NODE_FMT"\n",
		__func__, NODE_ARG(new_root));

	return new_root;
}

int sptree_ror(struct sptree_root *root, unsigned long addr)
{
	struct sptree_node *target, **ptarget;
	struct sptree_node *pivot;
	struct sptree_node *new_root;

	target = search(root, addr);
	if (!target)
		return -ENXIO;

	pivot = target->left;
	if (!pivot) {
		pr_err("%s: we don't have a pivot for "NODE_FMT"\n", __func__, NODE_ARG(target));
		return -EINVAL;
	}

	// validate balance
	//if (target->balance > 0) {
	//	pr_err("%s: root "NODE_FMT" already right-heavy, can't do\n", __func__, NODE_ARG(target));
	//	return -EINVAL;
	//}
	//if (pivot->balance > 0) {
	//	pr_err("%s: pivot "NODE_FMT" already right-heavy, can't do\n", __func__, NODE_ARG(target));
	//	return -EINVAL;
	//}

	// parent may contain L/R flags or NULL
	ptarget = get_pnode(root, target->parent);

	new_root = rotate_right_generic(target, ptarget);
	if (!new_root)
		return -ENOMEM;

	// TODO: remove once code stable
	validate_avl_balancing(root);

	return 0;
}


static struct sptree_node *rotate_left_generic(struct sptree_node *root, struct sptree_node **proot)
{
	// root = X
	struct sptree_node *pivot = root->right;	// Y
	struct sptree_node *new_root;			// new Y
	struct sptree_node *new_pivot;			// new X
	int height_diff;

	pr_debug("%s: rotate left at "NODE_FMT", pivot at "NODE_FMT"\n",
		__func__, NODE_ARG(root), NODE_ARG(pivot));

	height_diff = rol_height_diff(root);

	new_root = rotate_left_rcu(root, proot);
	if (!new_root)
		return NULL;

	new_pivot = new_root->left;

	// fix balance factors
	if (pivot->balance <= 0) {
		new_pivot->balance = root->balance - 1;
		if (root->balance >= 1)
			new_root->balance = pivot->balance - 1;
		else
			new_root->balance = root->balance + pivot->balance - 2;
	}
	else {
		new_pivot->balance = root->balance - pivot->balance - 1;
		if (root->balance >= pivot->balance + 1)
			new_root->balance = pivot->balance - 1;
		else
			new_root->balance = root->balance - 2;
	}

	// propagate change in height up the tree
	if (height_diff)
		propagate_height_diff(new_root, height_diff);

	pr_debug("%s: rotated left, new root is "NODE_FMT"\n",
		__func__, NODE_ARG(new_root));

	return new_root;
}

int sptree_rol(struct sptree_root *root, unsigned long addr)
{
	struct sptree_node *target, **ptarget;
	struct sptree_node *pivot;
	struct sptree_node *new_root;

	target = search(root, addr);
	if (!target)
		return -ENXIO;

	pivot = target->right;
	if (!pivot) {
		pr_err("%s: we don't have a pivot for "NODE_FMT"\n", __func__, NODE_ARG(target));
		return -EINVAL;
	}

	// validate balance
	//if (target->balance < 0) {
	//	pr_err("%s: root "NODE_FMT" already left-heavy, can't do\n", __func__, NODE_ARG(target));
	//	return -EINVAL;
	//}
	//if (pivot->balance < 0) {
	//	pr_err("%s: pivot "NODE_FMT" already left-heavy, can't do\n", __func__, NODE_ARG(target));
	//	return -EINVAL;
	//}

	// parent may contain L/R flags or NULL
	ptarget = get_pnode(root, target->parent);

	new_root = rotate_left_generic(target, ptarget);
	if (!new_root)
		return -ENOMEM;

	// TODO: remove once code stable
	validate_avl_balancing(root);

	return 0;
}


static struct sptree_node *rotate_right_left_generic(struct sptree_node *root, struct sptree_node **proot)
{
	// root = X
	struct sptree_node *right = root->right;	// Z
	struct sptree_node **pright = &root->right;
	//struct sptree_node *left = right->left;	// Y
	struct sptree_node *new_root;			// new Y

	pr_debug("%s: rotate right-left at "NODE_FMT"\n", __func__, NODE_ARG(root));

	// rotate right on Z
	if (!rotate_right_generic(right, pright))
		return NULL;

	// rotate left on X
	new_root = rotate_left_generic(root, proot);
	if (!new_root)
		return NULL;

	pr_debug("%s: rotated right-left, new root is "NODE_FMT"\n",
		__func__, NODE_ARG(new_root));

	return new_root;
}

int sptree_rrl(struct sptree_root *root, unsigned long addr)
{
	struct sptree_node *target, **ptarget;
	struct sptree_node *subtree;

	target = search(root, addr);
	if (!target)
		return -ENXIO;

	// validate the node
	if (!target->right) {
		pr_err("node too low\n");
		return -EINVAL;
	}
	if (!target->right->left) {
		pr_err("node too low\n");
		return -EINVAL;
	}

	// parent may contain L/R flags or NULL
	ptarget = get_pnode(root, target->parent);

	subtree = rotate_right_left_generic(target, ptarget);
	if (!subtree)
		return -ENOMEM;

	// TODO: remove once code stable
	validate_avl_balancing(root);

	return 0;
}


static struct sptree_node *rotate_left_right_generic(struct sptree_node *root, struct sptree_node **proot)
{
	// root = X
	struct sptree_node *left = root->left;		// Z
	struct sptree_node **pleft = &root->left;
	//struct sptree_node *right = left->right;	// Y
	struct sptree_node *new_root;

	pr_debug("%s: rotate left-right at "NODE_FMT"\n", __func__, NODE_ARG(root));

	// rotate left on Z
	if (!rotate_left_generic(left, pleft))
		return NULL;

	// rotate right on X
	new_root = rotate_right_generic(root, proot);
	if (!new_root)
		return NULL;

	pr_debug("%s: rotated left-right, new root is "NODE_FMT"\n",
		__func__, NODE_ARG(new_root));

	return new_root;
}

int sptree_rlr(struct sptree_root *root, unsigned long addr)
{
	struct sptree_node *target, **ptarget;
	struct sptree_node *subtree;

	target = search(root, addr);
	if (!target)
		return -ENXIO;

	// validate the node
	if (!target->left) {
		pr_err("node too low\n");
		return -EINVAL;
	}
	if (!target->left->right) {
		pr_err("node too low\n");
		return -EINVAL;
	}

	// parent may contain L/R flags or NULL
	ptarget = get_pnode(root, target->parent);

	subtree = rotate_left_right_generic(target, ptarget);
	if (!subtree)
		return -ENOMEM;

	// TODO: remove once code stable
	validate_avl_balancing(root);

	return 0;
}
