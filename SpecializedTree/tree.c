
#define pr_fmt(fmt)	KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/rcupdate.h>
#include <linux/compiler.h>

#include "tree.h"

int standard_init(struct sptree_root *root)
{
	root->root = NULL;

	pr_info("%s: created empty root\n", __func__);

	return 0;
}

// recursive post-order free function
static void postorder_free(struct sptree_node *node)
{
	if (!node)
		return;

	if (node->left)
		postorder_free(node->left);

	if (node->right)
		postorder_free(node->right);

	kfree(node);
}

void sptree_free(struct sptree_root *root)
{
	struct sptree_node *tree;

	tree = root->root;
	root->root = NULL;

	synchronize_rcu();

	// post-order traversal and remove the nodes
	postorder_free(tree);
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
struct sptree_node *search(struct sptree_root *root, unsigned long addr)
{
	struct sptree_node *crnt;

	pr_info("%s: looking for %lx\n", __func__, addr);

	for (crnt = root->root; crnt != NULL; ) {
		if (addr == crnt->start)
			break;
		else if (addr < crnt->start)
			crnt = crnt->left;
		else
			crnt = crnt->right;
	}

	if (crnt == NULL)
		pr_info("%s: found nothing\n", __func__);
	else
		pr_info("%s: found inside "NODE_FMT"\n", __func__, NODE_ARG(crnt));

	return crnt;
}

struct sptree_node *sptree_search(struct sptree_root *root, unsigned long addr)
{
	struct sptree_node *result;

	rcu_read_lock();
	result = search(root, addr);
	rcu_read_unlock();

	return result;
}



void sptree_iter_first_io(struct sptree_root *root, struct sptree_iterator *iter)
{
	if (unlikely(root->root == NULL)) {
		iter->node = NULL;
		iter->state = ITER_DONE;
		return;
	}

	iter->node = root->root;
	iter->state = ITER_UP;

	sptree_iter_next_io(iter);
}

void sptree_iter_next_io(struct sptree_iterator *iter)
{
	struct sptree_node *next;

	// INFO: left/right pointers of a node are overwritten by
	// WRITE_ONCE(*pnode, pointer) so they must be READ_ONCE()

	while (iter->node) {
		switch (iter->state) {
		case ITER_UP:					// comes from parent...
			next = READ_ONCE(iter->node->left);	// goes down left subtree
			if (next) {
				iter->node = next;		// state stays the same
				break;
			}
			else {
				iter->state = ITER_HANDLED;	// no left subtree, will handle the node
				return;
			}

		case ITER_HANDLED:				// node has just been handled...
			next = READ_ONCE(iter->node->right);	// goes down right subtree
			if (next) {
				iter->node = next;
				iter->state = ITER_UP;
				break;
			} // else fallback to the next case

		case ITER_RIGHT:				// comes from right subtree / handled case
			next = READ_ONCE(iter->node->parent);	// may contain L/R flags
			iter->node = strip_flags(next);		// goes up anyway

			if (is_root(next)) {
				iter->state = ITER_DONE;
				return;
			}
			else if (is_left_child(next)) {
				iter->state = ITER_HANDLED;	// directly handle the node
				return;
			}
			else {
				// we may be here coming from ITER_HANDLED case
				iter->state = ITER_RIGHT;	// right subtree has been handled
				break;
			}

		default:
			pr_warn("%s: unhandled iterator state\n", __func__);
			iter->node = NULL;			// cancels iteration
			return;
		}
	}
}


void sptree_iter_first_po(struct sptree_root *root, struct sptree_iterator *iter)
{
	if (unlikely(root->root == NULL)) {
		iter->node = NULL;
		iter->state = ITER_DONE;
		return;
	}

	iter->node = root->root;
	iter->state = ITER_HANDLED;
}

void sptree_iter_next_po(struct sptree_iterator *iter)
{
	struct sptree_node *next;

	while (iter->node) {
		switch (iter->state)
		{
		case ITER_HANDLED:
			next = READ_ONCE(iter->node->left);
			if (next) {
				iter->node = next;
				iter->state = ITER_HANDLED;
				return;
			}

		case ITER_LEFT:
			next = READ_ONCE(iter->node->right);
			if (next) {
				iter->node = next;
				iter->state = ITER_HANDLED;
				return;
			}

		case ITER_RIGHT:
			next = READ_ONCE(iter->node->parent);	// may contain L/R flags
			iter->node = strip_flags(next);		// goes up anyway

			if (is_root(next)) {
				iter->state = ITER_DONE;
				return;
			}
			else if (is_left_child(next)) {
				iter->state = ITER_LEFT;
				break;
			}
			else {
				iter->state = ITER_RIGHT;
				break;
			}

		default:
			pr_warn("%s: unhandled iterator state\n", __func__);
			iter->node = NULL;			// cancels iteration
			return;
		}

	}
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

	pr_info("%s: rotate right at "NODE_FMT", pivot at "NODE_FMT"\n",
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
	WRITE_ONCE(*proot, new_root);

	// link parent pointers
	if (pivot->left)
		WRITE_ONCE(pivot->left->parent, make_left(new_root));
	if (pivot->right)
		WRITE_ONCE(pivot->right->parent, make_left(new_pivot));
	if (root->right)
		WRITE_ONCE(root->right->parent, make_right(new_pivot));

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

	pr_info("%s: rotate left at "NODE_FMT", pivot at "NODE_FMT"\n",
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
	WRITE_ONCE(*proot, new_root);

	// link parent pointers
	if (root->left)
		WRITE_ONCE(root->left->parent, make_left(new_pivot));
	if (pivot->left)
		WRITE_ONCE(pivot->left->parent, make_right(new_pivot));
	if (pivot->right)
		WRITE_ONCE(pivot->right->parent, make_right(new_root));

	// free old nodes
	kfree_rcu(root, rcu);
	kfree_rcu(pivot, rcu);

	return new_root;
}

/**
 * rotate_right_left_rcu() - core compound rotation
 * @root:	The node rooting a subtree
 * @proot:	Address of pointer to @root (for changing parent)
 *		(root->root / parent->left / parent->right)
 *
 * Rotates the subtree rooted by @root in a RCU-safe manner.
 * The rotation does not break search or in-order walk operations.
 * Does not do balance factor fixing.
 * The rotation can fail in the middle if the second rotation fails.
 * In this case the tree remains unbalanced and we must shut down.
 *
 * Returns pointer to the new root of the subtree or NULL.
 */
static struct sptree_node *rotate_right_left_rcu(struct sptree_node *root, struct sptree_node **proot)
{
	// root = X
	struct sptree_node *right = root->right;	// Z
	struct sptree_node **pright = &root->right;
	//struct sptree_node *left = right->left;	// Y
	struct sptree_node *new_root;

	pr_info("%s: rotate right-left at "NODE_FMT"\n", __func__, NODE_ARG(root));

	// rotate right on Z
	if (!rotate_right_rcu(right, pright))
		return NULL;

	// rotate left on X
	new_root = rotate_left_rcu(root, proot);
	if (!new_root)
		return NULL;

	return new_root;
}

/**
 * rotate_left_right_rcu() - core compound rotation
 * @root:	The node rooting a subtree
 * @proot:	Address of pointer to @root (for changing parent)
 *		(root->root / parent->left / parent->right)
 *
 * Rotates the subtree rooted by @root in a RCU-safe manner.
 * The rotation does not break search or in-order walk operations.
 * Does not do balance factor fixing.
 * The rotation can fail in the middle if the second rotation fails.
 * In this case the tree remains unbalanced and we must shut down.
 *
 * Returns pointer to the new root of the subtree or NULL.
 */
static struct sptree_node *rotate_left_right_rcu(struct sptree_node *root, struct sptree_node **proot)
{
	// root = X
	struct sptree_node *left = root->left;		// Z
	struct sptree_node **pleft = &root->left;
	//struct sptree_node *right = left->right;	// Y
	struct sptree_node *new_root;

	pr_info("%s: rotate left-right at "NODE_FMT"\n", __func__, NODE_ARG(root));

	// rotate left on Z
	if (!rotate_left_rcu(left, pleft))
		return NULL;

	// rotate right on X
	new_root = rotate_right_rcu(root, proot);
	if (!new_root)
		return NULL;

	return new_root;
}

/**
 * delete_leaf_rcu() - core leaf deletion
 * @root:	The node rooting a subtree
 * @proot:	Address of pointer to @root (for changing parent)
 *		(root->root / parent->left / parent->right)
 *
 * Deletes the @root in a RCU-safe manner.
 * Does not do balance factor fixing.
 */
static void delete_leaf_rcu(struct sptree_node *root, struct sptree_node **proot)
{
	pr_info("%s: deleting at "NODE_FMT"\n", __func__, NODE_ARG(root));

	BUG_ON(!is_leaf(root));

	// unlink this leaf
	WRITE_ONCE(*proot, NULL);

	// free the leaf
	kfree_rcu(root, rcu);
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
		pr_info("%s: height of "NODE_FMT" decreases\n", __func__, NODE_ARG(root));
	else if (diff == 1)
		pr_info("%s: height of "NODE_FMT" increases\n", __func__, NODE_ARG(root));
	else
		pr_info("%s: height of "NODE_FMT" stays the same\n", __func__, NODE_ARG(root));

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
		pr_info("%s: height of "NODE_FMT" decreases\n", __func__, NODE_ARG(root));
	else if (diff == 1)
		pr_info("%s: height of "NODE_FMT" increases\n", __func__, NODE_ARG(root));
	else
		pr_info("%s: height of "NODE_FMT" stays the same\n", __func__, NODE_ARG(root));

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

		pr_info("%s: updated balance factor for "NODE_FMT"\n",
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

	pr_info("%s: rotate right at "NODE_FMT", pivot at "NODE_FMT"\n",
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

	pr_info("%s: rotated right, new root is "NODE_FMT"\n",
		__func__, NODE_ARG(new_root));

	return new_root;
}

int sptree_ror(struct sptree_root *root, unsigned long addr)
{
	struct sptree_node *target, **ptarget;
	struct sptree_node *pivot;
	struct sptree_node *new_root;

	if (!address_valid(root, addr))
		return -EINVAL;

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

	pr_info("%s: rotate left at "NODE_FMT", pivot at "NODE_FMT"\n",
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

	pr_info("%s: rotated left, new root is "NODE_FMT"\n",
		__func__, NODE_ARG(new_root));

	return new_root;
}

int sptree_rol(struct sptree_root *root, unsigned long addr)
{
	struct sptree_node *target, **ptarget;
	struct sptree_node *pivot;
	struct sptree_node *new_root;

	if (!address_valid(root, addr))
		return -EINVAL;

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

	pr_info("%s: rotate right-left at "NODE_FMT"\n", __func__, NODE_ARG(root));

	// rotate right on Z
	if (!rotate_right_generic(right, pright))
		return NULL;

	// rotate left on X
	new_root = rotate_left_generic(root, proot);
	if (!new_root)
		return NULL;

	pr_info("%s: rotated right-left, new root is "NODE_FMT"\n",
		__func__, NODE_ARG(new_root));

	return new_root;
}

int sptree_rrl(struct sptree_root *root, unsigned long addr)
{
	struct sptree_node *target, **ptarget;
	struct sptree_node *subtree;

	if (!address_valid(root, addr))
		return -EINVAL;

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

	pr_info("%s: rotate left-right at "NODE_FMT"\n", __func__, NODE_ARG(root));

	// rotate left on Z
	if (!rotate_left_generic(left, pleft))
		return NULL;

	// rotate right on X
	new_root = rotate_right_generic(root, proot);
	if (!new_root)
		return NULL;

	pr_info("%s: rotated left-right, new root is "NODE_FMT"\n",
		__func__, NODE_ARG(new_root));

	return new_root;
}

int sptree_rlr(struct sptree_root *root, unsigned long addr)
{
	struct sptree_node *target, **ptarget;
	struct sptree_node *subtree;

	if (!address_valid(root, addr))
		return -EINVAL;

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


/**
 * rotate_right_retrace() - helper for retracing
 * @root:	The node rooting a subtree
 * @proot:	Address of pointer to @root (for changing parent)
 *		(root->root / parent->left / parent->right)
 *
 * Called by ..._retrace() family after insertion/deletion.
 * Assumes that the nodes operated on meet AVL tree invariants.
 * No further checks are done.
 *
 * Returns pointer to the new root of the subtree or NULL.
 */
static struct sptree_node *rotate_right_retrace(struct sptree_node *root, struct sptree_node **proot)
{
	struct sptree_node *pivot = root->left;
	struct sptree_node *new_root;
	struct sptree_node *new_pivot;

	pr_info("%s: rotate right at "NODE_FMT", pivot at "NODE_FMT"\n",
		__func__, NODE_ARG(root), NODE_ARG(pivot));

	new_root = rotate_right_rcu(root, proot);
	if (!new_root)
		return NULL;

	new_pivot = new_root->right;

	// fix balance factors
	if (pivot->balance == 0) {
		new_root->balance = 1;
		new_pivot->balance = -1;
	}
	else {
		new_pivot->balance = 0;
		new_root->balance = 0;
	}

	pr_info("%s: rotated right, new root is "NODE_FMT"\n",
		__func__, NODE_ARG(new_root));

	return new_root;
}

/**
 * rotate_left_retrace() - helper for retracing
 * @root:	The node rooting a subtree
 * @proot:	Address of pointer to @root (for changing parent)
 *		(root->root / parent->left / parent->right)
 *
 * Called by ..._retrace() family after insertion/deletion.
 * Assumes that the nodes operated on meet AVL tree invariants.
 * No further checks are done.
 *
 * Returns pointer to the new root of the subtree or NULL.
 */
static struct sptree_node *rotate_left_retrace(struct sptree_node *root, struct sptree_node **proot)
{
	struct sptree_node *pivot = root->right;
	struct sptree_node *new_root;
	struct sptree_node *new_pivot;

	pr_info("%s: rotate left at "NODE_FMT", pivot at "NODE_FMT"\n",
		__func__, NODE_ARG(root), NODE_ARG(pivot));

	new_root = rotate_left_rcu(root, proot);
	if (!new_root)
		return NULL;

	new_pivot = new_root->left;

	// fix balance factors
	if (pivot->balance == 0) {
		new_root->balance = 1;
		new_pivot->balance = -1;
	}
	else {
		new_pivot->balance = 0;
		new_root->balance = 0;
	}

	pr_info("%s: rotated left, new root is "NODE_FMT"\n",
		__func__, NODE_ARG(new_root));

	return new_root;
}

/**
 * rotate_right_left_retrace() - helper for retracing
 * @root:	The node rooting a subtree
 * @proot:	Address of pointer to @root (for changing parent)
 *		(root->root / parent->left / parent->right)
 *
 * Called by ..._retrace() family after insertion/deletion.
 * Assumes that the nodes operated on meet AVL tree invariants.
 * No further checks are done.
 *
 * Returns pointer to the new root of the subtree or NULL.
 */
static struct sptree_node *rotate_right_left_retrace(struct sptree_node *root, struct sptree_node **proot)
{
	// root = X
	struct sptree_node *right = root->right;	// Z
	struct sptree_node *left = right->left;		// Y
	struct sptree_node *new_root;			// new Y
	struct sptree_node *new_left;			// new X
	struct sptree_node *new_right;			// new Z

	pr_info("%s: rotate right-left at "NODE_FMT"\n", __func__, NODE_ARG(root));

	new_root = rotate_right_left_rcu(root, proot);
	if (!new_root)
		return NULL;

	new_left = new_root->left;
	new_right = new_root->right;

	// fix balance factors
	if (left->balance > 0) {
		new_left->balance = -1;
		new_right->balance = 0;
	}
	else if (left->balance == 0) {
		new_left->balance = 0;
		new_right->balance = 0;
	}
	else {
		new_left->balance = 0;
		new_right->balance = 1;
	}
	new_root->balance = 0;

	pr_info("%s: rotated right-left, new root is "NODE_FMT"\n",
		__func__, NODE_ARG(new_root));

	return new_root;
}

/**
 * rotate_left_right_retrace() - helper for retracing
 * @root:	The node rooting a subtree
 * @proot:	Address of pointer to @root (for changing parent)
 *		(root->root / parent->left / parent->right)
 *
 * Called by ..._retrace() family after insertion/deletion.
 * Assumes that the nodes operated on meet AVL tree invariants.
 * No further checks are done.
 *
 * Returns pointer to the new root of the subtree or NULL.
 */
static struct sptree_node *rotate_left_right_retrace(struct sptree_node *root, struct sptree_node **proot)
{
	// root = X
	struct sptree_node *left = root->left;		// Z
	struct sptree_node *right = left->right;	// Y
	struct sptree_node *new_root;			// new Y
	struct sptree_node *new_left;			// new Z
	struct sptree_node *new_right;			// new X

	pr_info("%s: rotate left-right at "NODE_FMT"\n", __func__, NODE_ARG(root));

	new_root = rotate_left_right_rcu(root, proot);
	if (!new_root)
		return NULL;

	new_left = new_root->left;
	new_right = new_root->right;

	// fix balance factors
	if (right->balance > 0) {
		new_left->balance = -1;
		new_right->balance = 0;
	}
	else  if (right->balance == 0) {
		new_left->balance = 0;
		new_right->balance = 0;
	}
	else {
		new_left->balance = 0;
		new_right->balance = 1;
	}

	new_root->balance = 0;

	pr_info("%s: rotated left-right, new root is "NODE_FMT"\n",
		__func__, NODE_ARG(new_root));

	return new_root;
}

/**
 * standard_retrace() - retracing after insert
 * @root:	Root of the tree
 * @node:	Node where retracing begins
 *
 * Called after a node insertion increases the height of the (sub)tree.
 *
 * Returns 0 for success or -ENOMEM if rotations fail.
 */
static int standard_retrace(struct sptree_root *root, struct sptree_node *node)
{
	struct sptree_node *parent, **pparent;

	pr_info("%s: starting at "NODE_FMT"\n", __func__, NODE_ARG(node));

	for (parent = node->parent; parent != NULL; node = parent, parent = node->parent) {

		// parent pointer may contain left/right flag
		pr_info("%s: loop on parent "NODE_FMT", node is "NODE_FMT"\n", __func__,
			NODE_ARG(strip_flags(parent)), NODE_ARG(node));

		// grandparent, may contain L/R flags or NULL
		pparent = get_pnode(root, strip_flags(parent)->parent);

		if (is_left_child(parent)) {			// node is left child of parent
			// fix parent pointer
			parent = strip_flags(parent);

			pr_info("%s: node "NODE_FMT" is left child of "NODE_FMT"\n", __func__,
				NODE_ARG(node), NODE_ARG(parent));

			// parent is left-heavy
			if (parent->balance < 0) {
				// node is right-heavy
				if (node->balance > 0)
					parent = rotate_left_right_retrace(parent, pparent);
				else
					parent = rotate_right_retrace(parent, pparent);

				if (!parent)
					return -ENOMEM;
				break;
			}
			// parent is right-heavy
			else if (parent->balance > 0) {
				parent->balance = 0;
				pr_info("%s: parent becomes balanced: "NODE_FMT", stop\n",
					__func__, NODE_ARG(parent));
				break;
			}
			// parent is balanced
			else {
				parent->balance = -1;
				pr_info("%s: parent becomes unbalanced: "NODE_FMT", continue\n",
					__func__, NODE_ARG(parent));
			}
		}
		else {						// node is right child of parent
			// fix parent pointer
			parent = strip_flags(parent);

			pr_info("%s: node "NODE_FMT" is right child of "NODE_FMT"\n", __func__,
				NODE_ARG(node), NODE_ARG(parent));

			// parent is right heavy
			if (parent->balance > 0) {
				// node is left-heavy
				if (node->balance < 0)
					parent = rotate_right_left_retrace(parent, pparent);
				else
					parent = rotate_left_retrace(parent, pparent);

				if (!parent)
					return -ENOMEM;
				break;
			}
			// parent is left-heavy
			else if (parent->balance < 0) {
				parent->balance = 0;
				pr_info("%s: parent becomes balanced: "NODE_FMT", stop\n",
					__func__, NODE_ARG(parent));
				break;
			}
			// parent is balanced
			else {
				parent->balance = +1;
				pr_info("%s: parent becomes unbalanced: "NODE_FMT", continue\n",
					__func__, NODE_ARG(parent));
			}
		}
	}

	return 0;
}

/**
 * standard_insert() - inserts a standard node in an AVL tree
 * @root:	The root of the tree
 * @addr:	Address of the node
 *
 * Inserts a node in a tree at @addr. The insertion is RCU-safe
 * by default.
 *
 * Returns 0 on success or -E... on failure.
 */
int standard_insert(struct sptree_root *root, unsigned long addr)
{
	struct sptree_node *crnt, *parent;
	struct sptree_node **pparent;
	struct sptree_node *node;

	if (!address_valid(root, addr))
		return -EINVAL;

	/* look for a parent */
	for (crnt = root->root, parent = NULL, pparent = &root->root; crnt != NULL; ) {
		if (addr == crnt->start)
			return -EINVAL;
		else if (addr < crnt->start) {
			parent = make_left(crnt);
			pparent = &crnt->left;
			crnt = crnt->left;
		}
		else {
			parent = make_right(crnt);
			pparent = &crnt->right;
			crnt = crnt->right;
		}
	}

	node = kzalloc(sizeof(*node), GFP_ATOMIC);
	if (!node)
		return -ENOMEM;

	node->start = addr;

	// reverse, then direct link
	WRITE_ONCE(node->parent, parent);
	WRITE_ONCE(*pparent, node);

	standard_retrace(root, node);

	// TODO: remove once code stable
	validate_avl_balancing(root);

	return 0;
}


static struct sptree_node *unwind_left(struct sptree_root *root, struct sptree_node *target)
{
	struct sptree_node *pivot, **ptarget;
	struct sptree_node *subtree_root;

	pr_info("%s: at "NODE_FMT"\n", __func__, NODE_ARG(target));

	pivot = target->right;
	BUG_ON(!pivot);

	// parent may contain L/R flags or NULL
	ptarget = get_pnode(root, target->parent);

	if (pivot->balance == -1)
		subtree_root = rotate_right_left_generic(target, ptarget);
	else
		subtree_root = rotate_left_generic(target, ptarget);

	// rotation functions return the new subtree root
	target = subtree_root->left;
	return target;
}

static struct sptree_node *unwind_right(struct sptree_root *root, struct sptree_node *target)
{
	struct sptree_node *pivot, **ptarget;
	struct sptree_node *subtree_root;

	pr_info("%s: at "NODE_FMT"\n", __func__, NODE_ARG(target));

	pivot = target->left;
	BUG_ON(!pivot);

	// parent may contain L/R flags or NULL
	ptarget = get_pnode(root, target->parent);

	if (pivot->balance == 1)
		subtree_root = rotate_left_right_generic(target, ptarget);
	else
		subtree_root = rotate_right_generic(target, ptarget);

	// rotation functions return the new subtree root
	target = subtree_root->right;
	return target;
}

static struct sptree_node *reverse_rrl(struct sptree_root *root, struct sptree_node *target)
{
	struct sptree_node **ptarget;
	struct sptree_node *subtree_root;

	pr_info("%s: at "NODE_FMT"\n", __func__, NODE_ARG(target));

	// parent may contain L/R flags or NULL
	ptarget = get_pnode(root, target->parent);

	// rotation functions return the new subtree root
	subtree_root = rotate_right_generic(target, ptarget);
	target = subtree_root->right;

	// parent may contain L/R flags or NULL
	ptarget = get_pnode(root, target->parent);

	// rotation functions return the new subtree root
	subtree_root = rotate_left_generic(target, ptarget);
	target = subtree_root->left;

	return target;
}

static struct sptree_node *reverse_rlr(struct sptree_root *root, struct sptree_node *target)
{
	struct sptree_node **ptarget;
	struct sptree_node *subtree_root;

	pr_info("%s: at "NODE_FMT"\n", __func__, NODE_ARG(target));

	// parent may contain L/R flags or NULL
	ptarget = get_pnode(root, target->parent);

	// rotation functions return the new subtree root
	subtree_root = rotate_left_generic(target, ptarget);
	target = subtree_root->left;

	// parent may contain L/R flags or NULL
	ptarget = get_pnode(root, target->parent);

	// rotation functions return the new subtree root
	subtree_root = rotate_right_generic(target, ptarget);
	target = subtree_root->right;

	return target;
}

static struct sptree_node *unwind_double(struct sptree_root *root, struct sptree_node *target)
{
	struct sptree_node *left, *right;

	pr_info("%s: at "NODE_FMT"\n", __func__, NODE_ARG(target));

	// a leaf may be balanced, but we're interested in a balanced subtree
	BUG_ON(is_leaf(target));

	left = target->left;
	right = target->right;

	// choose direction based on the balance of the subtrees
	// a rotation will give the new pivot the nearest subtree of the old pivot

	// reverse RRL
	if (left->balance == -1)
		return reverse_rrl(root, target);

	// reverse RLR
	if (right->balance == 1)
		return reverse_rlr(root, target);

	// don't care: so reverse RRL
	if (right->balance == 0 && left->balance == 0)
		return reverse_rrl(root, target);

	// both poorly balanced: rebalance one of them
	if (right->balance == -1 && left->balance == 1) {
		// rebalance the right subtree, then apply case 2
		rotate_right_generic(right, &target->right);
		return reverse_rlr(root, target);
	}

	// rebalance left
	if (right->balance == 0 && left->balance == 1) {
		// rebalance the left subtree, then apply case 1
		rotate_left_generic(left, &target->left);
		return reverse_rrl(root, target);
	}

	// rebalance right
	if (right->balance == -1 && left->balance == 0) {
		// rebalance the right subtree, then apply case 2
		rotate_right_generic(right, &target->right);
		return reverse_rlr(root, target);
	}

	pr_err("%s: invalid case at "NODE_FMT", left "NODE_FMT", right "NODE_FMT"\n",
		__func__, NODE_ARG(target), NODE_ARG(left), NODE_ARG(right));
	return NULL;
}

/**
 * unwind_avl() - the unwind algorith
 * @root:	Root of the tree
 * @target:	Node where fixing begins
 *
 * Bubbles the target node down the tree to a leaf position where it can be deleted.
 * The node has to take the shortest path, to minimize the number of rotations.
 *
 * TODO: Returns 0 for success or -ENOMEM if rotations fail.
 */
static struct sptree_node *unwind_avl(struct sptree_root *root, struct sptree_node *target)
{
	pr_info("%s: at "NODE_FMT"\n", __func__, NODE_ARG(target));

	do {
		// each unwinding step must start with a normal balance
		BUG_ON(target->balance < -1 || target->balance > 1);

		// each of these functions will return NULL on error
		switch (target->balance) {
		case -1:
			target = unwind_right(root, target);
			break;

		case 0:
			target = unwind_double(root, target);
			break;

		case 1:
			target = unwind_left(root, target);
			break;

		default:
			pr_err("%s: invalid case at "NODE_FMT"\n", __func__, NODE_ARG(target));
			return NULL;
		}

		// TODO: remove once code stable
		validate_avl_balancing(root);
	} while (target != NULL && !is_leaf(target));

	return target;
}

/**
 * fix_avl() - fix the unbalancing caused by the unwinding algorithm
 * @root:	Root of the tree
 * @target:	Node where fixing begins (parent of deleted node)
 * @stop:	Node where fixing ends (NULL for root)
 *
 * Walks up the branch & undoes any reverse double rotations done by unwind.
 * The walk can stop at the previous parent of the deleted node, everything
 * above is unaffected.
 *
 * TODO: Returns 0 for success or -ENOMEM if rotations fail.
 * TODO: If the subtree whose root is being deleted changes height,
 * this change must propagate above the previous parent !!!!
 */
static void fix_avl(struct sptree_root *root, struct sptree_node *target, struct sptree_node *stop)
{
	struct sptree_node **ptarget;

	for (; target != stop; target = strip_flags(target->parent)) {
		BUG_ON(is_root(target));

		pr_info("%s: currently on target "NODE_FMT"\n",
			__func__, NODE_ARG(target));

		if (target->balance >= -1 && target->balance <= 1)
			continue;

		// parent may contain L/R flags or NULL
		ptarget = get_pnode(root, target->parent);

		if (target->balance == -2)
			target = rotate_left_right_generic(target, ptarget);
		else
			target = rotate_right_left_generic(target, ptarget);
	}
}

/**
 * delete_leaf() - helper for leaf deleting
 * @root:	The node rooting a subtree (the leaf itself)
 * @proot:	Address of pointer to @root (for changing parent)
 *		(root->root / parent->left / parent->right)
 *
 * Called after the node to be deleted is moved down the tree.
 * In the end the node represents an empty subtree rooted by itself (depth 1).
 * Since it goes away, the branch containing it must change balance.
 */
static void delete_leaf(struct sptree_root *root, struct sptree_node *target)
{
	struct sptree_node **ptarget;

	pr_info("%s: deleting at "NODE_FMT"\n", __func__, NODE_ARG(target));

	BUG_ON(!is_leaf(target));

	// go up the branch & change balance
	propagate_height_diff(target, -1);

	// parent may contain L/R flags or NULL
	ptarget = get_pnode(root, target->parent);

	delete_leaf_rcu(target, ptarget);
}

/**
 * standard_delete() - deletes a node from the tree
 * @root:	The root of the tree
 * @addr:	Address of the node
 *
 * Finds the node at @addr and moves it to the bottom of the tree
 * through successive rotations. Then deletes it (RCU safe).
 * The AVL properties of the tree may be broken in the process,
 * but are fixed after deletion.
 *
 * Returns 0 on success or -E... on failure.
 */
int standard_delete(struct sptree_root *root, unsigned long addr)
{
	struct sptree_node *target;
	struct sptree_node *parent_before, *parent_after;

	if (!address_valid(root, addr))
		return -EINVAL;

	target = search(root, addr);
	if (!target)
		return -ENXIO;

	// parent before unwinding
	parent_before = strip_flags(target->parent);

	// unwinding algorithm, target goes down the tree
	if (!is_leaf(target))
		target = unwind_avl(root, target);

	// call the internal delete function
	delete_leaf(root, target);

	// walk up the branch & fix the tree
	parent_after = strip_flags(target->parent);
	if (!is_root(parent_after))
		fix_avl(root, parent_after, parent_before);

	// TODO: remove once code stable
	validate_avl_balancing(root);

	return 0;
}
