
#define pr_fmt(fmt)	KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/rcupdate.h>
#include <linux/compiler.h>

#include "tree.h"

int standard_init(struct sptree_root *root, unsigned long start, size_t length)
{
	root->start = start;
	root->length = length;
	root->root = NULL;

	pr_info("%s: created empty root\n", __func__);

	return 0;
}

int interval_init(struct sptree_root *root, unsigned long start, size_t length)
{
	root->root = kmalloc(sizeof(*root->root), GFP_KERNEL);
	if (!root->root)
		return -ENOMEM;

	root->start = start;
	root->length = length;

	memset(root->root, 0, sizeof(*root->root));
	root->root->start = start;
	root->root->length = length;

	pr_info("%s: created root "NODE_FMT"\n", __func__, NODE_ARG(root->root));

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
		pr_warn("%s: excessive balancing on "NODE_FMT", left depth %d, right depth %d\n",
			__func__, NODE_ARG(node), left_depth, right_depth);

	// check if the algorithm computed the balance right
	if (diff != node->balancing)
		pr_warn("%s: wrong balance factor on "NODE_FMT", left depth %d, right depth %d\n",
			__func__, NODE_ARG(node), left_depth, right_depth);

	// return the depth of the subtree rooted at node
	return max(left_depth, right_depth) + 1;
}

static void validate_avl_balancing(struct sptree_root *root)
{
	if (root->root)
		validate_subtree_balancing(root->root);
}



// search for the node containing this address
static struct sptree_node *search(struct sptree_root *root, unsigned long addr)
{
	struct sptree_node *crnt;

	pr_info("%s: looking for %lx\n", __func__, addr);

	for (crnt = root->root; crnt != NULL; ) {
		if (addr >= crnt->start && addr < crnt->start + crnt->length)
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
			} else {
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
			iter->node = strip_flag(next);		// goes up anyway

			if (is_root(next)) {
				iter->state = ITER_DONE;
				return;
			} else if (is_left_child(next)) {
				iter->state = ITER_HANDLED;	// directly handle the node
				return;
			} else {
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
			iter->node = strip_flag(next);		// goes up anyway

			if (is_root(next)) {
				iter->state = ITER_DONE;
				return;
			} else if (is_left_child(next)) {
				iter->state = ITER_LEFT;
				break;
			} else {
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
	BUG_ON(!is_mapping(root));
	BUG_ON(!pivot);
	BUG_ON(!is_mapping(pivot));

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
	BUG_ON(!is_mapping(root));
	BUG_ON(!pivot);
	BUG_ON(!is_mapping(pivot));

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



static struct sptree_node *rotate_right_generic(struct sptree_node *root, struct sptree_node **proot)
{
	// root = X
	struct sptree_node *pivot = root->left;		// Y
	struct sptree_node *new_root;			// new Y
	struct sptree_node *new_pivot;			// new X

	pr_info("%s: rotate right at "NODE_FMT", pivot at "NODE_FMT"\n",
		__func__, NODE_ARG(root), NODE_ARG(pivot));

	new_root = rotate_right_rcu(root, proot);
	if (!new_root)
		return NULL;

	new_pivot = new_root->right;

	// fix balance factors
	if (pivot->balancing >= 0) {
		new_pivot->balancing = root->balancing + 1;
		if (root->balancing <= -1)
			new_root->balancing = pivot->balancing + 1;
		else
			new_root->balancing = root->balancing + pivot->balancing + 2;
	} else {
		new_pivot->balancing = root->balancing - pivot->balancing + 1;
		if (root->balancing <= pivot->balancing - 1)
			new_root->balancing = pivot->balancing + 1;
		else
			new_root->balancing = root->balancing + 2;
	}

	pr_info("%s: rotated right, new root is "NODE_FMT"\n",
		__func__, NODE_ARG(new_root));

	return new_root;
}

int sptree_ror(struct sptree_root *root, unsigned long addr)
{
	struct sptree_node *target, *parent;
	struct sptree_node **ptarget;
	struct sptree_node *pivot;
	struct sptree_node *new_root;

	if (!address_valid(root, addr))
		return -EINVAL;

	target = search(root, addr);
	BUG_ON(!target);
	pivot = target->left;

	// validate nodes
	if (!is_mapping(target)) {
		pr_err("%s: found root "NODE_FMT", not a mapping\n", __func__, NODE_ARG(target));
		return -EINVAL;
	}
	if (!pivot) {
		pr_err("%s: we don't have a pivot for "NODE_FMT"\n", __func__, NODE_ARG(target));
		return -EINVAL;
	}
	if (!is_mapping(pivot)) {
		pr_err("%s: pivot "NODE_FMT" not a mapping\n", __func__, NODE_ARG(pivot));
		return -EINVAL;
	}

	// validate balancing
	//if (target->balancing > 0) {
	//	pr_err("%s: root "NODE_FMT" already right-heavy, can't do\n", __func__, NODE_ARG(target));
	//	return -EINVAL;
	//}
	//if (pivot->balancing > 0) {
	//	pr_err("%s: pivot "NODE_FMT" already right-heavy, can't do\n", __func__, NODE_ARG(target));
	//	return -EINVAL;
	//}

	// may contain L/R flags or NULL
	parent = READ_ONCE(target->parent);
	ptarget = get_pnode(root, parent);

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

	pr_info("%s: rotate left at "NODE_FMT", pivot at "NODE_FMT"\n",
		__func__, NODE_ARG(root), NODE_ARG(pivot));

	new_root = rotate_left_rcu(root, proot);
	if (!new_root)
		return NULL;

	new_pivot = new_root->left;

	// fix balance factors
	if (pivot->balancing <= 0) {
		new_pivot->balancing = root->balancing - 1;
		if (root->balancing >= 1)
			new_root->balancing = pivot->balancing - 1;
		else
			new_root->balancing = root->balancing + pivot->balancing - 2;
	} else {
		new_pivot->balancing = root->balancing - pivot->balancing - 1;
		if (root->balancing >= pivot->balancing + 1)
			new_root->balancing = pivot->balancing - 1;
		else
			new_root->balancing = root->balancing - 2;
	}

	pr_info("%s: rotated left, new root is "NODE_FMT"\n",
		__func__, NODE_ARG(new_root));

	return new_root;
}

int sptree_rol(struct sptree_root *root, unsigned long addr)
{
	struct sptree_node *target, *parent;
	struct sptree_node **ptarget;
	struct sptree_node *pivot;
	struct sptree_node *new_root;

	if (!address_valid(root, addr))
		return -EINVAL;

	target = search(root, addr);
	BUG_ON(!target);
	pivot = target->right;

	// validate nodes
	if (!is_mapping(target)) {
		pr_err("%s: found root "NODE_FMT", not a mapping\n", __func__, NODE_ARG(target));
		return -EINVAL;
	}
	if (!pivot) {
		pr_err("%s: we don't have a pivot for "NODE_FMT"\n", __func__, NODE_ARG(target));
		return -EINVAL;
	}
	if (!is_mapping(pivot)) {
		pr_err("%s: pivot "NODE_FMT" not a mapping\n", __func__, NODE_ARG(pivot));
		return -EINVAL;
	}

	// validate balancing
	//if (target->balancing < 0) {
	//	pr_err("%s: root "NODE_FMT" already left-heavy, can't do\n", __func__, NODE_ARG(target));
	//	return -EINVAL;
	//}
	//if (pivot->balancing < 0) {
	//	pr_err("%s: pivot "NODE_FMT" already left-heavy, can't do\n", __func__, NODE_ARG(target));
	//	return -EINVAL;
	//}

	// may contain L/R flags or NULL
	parent = READ_ONCE(target->parent);
	ptarget = get_pnode(root, parent);

	new_root = rotate_left_generic(target, ptarget);
	if (!new_root)
		return -ENOMEM;

	// TODO: remove once code stable
	validate_avl_balancing(root);

	return 0;
}


/**
 * ror_height_increase() - check if height of tree is modified by rotation
 * @root:	The node rooting a subtree
 *
 * Sometimes a simple rotation causes the overall height of a tree to
 * increase/decrease (by maximum 1).
 * This is important in compound rotations, since the balance of the root has
 * to be modified after the first rotation.
 * Returns -1/0/+1 depending of the modification of the height.
 */
//static int ror_height_increase(struct sptree_node *root)
//{
//	// root = X
//	struct sptree_node *pivot = root->left;		// Y
//	int increase = 0;
//
//	if (pivot->balancing >= 0) {
//		if (root->balancing >= 0)
//			increase = 1;
//	} else {
//		if (root->balancing >= -1)
//			increase = 1;
//		else if (root->balancing <= -2)
//			increase = -1;
//	}
//
//	if (increase == -1)
//		pr_info("%s: height of "NODE_FMT" decreases\n", __func__, NODE_ARG(root));
//	else if (increase == 1)
//		pr_info("%s: height of "NODE_FMT" increases\n", __func__, NODE_ARG(root));
//	else
//		pr_info("%s: height of "NODE_FMT" stays the same\n", __func__, NODE_ARG(root));
//
//	return increase;
//}

/**
 * rol_height_increase() - check if height of tree is modified by rotation
 * @root:	The node rooting a subtree
 *
 * Sometimes a simple rotation causes the overall height of a tree to
 * increase/decrease (by maximum 1).
 * This is important in compound rotations, since the balance of the root has
 * to be modified after the first rotation.
 * Returns -1/0/+1 depending of the modification of the height.
 */
//static int rol_height_increase(struct sptree_node *root)
//{
//	// root = X
//	struct sptree_node *pivot = root->right;	// Y
//	int increase = 0;
//
//	if (pivot->balancing <= 0) {
//		if (root->balancing <= 0)
//			increase = 1;
//	} else {
//		if (root->balancing <= 1)
//			increase = 1;
//		else if (root->balancing >= 2)
//			increase = -1;
//	}
//
//	if (increase == -1)
//		pr_info("%s: height of "NODE_FMT" decreases\n", __func__, NODE_ARG(root));
//	else if (increase == 1)
//		pr_info("%s: height of "NODE_FMT" increases\n", __func__, NODE_ARG(root));
//	else
//		pr_info("%s: height of "NODE_FMT" stays the same\n", __func__, NODE_ARG(root));
//
//	return increase;
//}


static struct sptree_node *rotate_right_left_generic(struct sptree_node *root, struct sptree_node **proot)
{
	// root = X
	struct sptree_node *right = root->right;	// Z
	struct sptree_node **pright = &root->right;
	//struct sptree_node *left = right->left;	// Y
	struct sptree_node *new_root;			// new Y

	pr_info("%s: rotate right-left at "NODE_FMT"\n", __func__, NODE_ARG(root));

	// TODO: astea nu merg bine
	// the next rotation will increase the height of the subtree
	//root->balancing += ror_height_increase(right);

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
	struct sptree_node *target, *parent;
	struct sptree_node **ptarget;
	struct sptree_node *subtree;

	if (!address_valid(root, addr))
		return -EINVAL;

	target = search(root, addr);
	BUG_ON(!target);

	// validate the node
	if (!is_mapping(target)) {
		pr_err("found node "NODE_FMT", not a mapping\n", NODE_ARG(target));
		return -EINVAL;
	}
	if (!target->right) {
		pr_err("node too low\n");
		return -EINVAL;
	}
	if (!is_mapping(target->right)) {
		pr_err("pivot not a mapping\n");
		return -EINVAL;
	}
	if (!target->right->left) {
		pr_err("node too low\n");
		return -EINVAL;
	}
	if (!is_mapping(target->right->left)) {
		pr_err("pivot not a mapping\n");
		return -EINVAL;
	}

	// may contain L/R flags or NULL
	parent = READ_ONCE(target->parent);
	ptarget = get_pnode(root, parent);

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

	// TODO: astea nu merg bine
	// the next rotation will increase the height of the subtree
	//root->balancing -= rol_height_increase(left);

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
	struct sptree_node *target, *parent;
	struct sptree_node **ptarget;
	struct sptree_node *subtree;

	if (!address_valid(root, addr))
		return -EINVAL;

	target = search(root, addr);
	BUG_ON(!target);

	// validate the node
	if (!is_mapping(target)) {
		pr_err("found node "NODE_FMT", not a mapping\n", NODE_ARG(target));
		return -EINVAL;
	}
	if (!target->left) {
		pr_err("node too low\n");
		return -EINVAL;
	}
	if (!is_mapping(target->left)) {
		pr_err("pivot not a mapping\n");
		return -EINVAL;
	}
	if (!target->left->right) {
		pr_err("node too low\n");
		return -EINVAL;
	}
	if (!is_mapping(target->left->right)) {
		pr_err("pivot not a mapping\n");
		return -EINVAL;
	}

	// may contain L/R flags or NULL
	parent = READ_ONCE(target->parent);
	ptarget = get_pnode(root, parent);

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
	if (pivot->balancing == 0) {
		new_root->balancing = 1;
		new_pivot->balancing = -1;
	} else {
		new_pivot->balancing = 0;
		new_root->balancing = 0;
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
	if (pivot->balancing == 0) {
		new_root->balancing = 1;
		new_pivot->balancing = -1;
	} else {
		new_pivot->balancing = 0;
		new_root->balancing = 0;
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
	if (left->balancing > 0) {
		new_left->balancing = -1;
		new_right->balancing = 0;
	}
	else if (left->balancing == 0) {
		new_left->balancing = 0;
		new_right->balancing = 0;
	} else {
		new_left->balancing = 0;
		new_right->balancing = 1;
	}
	new_root->balancing = 0;

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
	if (right->balancing > 0) {
		new_left->balancing = -1;
		new_right->balancing = 0;
	} else  if (right->balancing == 0) {
		new_left->balancing = 0;
		new_right->balancing = 0;
	} else {
		new_left->balancing = 0;
		new_right->balancing = 1;
	}

	new_root->balancing = 0;

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
	struct sptree_node *parent, *gparent;
	struct sptree_node **pparent;

	pr_info("%s: starting at "NODE_FMT"\n", __func__, NODE_ARG(node));

	for (parent = READ_ONCE(node->parent); parent != NULL; node = parent, parent = READ_ONCE(node->parent)) {

		// parent pointer may contain left/right flag
		pr_info("%s: loop on parent "NODE_FMT", node is "NODE_FMT"\n", __func__,
			NODE_ARG(strip_flag(parent)), NODE_ARG(node));

		// may contain L/R flags or NULL
		gparent = READ_ONCE(strip_flag(parent)->parent);
		pparent = get_pnode(root, gparent);

		if (is_left_child(parent)) {			// node is left child of parent
			// fix parent pointer
			parent = strip_flag(parent);

			pr_info("%s: node "NODE_FMT" is left child of "NODE_FMT"\n", __func__,
				NODE_ARG(node), NODE_ARG(parent));

			// parent is left-heavy
			if (parent->balancing < 0) {
				// node is right-heavy
				if (node->balancing > 0)
					parent = rotate_left_right_retrace(parent, pparent);
				else
					parent = rotate_right_retrace(parent, pparent);

				if (!parent)
					return -ENOMEM;
				break;
			}
			// parent is right-heavy
			else if (parent->balancing > 0) {
				parent->balancing = 0;
				pr_info("%s: parent becomes balanced: "NODE_FMT", stop\n",
					__func__, NODE_ARG(parent));
				break;
			}
			// parent is balanced
			else {
				parent->balancing = -1;
				pr_info("%s: parent becomes unbalanced: "NODE_FMT", continue\n",
					__func__, NODE_ARG(parent));
			}
		}
		else {						// node is right child of parent
			// fix parent pointer
			parent = strip_flag(parent);

			pr_info("%s: node "NODE_FMT" is right child of "NODE_FMT"\n", __func__,
				NODE_ARG(node), NODE_ARG(parent));

			// parent is right heavy
			if (parent->balancing > 0) {
				// node is left-heavy
				if (node->balancing < 0)
					parent = rotate_right_left_retrace(parent, pparent);
				else
					parent = rotate_left_retrace(parent, pparent);

				if (!parent)
					return -ENOMEM;
				break;
			}
			// parent is left-heavy
			else if (parent->balancing < 0) {
				parent->balancing = 0;
				pr_info("%s: parent becomes balanced: "NODE_FMT", stop\n",
					__func__, NODE_ARG(parent));
				break;
			}
			// parent is balanced
			else {
				parent->balancing = +1;
				pr_info("%s: parent becomes unbalanced: "NODE_FMT", continue\n",
					__func__, NODE_ARG(parent));
			}
		}
	}

	return 0;
}


/**
 * interval_retrace() - custom retracing for splitting case
 * @root:	Root of the tree
 * @node:	Node where retracing begins
 *
 * This differs from the standard retracing algorithm because a subtree is
 * inserted at the end, and a rotation may not always balance the subtree.
 * So retracing is not stopped after the first rotation.
 *
 * Returns 0 for success or -ENOMEM if rotations fail.
 */
static int interval_retrace(struct sptree_root *root, struct sptree_node *node)
{
	// ...

	return 0;
}

/**
 * interval_alloc_subtree() - creates a subtree centered on addr
 * @node:	A leaf node (interval) from within the tree
 * @addr:	Address where splitting is done
 *
 * The created subtree represents the same interval with a split at @addr.
 * The subtree may be degenerate or just a node if @addr represents
 * the starting/ending page of the interval (or both).
 *
 * Returns the root of the subtree or NULL if allocation failed.
 */
static struct sptree_node *interval_alloc_subtree(struct sptree_node *node, unsigned long addr)
{
	struct sptree_node *subtree_root = NULL;
	struct sptree_node *subtree_left = NULL;
	struct sptree_node *subtree_right = NULL;

	pr_info("%s: splitting "NODE_FMT" at %lx\n", __func__, NODE_ARG(node), addr);

	// a mapping can also be a leaf if it replaced a segment of same size...
	// ...but a segment can't be a node
	BUG_ON(!is_leaf(node));
	BUG_ON(is_mapping(node));

	// this will be the mapping
	subtree_root = kzalloc(sizeof(*subtree_root), GFP_ATOMIC);
	if (!subtree_root)
		return NULL;

	subtree_root->start = addr;
	subtree_root->length = PAGE_SIZE;
	subtree_root->mapping = true;

	// first page in range
	if (addr != node->start) {
		subtree_left = kzalloc(sizeof(*subtree_left), GFP_ATOMIC);
		if (!subtree_left)
			goto error;

		subtree_left->start = node->start;
		subtree_left->length = addr - node->start;

		subtree_root->balancing -= 1;
		subtree_root->left = subtree_left;
		subtree_left->parent = make_left(subtree_root);

		pr_info("%s: to the left we have "NODE_FMT"\n", __func__, NODE_ARG(subtree_left));
	}

	// last page in range
	if (addr != node->start + node->length - PAGE_SIZE) {
		subtree_right = kzalloc(sizeof(*subtree_right), GFP_ATOMIC);
		if (!subtree_right)
			goto error;

		subtree_right->start = addr + PAGE_SIZE;
		subtree_right->length = node->length - (addr - node->start + PAGE_SIZE);

		subtree_root->balancing += 1;
		subtree_root->right = subtree_right;
		subtree_right->parent = make_right(subtree_root);

		pr_info("%s: to the right we have "NODE_FMT"\n", __func__, NODE_ARG(subtree_right));
	}

	return subtree_root;

error:
	if (subtree_root)
		kfree(subtree_root);
	if (subtree_left)
		kfree(subtree_left);
	if (subtree_right)
		kfree(subtree_right);

	return NULL;
}

/**
 * interval_insert() - inserts a mapping in an address interval
 * @root:	The root of the tree
 * @addr:	Address where splitting is done
 *
 * Finds the node containing @addr and splits it into a subtree.
 * The subtree replaces the node in a RCU-safe manner.
 *
 * Returns 0 on success or -E... on failure.
 */
int interval_insert(struct sptree_root *root, unsigned long addr)
{
	struct sptree_node *target, *parent;
	struct sptree_node **ptarget;
	struct sptree_node *subtree;

	if (!address_valid(root, addr))
		return -EINVAL;

	target = search(root, addr);
	BUG_ON(!target);

	// validate the node
	if (is_mapping(target))
		return -EALREADY;

	// alloc & insert subtree
	subtree = interval_alloc_subtree(target, addr);
	if (!subtree)
		return -ENOMEM;

	// may contain L/R flags or NULL
	parent = READ_ONCE(target->parent);
	ptarget = get_pnode(root, parent);

	// reverse, then direct link
	WRITE_ONCE(subtree->parent, parent);
	WRITE_ONCE(*ptarget, subtree);

	// rebalance tree if the subtree has depth != 1
	if (!is_leaf(subtree))
		interval_retrace(root, subtree);

	// TODO: remove once code stable
	validate_avl_balancing(root);

	// finally free the replaced node
	kfree_rcu(target, rcu);

	return 0;
}

/**
 * standard_insert() - inserts a standard node in an AVL tree
 * @root:	The root of the tree
 * @addr:	Address of the node
 *
 * Inserts a node in a tree at @addr. The insertion is RCU-safe
 * by default. Marks it as a mapping for backward compliance.
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
		} else {
			parent = make_right(crnt);
			pparent = &crnt->right;
			crnt = crnt->right;
		}
	}

	node = kzalloc(sizeof(*node), GFP_ATOMIC);
	if (!node)
		return -ENOMEM;

	node->start = addr;
	node->length = PAGE_SIZE;
	node->mapping = true;

	// reverse, then direct link
	WRITE_ONCE(node->parent, parent);
	WRITE_ONCE(*pparent, node);

	standard_retrace(root, node);

	// TODO: remove once code stable
	validate_avl_balancing(root);

	return 0;
}

/**
 * sptree_delete() - deletes a mapping from an address interval
 * @root:	The root of the tree
 * @addr:	Mapping address
 *
 * Finds the mapping at @addr and merges it with the successor
 * and/or predesessor intervals to form a new interval.
 * The new interval (a node) replaces the subtree centered at
 * @addr in a RCU-safe manner.
 *
 * Returns 0 on success or -E... on failure.
 */
int sptree_delete(struct sptree_root *root, unsigned long addr)
{
	// TODO: ...



	return 0;
}
