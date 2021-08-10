
#define pr_fmt(fmt)	KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/rcupdate.h>
#include <linux/compiler.h>

#include "internal.h"

#define ASSERT(_expr) BUG_ON(!(_expr))


void sptree_ctxt_init(struct sptree_ctxt *ctxt, struct sptree_root *root)
{
	ctxt->root = root;
	init_llist_head(&ctxt->old);
	ctxt->diff = 0;
}


static struct sptree_node *prealloc_left_deepest(struct sptree_node *node)
{
	ASSERT(is_new_branch(node));

	for (;;) {
		if (node->left && is_new_branch(node->left))
			node = node->left;
		else if (node->right && is_new_branch(node->right))
			node = node->right;
		else
			return node;
	}
}

static struct sptree_node *prealloc_next_postorder(struct sptree_node *node)
{
	struct sptree_node *parent;

	/* iteration macro does this */
	if (!node)
		return NULL;

	/* NULL if root, otherwise node on the old/new branch */
	parent = get_parent(node);
	if (is_root(parent) || !is_new_branch(parent))
		return NULL;

	/* LRN, next is right sibling, otherwise go to parent */
	if (is_left_child(node->parent) && parent->right && is_new_branch(parent->right))
		return prealloc_left_deepest(parent->right);
	else
		return parent;
}

/* the preallocated branch/subtree doesn't have a root, just a root node */
static struct sptree_node *prealloc_first_postorder(struct sptree_node *node)
{
	if (!node)
		return NULL;

	ASSERT(is_new_branch(node));

	return prealloc_left_deepest(node);
}

/* iterate post-order only on the preallocated branch */
#define sptree_for_each_prealloc_po(pos, root)	\
	for (pos = prealloc_first_postorder(root);	\
	     pos;					\
	     pos = prealloc_next_postorder(pos))

#define sptree_for_each_prealloc_po_safe(pos, n, root)		\
	for (pos = prealloc_first_postorder(root);		\
	     pos && ({ n = prealloc_next_postorder(pos); 1; });	\
	     pos = prealloc_next_postorder(n))

/*
 * _delete_prealloc() - delete prealloc branch/subtree in case of failure
 * @ctxt:		AVL operations environment
 * @prealloc:		The root node of the preallocated branch
 *
 * The prealloc branch is not yet inserted into the tree.
 * Will do a post-order walk and delete the nodes.
 */
void _delete_prealloc(struct sptree_ctxt *ctxt, struct sptree_node *prealloc)
{
	struct sptree_ops *ops = ctxt->root->ops;
	struct sptree_node *node, *temp;

	pr_debug("%s: start at "NODE_FMT"\n", __func__, NODE_ARG(prealloc));
	ASSERT(prealloc);
	ASSERT(is_new_branch(prealloc));
	ASSERT(is_root(get_parent(prealloc)) || !is_new_branch(get_parent(prealloc)));

	sptree_for_each_prealloc_po_safe(node, temp, prealloc) {
		ASSERT(is_new_branch(node));
		ops->free(node);
	}
}


/* reverse-in-order iteration on the new branch */
static struct sptree_node *prealloc_rightmost_rin(struct sptree_node *node)
{
	ASSERT(is_new_branch(node));

	/* descend along the right branch */
	for (;;) {
		if (node->right && is_new_branch(node->right))
			node = node->right;
		else
			return node;
	}
}

static struct sptree_node *prealloc_successor_rin(struct sptree_node *node)
{
	struct sptree_node *parent;

	/* ascend along the left branch */
	for (;;) {
		parent = get_parent(node);
		if (is_root(parent) || !is_new_branch(parent))
			return NULL;

		/* is right child */
		if (!is_left_child(node->parent))
			return parent;

		node = parent;
	}
}

/* the preallocated branch/subtree doesn't have a root, just a root node */
static struct sptree_node *prealloc_first_rin(struct sptree_node *node)
{
	if (!node)
		return NULL;

	ASSERT(is_new_branch(node));

	return prealloc_rightmost_rin(node);
}

struct sptree_node *prealloc_next_rin(struct sptree_node *node)
{
	/* reverse-in-order RNL -> next is left */
	if (node->left && is_new_branch(node->left))
		return prealloc_rightmost_rin(node->left);

	return prealloc_successor_rin(node);
}

#define sptree_for_each_prealloc_rin(pos, root)	\
	for (pos = prealloc_first_rin(root);	\
	     pos != NULL;			\
	     pos = prealloc_next_rin(pos))	\

/**
 * prealloc_connect() - insert the new branch in a tree
 * @pbranch:	The place where new branch will be connected
 * @branch:	The root of the new branch/subtree
 *
 * Used on insertion/deletion. The new branch can be:
 * - NULL (empty branch) when deleting a leaf node
 * - a single node or a branch for insertion
 * - a single node or a subtree for deletion
 *
 * The connections must be made in reverse-post-order (RLN), clockwise,
 * starting from the rightmost node of the new branch and finishing at root,
 * so when an in-order tree walk returns from a connected subtree,
 * it returns into the new branch and stays there.
 * Searches will start in the old branch and will stay there.
 */
void prealloc_connect(struct sptree_node **pbranch, struct sptree_node *branch)
{
	struct sptree_node *node;

	pr_debug("%s: at "NODE_FMT"\n", __func__, NODE_ARG(branch));

	sptree_for_each_prealloc_rin(node, branch) {
		ASSERT(is_new_branch(node));

		if (node->right && !is_new_branch(node->right))
			rcu_assign_pointer(node->right->parent, make_right(node));
		if (node->left && !is_new_branch(node->left))
			rcu_assign_pointer(node->left->parent, make_left(node));
	}

	/* clear the new branch flag post-order, otherwise it breaks iteration */
	sptree_for_each_prealloc_po(node, branch) {
		ASSERT(is_new_branch(node));
		node->new_branch = 0;
	}

	/* finally link root / degenerate case (empty new branch) */
	rcu_assign_pointer(*pbranch, branch);
}

/*
 * prealloc_connect_root() - degenerate case for prealloc_connect()
 * @root	root of the tree
 *
 * Used for deletion, in the case where the single node in the tree
 * (the root) gets deleted. In this case the preallocated branch is
 * NULL and gets connected to the root.
 */
void prealloc_connect_root(struct sptree_root *root)
{
	pr_debug("%s: at (EMPTY)\n", __func__);

	rcu_assign_pointer(root->root, NULL);
}

/*
 * prealloc_remove_old() - remove (RCU) nodes replaced by the new branch
 * @ctxt:	AVL operations environment (contains ops & the old nodes chain).
 *
 * All nodes in the old chain will be passed to RCU for deletion.
 */
void prealloc_remove_old(struct sptree_ctxt *ctxt)
{
	struct sptree_ops *ops = ctxt->root->ops;
	struct llist_node *node;
	struct sptree_node *old, *temp;

	node = __llist_del_all(&ctxt->old);
	llist_for_each_entry_safe(old, temp, node, old)
		ops->free_rcu(old);
}

/*
 * prealloc_remove_old() - remove (RCU) nodes replaced by the new branch
 * @ctxt:	AVL operations environment (contains ops & the old nodes chain).
 *
 * All nodes in the old chain will be passed to RCU for deletion.
 * This is the version used in the delete function. The node matching the key
 * will be the first one that is replaced by the unwind logic and will be found
 * at the end of the old chain. Skip this node, it will be handed to the user.
 */
static void prealloc_remove_old_delete(struct sptree_ctxt *ctxt)
{
	struct sptree_ops *ops = ctxt->root->ops;
	struct llist_node *node;
	struct sptree_node *old, *temp;

	node = __llist_del_all(&ctxt->old);
	node = llist_reverse_order(node);
	node = node->next;
	llist_for_each_entry_safe(old, temp, node, old)
		ops->free_rcu(old);
}


/*
 * prealloc_replace() - replicates a node on the new branch
 * @ctxt() - AVL operations environment
 * @target - node to be replaced
 *
 * Returns a node on the new branch or NULL on error.
 */
struct sptree_node *prealloc_replace(struct sptree_ctxt *ctxt, struct sptree_node *target)
{
	struct sptree_ops *ops = ctxt->root->ops;
	struct sptree_node *prealloc;

	pr_debug("%s: at "NODE_FMT"\n", __func__, NODE_ARG(target));
	ASSERT(!is_new_branch(target));

	/* start by allocating a node that replaces target */
	prealloc = ops->alloc();
	if (!prealloc)
		return NULL;

	ops->copy(prealloc, target);
	prealloc->new_branch = 1;

	__llist_add(&target->old, &ctxt->old);				/* add to chain of old nodes */

	return prealloc;
}

/*
 * prealloc_parent() - brings a parent to the new branch
 * @ctxt - AVL operation environment
 * @child - child already on the new branch
 *
 * Brings a parent to the new branch (one of the children is already on the new branch).
 * Useful for retrace where the algorithm ascends.
 */
struct sptree_node *prealloc_parent(struct sptree_ctxt *ctxt, struct sptree_node *child)
{
	struct sptree_node *parent = get_parent(child);
	struct sptree_node *new_parent;

	pr_debug("%s: at "NODE_FMT"\n", __func__, NODE_ARG(parent));
	ASSERT(is_new_branch(child));
	ASSERT(!is_new_branch(parent));

	/* bring the parent to the new branch */
	new_parent = prealloc_replace(ctxt, parent);
	if (!new_parent)
		return NULL;

	/* link on the new branch */
	if (is_left_child(child->parent)) {
		new_parent->left = child;
		child->parent = make_left(new_parent);
	}
	else {
		new_parent->right = child;
		child->parent = make_right(new_parent);
	}

	return new_parent;
}

/*
 * prealloc_child() - brings a child to the new branch
 * @ctxt - AVL operation environment
 * @parent - a leaf on the new branch
 * @which - LEFT or RIGHT child
 *
 * Only useful for unwind, cause it descents and must find a way down.
 * The parent needs to be part of the new branch (the child points to old parent).
 */
struct sptree_node *prealloc_child(struct sptree_ctxt *ctxt, struct sptree_node *parent, int which)
{
	struct sptree_node *child = which == LEFT_CHILD ? parent->left : parent->right;
	struct sptree_node *new_child;

	pr_debug("%s: %s child of "NODE_FMT"\n", __func__,
		(which == LEFT_CHILD) ? "left" : "right", NODE_ARG(parent));
	ASSERT(is_new_branch(parent));
	ASSERT(child);

	/* since rebalancing poorly balanced branches,
	* some nodes may get new branch children during unwind */
	if (is_new_branch(child))
		return child;

	/* bring the child to the new branch */
	new_child = prealloc_replace(ctxt, child);
	if (!new_child)
		return NULL;

	/* link on the new branch */
	if (which == LEFT_CHILD) {
		parent->left = new_child;
		new_child->parent = make_left(parent);
	}
	else {
		parent->right = new_child;
		new_child->parent = make_right(parent);
	}

	return new_child;
}

/*
 * Difference between retrace rotations & generic rotations:
 * - retrace rotations work on a subset of the cases...
 * - retrace rotations always work bottom-up (the parent is never part of the new branch).
 * - the diff in height of +-1 is encoded in the control flow (not in the subtree root).
 * - retrace rotations always attempt to reduce the height of the subtree
 */

/* return new root to be set as top of branch */
static struct sptree_node *prealloc_retrace_ror(struct sptree_node *root)
{
	struct sptree_node *pivot = root->left;
	struct sptree_node *t2 = pivot->right;
	struct sptree_node *new_root = pivot;
	struct sptree_node *new_pivot = root;

	pr_debug("%s: root at "NODE_FMT", pivot at "NODE_FMT"\n",
		__func__, NODE_ARG(root), NODE_ARG(pivot));
	ASSERT(is_new_branch(root));
	ASSERT(is_new_branch(pivot));

	// redistribute t2
	new_pivot->left = t2;
	if (t2 && is_new_branch(t2))
		t2->parent = make_left(new_pivot);

	// relink nodes
	new_root->parent = root->parent;
	new_root->right = new_pivot;
	new_pivot->parent = make_right(new_root);

	// fix balance factors
	if (pivot->balance == 0) {
		new_root->balance = 1;
		new_pivot->balance = -1;
	}
	else {
		new_pivot->balance = 0;
		new_root->balance = 0;
	}

	pr_debug("%s: new root is "NODE_FMT"\n", __func__, NODE_ARG(new_root));

	return new_root;
}

/* return new root to be set as top of branch */
static struct sptree_node *prealloc_retrace_rlr(struct sptree_node *root)
{
	// root = X
	struct sptree_node *left = root->left;		// Z
	struct sptree_node *right = left->right;	// Y
	struct sptree_node *t2 = right->left;
	struct sptree_node *t3 = right->right;
	struct sptree_node *new_root = right;		// new Y
	struct sptree_node *new_left = left;		// new Z
	struct sptree_node *new_right = root;		// new X

	pr_debug("%s: root at "NODE_FMT"\n", __func__, NODE_ARG(root));
	ASSERT(is_new_branch(root));
	ASSERT(is_new_branch(left));
	ASSERT(is_new_branch(right));

	// redistribute t2, t3
	new_left->right = t2;
	if (t2 && is_new_branch(t2))
		t2->parent = make_right(new_left);

	new_right->left = t3;
	if (t3 && is_new_branch(t3))
		t3->parent = make_left(new_right);

	// relink nodes
	new_root->parent = root->parent;
	new_root->left = new_left;
	new_left->parent = make_left(new_root);
	new_root->right = new_right;
	new_right->parent = make_right(new_root);

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

	pr_debug("%s: new root is "NODE_FMT"\n", __func__, NODE_ARG(new_root));

	return new_root;
}

/* return new root to be set as top of branch */
static struct sptree_node *prealloc_retrace_rol(struct sptree_node *root)
{
	struct sptree_node *pivot = root->right;
	struct sptree_node *t2 = pivot->left;
	struct sptree_node *new_root = pivot;
	struct sptree_node *new_pivot = root;

	pr_debug("%s: root at "NODE_FMT", pivot at "NODE_FMT"\n",
		__func__, NODE_ARG(root), NODE_ARG(pivot));
	ASSERT(is_new_branch(root));
	ASSERT(is_new_branch(pivot));

	// redistribute t2
	new_pivot->right = t2;
	if (t2 && is_new_branch(t2))
		t2->parent = make_right(new_pivot);

	// relink nodes
	new_root->parent = root->parent;
	new_root->left = new_pivot;
	new_pivot->parent = make_left(new_root);

	// fix balance factors
	if (pivot->balance == 0) {
		new_root->balance = 1;
		new_pivot->balance = -1;
	}
	else {
		new_pivot->balance = 0;
		new_root->balance = 0;
	}

	pr_debug("%s: new root is "NODE_FMT"\n",
		__func__, NODE_ARG(new_root));

	return new_root;
}

/* return new root to be set as top of branch */
static struct sptree_node *prealloc_retrace_rrl(struct sptree_node *root)
{
	// root = X
	struct sptree_node *right = root->right;	// Z
	struct sptree_node *left = right->left;		// Y
	struct sptree_node *t2 = left->left;
	struct sptree_node *t3 = left->right;
	struct sptree_node *new_root = left;		// new Y
	struct sptree_node *new_left = root;		// new X
	struct sptree_node *new_right = right;		// new Z

	pr_debug("%s: root at "NODE_FMT"\n", __func__, NODE_ARG(root));
	ASSERT(is_new_branch(root));
	ASSERT(is_new_branch(right));
	ASSERT(is_new_branch(left));

	// redistribute t2, t3
	new_left->right = t2;
	if (t2 && is_new_branch(t2))
		t2->parent = make_right(new_left);

	new_right->left = t3;
	if (t3 && is_new_branch(t3))
		t3->parent = make_left(new_right);

	// relink nodes
	new_root->parent = root->parent;
	new_root->left = new_left;
	new_left->parent = make_left(new_root);
	new_root->right = new_right;
	new_right->parent = make_right(new_root);

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

	pr_debug("%s: new root is "NODE_FMT"\n", __func__, NODE_ARG(new_root));

	return new_root;
}

/* retrace for insert */
static struct sptree_node *prealloc_retrace(struct sptree_ctxt *ctxt, struct sptree_node *node)
{
	struct sptree_node *parent;

	/* node is the latest member of branch */
	pr_debug("%s: starting at "NODE_FMT"\n", __func__, NODE_ARG(node));
	ASSERT(is_new_branch(node));

	for (parent = get_parent(node); !is_root(parent); node = parent, parent = get_parent(node)) {

		pr_debug("%s: loop on parent "NODE_FMT", node is "NODE_FMT"\n",
			__func__, NODE_ARG(parent), NODE_ARG(node));

		parent = prealloc_parent(ctxt, node);
		if (!parent)
			goto error;

		if (is_left_child(node->parent)) {
			// parent is left-heavy
			if (parent->balance < 0) {
				// node is right-heavy
				if (node->balance > 0)
					parent = prealloc_retrace_rlr(parent);
				else
					parent = prealloc_retrace_ror(parent);

				return parent;
			}
			// parent is right-heavy
			else if (parent->balance > 0) {
				parent->balance = 0;			/* parent becomes balanced */

				return parent;
			}
			// parent is balanced
			else {
				parent->balance = -1;			/* parent becomes left-heavy */
			}
		}
		// is right child
		else {
			// parent is right heavy
			if (parent->balance > 0) {
				// node is left-heavy
				if (node->balance < 0)
					parent = prealloc_retrace_rrl(parent);
				else
					parent = prealloc_retrace_rol(parent);

				return parent;
			}
			// parent is left-heavy
			else if (parent->balance < 0) {
				parent->balance = 0;			/* parent becomes balanced */

				return parent;
			}
			// parent is balanced
			else {
				parent->balance = 1;			/* parent becomes right-heavy */
			}
		}
	}

	return node;

error:
	// node should have the top of the new branch
	_delete_prealloc(ctxt, node);

	return NULL;
};

/*
 * prealloc_insert() - insert new node into the tree
 * @root - the root of the tree
 * @node - the new node to be added
 *
 * The container of the node must be allocated & compatible with deletion callback.
 * The node must be zeroed before insertion.
 *
 * If insertion succeeds, the container may already be deleted by the time this
 * function exits, so usage of the whole structure is invalid after insertion.
 * If insertion fails, it is the task of the user to delete the container.
 */
int prealloc_insert(struct sptree_root *root, struct sptree_node *node)
{
	struct sptree_ops *ops = root->ops;
	struct sptree_node *crnt, *parent, **pbranch;
	struct sptree_node *prealloc;
	struct sptree_ctxt ctxt;

	// manual loop-invariant code motion
	unsigned long node_key = ops->get_key(node);
	unsigned long crnt_key;

	pr_debug("%s: node "NODE_FMT"\n", __func__, NODE_ARG(node));
	ASSERT(node->balance == 0);
	ASSERT(is_leaf(node));

	/* look for a parent */
	for (crnt = root->root, parent = NULL; crnt != NULL; ) {
		crnt_key = ops->get_key(crnt);

		if (unlikely(node_key == crnt_key))
			return -EINVAL;
		else if (node_key < crnt_key) {
			parent = make_left(crnt);
			crnt = crnt->left;
		}
		else {
			parent = make_right(crnt);
			crnt = crnt->right;
		}
	}

	node->parent = parent;		/* only link one way */
	node->new_branch = 1;

	sptree_ctxt_init(&ctxt, root);

	/* retrace generates the preallocated branch */
	prealloc = prealloc_retrace(&ctxt, node);
	if (!prealloc)
		return -ENOMEM;

	pbranch = get_pnode(root, prealloc->parent);
	prealloc_connect(pbranch, prealloc);

	// this will remove the replaced nodes
	if (!llist_empty(&ctxt.old))
		prealloc_remove_old(&ctxt);

	// TODO: remove once code stable
	validate_avl_balancing(root);

	return 0;
}


struct balance_factors
{
	int root;
	int pivot;
};

static int rol_height_diff(int root_bal, int pivot_bal)
{
	int diff = 0;

	if (pivot_bal <= 0) {
		if (root_bal <= 0)
			diff = 1;
	}
	else {
		if (root_bal <= 0)
			diff = 1;
		else if (root_bal > 1)
			diff = -1;
	}

	return diff;
}

static int ror_height_diff(int root_bal, int pivot_bal)
{
	int diff = 0;

	if (pivot_bal >= 0) {
		if (root_bal >= 0)
			diff = 1;
	}
	else {
		if (root_bal >= 0)
			diff = 1;
		else if (root_bal < -1)
			diff = -1;
	}

	return diff;
}

static struct balance_factors rol_new_balance(int root_bal, int pivot_bal)
{
	struct balance_factors new_balance;

	if (pivot_bal <= 0) {
		new_balance.pivot = root_bal - 1;
		if (root_bal >= 1)
			new_balance.root = pivot_bal - 1;
		else
			new_balance.root = root_bal + pivot_bal - 2;
	}
	else {
		new_balance.pivot = root_bal - pivot_bal - 1;
		if (root_bal >= pivot_bal + 1)
			new_balance.root = pivot_bal - 1;
		else
			new_balance.root = root_bal - 2;
	}


	return new_balance;
}

static struct balance_factors ror_new_balance(int root_bal, int pivot_bal)
{
	struct balance_factors new_balance;

	if (pivot_bal >= 0) {
		new_balance.pivot = root_bal + 1;
		if (root_bal <= -1)
			new_balance.root = pivot_bal + 1;
		else
			new_balance.root = root_bal + pivot_bal + 2;
	}
	else {
		new_balance.pivot = root_bal - pivot_bal + 1;
		if (root_bal <= pivot_bal - 1)
			new_balance.root = pivot_bal + 1;
		else
			new_balance.root = root_bal + 2;
	}

	return new_balance;
}


/*
 * prealloc_propagate_change() - propagate height diff up the new branch
 * @subtree:	- root of the subtree that changed height during a rotation
 * @diff	- height diff +-1
 *
 * Under certain operations, the height of the subtree changes, and
 * the changes must be propagated up to the root (of the new branch).
 *
 * If a subtree is rotated, the balances of the root & pivot are changed by
 * the rotation, but the height diff is propagated starting with the parent.
 *
 * On a node deletion, the node is bubbled down to a leaf node and deleted,
 * there is a decrease in height starting with the parent of the deleted leaf.
 *
 * WARNING: this function can only handle diffs of +-1, which are absorbed by balanced nodes
 * These diffs result from legal rotations (on AVL balanced subtrees).
 */
void prealloc_propagate_change(struct sptree_ctxt *ctxt, struct sptree_node *subtree, int diff)
{
	struct sptree_node *parent;
	bool left_child, balance_before;

	pr_debug("%s: starting at "NODE_FMT"\n", __func__, NODE_ARG(subtree));
	ASSERT(is_new_branch(subtree));
	ASSERT(diff == -1 || diff == 1);

	// parent may contain L/R flags or NULL, strip flags before using as pointer
	for (parent = subtree->parent; !is_root(parent); parent = parent->parent) {
		left_child = is_left_child(parent);
		parent = strip_flags(parent);

		// act only on the new branch
		if (!is_new_branch(parent))
			break;

		// parent was balanced before applying the diff
		balance_before = parent->balance;

		if (left_child)
			parent->balance -= diff;
		else
			parent->balance += diff;

		// on delete, the propagation stops when it encounters a balanced parent
		// (the height of the subtree remains unchanged)
		if (diff == -1 && balance_before == 0) {
			pr_debug("%s: decrease, stop update at "NODE_FMT"\n",
				__func__, NODE_ARG(parent));
			return;		// will not accumulate height diff
		}
		// on addition, a height increase is absorbed by a balancing
		else if (diff == 1 && parent->balance == 0) {
			pr_debug("%s: increase, stop update at "NODE_FMT"\n",
				__func__, NODE_ARG(parent));
			return;		// will not accumulate height diff
		}

		pr_debug("%s: updated balance factor for "NODE_FMT"\n",
			__func__, NODE_ARG(parent));
	}

	// accumulate height diff across the unwind + fix steps
	ctxt->diff += diff;
}

/*
 * prealloc_rol() - generic ROL for the unwind & fix steps
 * @ctxt	AVL operations environment
 * @target	root of the subtree to be rotated
 *
 * Both the target and the pivot must be part of the new branch.
 * Returns the new root of the subtree. Does not fail.
 */
struct sptree_node *prealloc_rol(struct sptree_ctxt *ctxt, struct sptree_node *target)
{
	struct sptree_node *parent, **ptarget;
	struct sptree_node *pivot = target->right;
	struct sptree_node *t2 = pivot->left;
	struct sptree_node *new_root = pivot;
	struct sptree_node *new_pivot = target;
	struct balance_factors new_balance;
	int diff_height;

	pr_debug("%s: at "NODE_FMT"\n", __func__, NODE_ARG(target));
	ASSERT(is_new_branch(target));
	ASSERT(is_new_branch(pivot));

	/* compute new balance factors & tree height diff after rotation */
	diff_height = rol_height_diff(target->balance, pivot->balance);
	new_balance = rol_new_balance(target->balance, pivot->balance);

	/* redistribute t2 */
	new_pivot->right = t2;
	if (t2 && is_new_branch(t2))
		t2->parent = make_right(new_pivot);

	/* relink parents */
	parent = get_parent(target);
	ptarget = get_pnode(ctxt->root, target->parent);

	new_root->parent = target->parent;
	if (parent && is_new_branch(parent))
		*ptarget = new_root;

	new_root->left = new_pivot;
	new_pivot->parent = make_left(new_root);

	/* fix balance factors & height diff */
	new_root->balance = new_balance.root;
	new_pivot->balance = new_balance.pivot;
	if (diff_height)
		prealloc_propagate_change(ctxt, new_root, diff_height);

	return new_root;
}

/*
 * prealloc_ror() - generic ROR for the unwind & fix steps
 * @ctxt	AVL operations environment
 * @target	root of the subtree to be rotated
 *
 * Both the target and the pivot must be part of the new branch.
 * Returns the new root of the subtree. Does not fail.
 */
struct sptree_node *prealloc_ror(struct sptree_ctxt *ctxt, struct sptree_node *target)
{
	struct sptree_node *parent, **ptarget;
	struct sptree_node *pivot = target->left;
	struct sptree_node *t2 = pivot->right;
	struct sptree_node *new_root = pivot;
	struct sptree_node *new_pivot = target;
	struct balance_factors new_balance;
	int diff_height;

	pr_debug("%s: at "NODE_FMT"\n", __func__, NODE_ARG(target));
	ASSERT(is_new_branch(target));
	ASSERT(is_new_branch(pivot));

	/* compute new balance factors & tree height diff after rotation */
	diff_height = ror_height_diff(target->balance, pivot->balance);
	new_balance = ror_new_balance(target->balance, pivot->balance);

	/* redistribute t2 */
	new_pivot->left = t2;
	if (t2 && is_new_branch(t2))
		t2->parent = make_left(new_pivot);

	/* relink parents */
	parent = get_parent(target);
	ptarget = get_pnode(ctxt->root, target->parent);

	new_root->parent = target->parent;
	if (parent && is_new_branch(parent))
		*ptarget = new_root;

	new_root->right = new_pivot;
	new_pivot->parent = make_right(new_root);

	/* fix balance factors & height diff */
	new_root->balance = new_balance.root;
	new_pivot->balance = new_balance.pivot;
	if (diff_height)
		prealloc_propagate_change(ctxt, new_root, diff_height);

	return new_root;
}

/*
 * prealloc_rrl() - generic RRL for the unwind & fix steps
 * @ctxt	AVL operations environment
 * @target	root of the subtree to be rotated
 *
 * Both the target and the pivot must be part of the new branch.
 * Returns the new root of the subtree. Does not fail.
 */
struct sptree_node *prealloc_rrl(struct sptree_ctxt *ctxt, struct sptree_node *target)
{
	struct sptree_node *pivot = target->right;

	pr_debug("%s: at "NODE_FMT"\n", __func__, NODE_ARG(target));
	ASSERT(is_new_branch(target));
	ASSERT(is_new_branch(pivot));

	prealloc_ror(ctxt, pivot);
	return prealloc_rol(ctxt, target);
}

/*
 * prealloc_rlr() - generic RLR for the unwind & fix steps
 * @ctxt	AVL operations environment
 * @target	root of the subtree to be rotated
 *
 * Both the target and the pivot must be part of the new branch.
 * Returns the new root of the subtree. Does not fail.
 */
struct sptree_node *prealloc_rlr(struct sptree_ctxt *ctxt, struct sptree_node *target)
{
	struct sptree_node *pivot = target->left;

	pr_debug("%s: at "NODE_FMT"\n", __func__, NODE_ARG(target));
	ASSERT(is_new_branch(target));
	ASSERT(is_new_branch(pivot));

	prealloc_rol(ctxt, pivot);
	return prealloc_ror(ctxt, target);
}


/*
 * poor_balance_depth() - count the number of moves needed to rebalance a poorly balanced subtree
 * @node - first poorly balanced node (root of the subtree)
 *
 * A poorly balanced subtree can be recursive. A single rebalance will not be enough.
 * This function counts the number of recursive rebalances needed to rebalance this subtree.
 */
static int poor_balance_depth(struct sptree_node *node)
{
	int count = 1;
	int expected = node->balance;

	pr_debug("%s: at "NODE_FMT"\n", __func__, NODE_ARG(node));
	ASSERT(node->balance == -1 || node->balance == 1);
	ASSERT(!is_leaf(node));	// kinda useless assert

	for (;;) {
		/* advance to child -> child will exist */
		expected = -expected;
		if (node->balance == -1)
			node = node->left;
		else // node->balance == 1
			node = node->right;

		/* check current balance if +-1 */
		if (node->balance == expected)
			count++;
		else
			break;
	}

	return count;
}

/*
 * prealloc_rebalance() - recursively rebalance a poorly balanced subtree
 * @ctxt - AVL operations environment
 * @target - first poorly balanced node (root of the subtree)
 *
 * A poorly balanced subtree can be recursive. A single rebalance will not be enough.
 * This function will recursvely rebalance the poorly balanced branch.
 * This subtree will not increase in height (according to rotation tables).
 * Returns the new root of the subtree or NULL on error.
 */
static struct sptree_node *prealloc_rebalance(struct sptree_ctxt *ctxt, struct sptree_node *target)
{
	struct sptree_node *node = target;
	struct sptree_node *child;
	bool stop = false;
	int expected = node->balance;

	pr_debug("%s: at "NODE_FMT"\n", __func__, NODE_ARG(target));
	ASSERT(is_new_branch(target));
	ASSERT(target->balance == -1 || target->balance == 1);

	/* descend along poorly balanced branch, starting with the first node... */
	while (node->balance == expected) {
		int which_child = (expected == -1) ? LEFT_CHILD : RIGHT_CHILD;

		/* prealloc child anyway, it's either used for rotation or poorly balanced */
		child = prealloc_child(ctxt, node, which_child);
		if (!child)
			return NULL;

		expected = -expected;

		/* this will be the left or the right child */
		/* the child needs to have balance opposed to the parent, otherwise stop at parent */
		if (child->balance == expected)
			node = child;
	}

	/* we are at the bottom poorly balanced node */
	ASSERT(is_new_branch(node));	/* poorly balanced parent */
	ASSERT(is_new_branch(child));	/* child that broke the loop */

	/* ascend & rebalance the tree */
	do {
		/* last rotation... */
		if (node == target)
			stop = true;

		if (node->balance == 1)
			node = prealloc_rol(ctxt, node);
		else
			node = prealloc_ror(ctxt, node);

		/* ...otherwise advance to parent */
		if (!stop)
			node = get_parent(node);
	} while (!stop);

	return node;
}

/*
 * prealloc_reverse_rrl() - reverse RRL for the unwind step
 * @ctxt	AVL operations environment
 * @target	root of the subtree to be rotated
 *
 * All the nodes subject to rotations must be part of the new branch.
 * Returns the bottom of the rotated nodes. Does not fail.
 */
static struct sptree_node *prealloc_reverse_rrl(struct sptree_ctxt *ctxt, struct sptree_node *target)
{
	struct sptree_node *subtree_root;

	pr_debug("%s: at "NODE_FMT"\n", __func__, NODE_ARG(target));

	/* the new subtree root is established after the 1st rotation */
	subtree_root = prealloc_ror(ctxt, target);
	target = subtree_root->right;

	/* the 2nd rotation may add to the overall unbalancing/diff in height */
	target = prealloc_rol(ctxt, target);
	target = target->left;

	return target;
}

/*
 * prealloc_reverse_rlr() - reverse RLR for the unwind step
 * @ctxt	AVL operations environment
 * @target	root of the subtree to be rotated
 *
 * All the nodes subject to rotations must be part of the new branch.
 * Returns the bottom of the rotated nodes. Does not fail.
 */
static struct sptree_node *prealloc_reverse_rlr(struct sptree_ctxt *ctxt, struct sptree_node *target)
{
	struct sptree_node *subtree_root;

	pr_debug("%s: at "NODE_FMT"\n", __func__, NODE_ARG(target));

	/* the new subtree root is established after the 1st rotation */
	subtree_root = prealloc_rol(ctxt, target);
	target = subtree_root->left;

	/* the 2nd rotation may add to the overall unbalancing/diff in height */
	target = prealloc_ror(ctxt, target);
	target = target->right;

	return target;
}

static struct sptree_node *prealloc_unwind_double(struct sptree_ctxt *ctxt, struct sptree_node *target)
{
	struct sptree_node *left = target->left;
	struct sptree_node *right = target->right;
	int poor_left, poor_right;

	pr_debug("%s: at "NODE_FMT"\n", __func__, NODE_ARG(target));
	ASSERT(!is_leaf(target));
	ASSERT(is_new_branch(target));

	/* prealloc both the left and right pivots,
	 * both will be used in reverse compound rotations */
	left = prealloc_child(ctxt, target, LEFT_CHILD);
	if (!left)
		return NULL;

	right = prealloc_child(ctxt, target, RIGHT_CHILD);
	if (!right)
		return NULL;

	/* choose direction based on the balance of the subtrees
	 * a rotation will give the new pivot the nearest subtree of the old pivot */

	// reverse RRL
	if (left->balance == -1)
		return prealloc_reverse_rrl(ctxt, target);

	// reverse RLR
	if (right->balance == 1)
		return prealloc_reverse_rlr(ctxt, target);

	// don't care: so reverse RRL
	if (right->balance == 0 && left->balance == 0)
		return prealloc_reverse_rrl(ctxt, target);

	// poorly balanced case, rebalance left
	if (right->balance == 0 && left->balance == 1) {
rebalance_left:
		// rebalance the left subtree, then apply case 1
		left = prealloc_rebalance(ctxt, left);
		if (!left)
			return NULL;
		return prealloc_reverse_rrl(ctxt, target);
	}

	// poorly balanced case, rebalance right
	if (right->balance == -1 && left->balance == 0) {
rebalance_right:
		// rebalance the right subtree, then apply case 2
		right = prealloc_rebalance(ctxt, right);
		if (!right)
			return NULL;
		return prealloc_reverse_rlr(ctxt, target);
	}

	// both poorly balanced: rebalance the one with the least depth
	if (right->balance == -1 && left->balance == 1) {
		poor_left = poor_balance_depth(left);
		poor_right = poor_balance_depth(right);

		if (poor_left < poor_right)
			goto rebalance_left;
		else
			goto rebalance_right;
	}

	/* did I miss something ? */
	pr_err("%s: invalid case at "NODE_FMT", left "NODE_FMT", right "NODE_FMT"\n",
		__func__, NODE_ARG(target), NODE_ARG(left), NODE_ARG(right));
	BUG();
	return ERR_PTR(-EINVAL);
}

/* target->balance == 1 */
static struct sptree_node *prealloc_unwind_left(struct sptree_ctxt *ctxt, struct sptree_node *target)
{
	struct sptree_node *pivot = target->right;

	pr_debug("%s: at "NODE_FMT"\n", __func__, NODE_ARG(target));
	ASSERT(pivot);

	/* prealloc child anyway, it's either used for rotation or poorly balanced */
	pivot = prealloc_child(ctxt, target, RIGHT_CHILD);
	if (!pivot)
		return NULL;

	/* poorly balanced case, rebalance the subtree rooted by pivot */
	if (pivot->balance == -1) {
		pivot = prealloc_rebalance(ctxt, pivot);
		if (!pivot)
			return NULL;
	}

	/* rotation functions return the new subtree root */
	target = prealloc_rol(ctxt, target);
	target = target->left;

	return target;
}

/* target->balance == -1 */
static struct sptree_node *prealloc_unwind_right(struct sptree_ctxt *ctxt, struct sptree_node *target)
{
	struct sptree_node *pivot = target->left;

	pr_debug("%s: at "NODE_FMT"\n", __func__, NODE_ARG(target));
	ASSERT(pivot);

	/* prealloc child anyway, it's either used for rotation or poorly balanced */
	pivot = prealloc_child(ctxt, target, LEFT_CHILD);
	if (!pivot)
		return NULL;

	/* poorly balanced case, rebalance the subtree rooted by pivot */
	if (pivot->balance == 1) {
		pivot = prealloc_rebalance(ctxt, pivot);
		if (!pivot)
			return NULL;
	}

	/* rotation functions return the new subtree root */
	target = prealloc_ror(ctxt, target);
	target = target->right;

	return target;
}

/*
 * prealloc_top() - iterate to the top of the new branch
 * @target - a node on the new branch
 *
 * Used for failure cases in unwind/fix/retrace, when one needs
 * the top of the new branch to delete it.
 * Always returns a valid value;
 */
struct sptree_node *prealloc_top(struct sptree_node *target)
{
	struct sptree_node *parent;

	pr_debug("%s: at "NODE_FMT"\n", __func__, NODE_ARG(target));
	ASSERT(is_new_branch(target));

	for (parent = get_parent(target); !is_root(parent) && is_new_branch(parent); parent = get_parent(target))
		target = parent;

	return target;
}

/* prealloc_unwind() - bubble a node down to a leaf
 * @ctxt - AVL operations environment
 * @target - node to be bublled
 *
 * This is called only if the node is not a leaf, so must return a preallocated branch != NULL.
 * It takes the initial target node and bubbles it down to a leaf node -> which will later be deleted.
 * Will return NULL on error.
 * WARNING: this function does not return the top of the breallocated branch, but the bottom !!
 */
struct sptree_node *prealloc_unwind(struct sptree_ctxt *ctxt, struct sptree_node *target)
{
	struct sptree_node *prealloc;

	pr_debug("%s: at "NODE_FMT"\n", __func__, NODE_ARG(target));
	ASSERT(!is_leaf(target));

	/* start by allocating a node that replaces target */
	prealloc = prealloc_replace(ctxt, target);
	if (!prealloc)
		goto error;

	/* walk the new branch, add nodes that represent the bubbling sequence */
	do {
		/* target contains the top of the prealloc branch */
		target = prealloc;

		/* each unwinding step must start with a normal balance */
		ASSERT(is_avl(target));

		switch (target->balance) {
		case -1:
			prealloc = prealloc_unwind_right(ctxt, target);
			break;

		case 0:
			prealloc = prealloc_unwind_double(ctxt, target);
			break;

		case 1:
			prealloc = prealloc_unwind_left(ctxt, target);
			break;

		default:
			pr_err("%s: invalid case at "NODE_FMT"\n", __func__, NODE_ARG(target));
			BUG();
			return NULL;
		}

		/* check if the allocation succeeded */
		if (!prealloc)
			goto error;

		/* check that the new pivot (prealloc) is AVL invariant */
		ASSERT(prealloc->balance >= -1 && prealloc->balance <= 1);

	} while (!is_leaf(prealloc));

	/* return the bottom of the preallocated branch */
	return prealloc;

error:
	// walk the parent to the top of the preallocated branch
	prealloc = prealloc_top(prealloc);
	_delete_prealloc(ctxt, prealloc);

	return NULL;
}

/**
 * delete_retrace() - does the fix & retrace steps
 * @ctxt - AVL operations environment
 * @parent - parent of the leaf that was just deleted
 *
 * Receives the parent of the deleted leaf, that is now itself a leaf on the new branch.
 * (It may have another child, but on the old branch. Does this comment help at all ?!)
 * So the bottom of the new branch.
 *
 * Does the fix on the new branch. That is correcting the excessive unbalance that resulted
 * from reverse double rotations. If the resulting subtree is shorter than the original,
 * then a retrace is attempted starting at its parent.
 *
 * Will return NULL on error.
 */
static struct sptree_node *delete_retrace(struct sptree_ctxt *ctxt, struct sptree_node *parent)
{
	struct sptree_node *node, *temp, *sibling;
	int sibling_balance_before;

	pr_debug("%s: starting at "NODE_FMT"\n", __func__, NODE_ARG(parent));
	ASSERT(is_new_branch(parent));

	/* fix step - running only on the new branch */
	/* walk up the new branch & look for excessive unbalancing */
	do {
		node = parent;

		switch (node->balance) {
		case -2:
			temp = prealloc_rlr(ctxt, node);
			if (!temp)
				goto error;
			node = temp;
			break;

		case 2:
			temp = prealloc_rrl(ctxt, node);
			if (!temp)
				goto error;
			node = temp;
			break;
		}

		parent = get_parent(node);
	} while (!is_root(parent) && is_new_branch(parent));

	pr_debug("%s: overall diff after fix %d\n", __func__, ctxt->diff);

	/* no need to retrace if there is no change in height */
	if (ctxt->diff == 0)
		return node;

	ASSERT(ctxt->diff == -1);

	/* rest of the retracing (parent already contains the link point of the new branch) */
	/* integrate diff == -1 condition into the control flow,
	 * loop as long as there is a decrease in height present */
	for (; !is_root(parent); node = parent, parent = get_parent(node)) {

		pr_debug("%s: loop on parent "NODE_FMT", node is "NODE_FMT"\n",
			__func__, NODE_ARG(parent), NODE_ARG(node));

		/* bring parent to new branch */
		parent = prealloc_parent(ctxt, node);
		if (!parent)
			goto error;

		if (is_left_child(node->parent)) {
			if (parent->balance > 0) {
				sibling = parent->right;
				sibling_balance_before = sibling->balance;

				if (sibling->balance < 0)
					parent = prealloc_retrace_rlr(parent);
				else
					parent = prealloc_retrace_rol(parent);
			}
			else if (parent->balance == 0) {
				parent->balance = 1;
				return parent;
			}
			// parent->balance == -1
			else {
				parent->balance = 0;
				continue;
			}
		}
		// is right child
		else {
			if (parent->balance < 0) {
				sibling = parent->left;
				sibling_balance_before = sibling->balance;

				if (sibling->balance > 0)
					parent = prealloc_retrace_rlr(parent);
				else
					parent = prealloc_retrace_ror(parent);
			}
			else if (parent->balance == 0) {
				parent->balance = -1;
				return parent;
			}
			// parent->balance == 1
			else {
				parent->balance = 0;
				continue;
			}
		}

		// only reachable from sibling-related branch
		if (sibling_balance_before == 0)
			return parent;
	}

	return node;

error:
	node = prealloc_top(node);
	_delete_prealloc(ctxt, node);

	return NULL;
}

/* delete & retrace in a single function */
/* WARNING: can return NULL as a valid value (check corner case) */
static struct sptree_node *unwind_delete_retrace(struct sptree_ctxt *ctxt, struct sptree_node *node)
{
	struct sptree_ops *ops = ctxt->root->ops;
	struct sptree_node *prealloc, *leaf;
	struct sptree_node *parent;
	bool left_child;

	if (is_leaf(node)) {
		struct sptree_node fake_leaf;

		__llist_add(&node->old, &ctxt->old);		/* add to chain of old nodes */

		/* if that node is the only node in the tree, the new branch is empty (NULL) */
		if (is_root(node->parent))
			return NULL;

		/* create a fake leaf on the stack to avoid allocations */
		memcpy(&fake_leaf, node, sizeof(struct sptree_node));
		fake_leaf.new_branch = 1;
		leaf = &fake_leaf;

		/* prealloc parent from the fake leaf */
		parent = prealloc_parent(ctxt, leaf);
		if (!parent)
			return ERR_PTR(-ENOMEM);

		left_child = is_left_child(leaf->parent);
		prealloc_propagate_change(ctxt, leaf, -1);
	}
	else {
		/* bubble target node to bottom */
		leaf = prealloc_unwind(ctxt, node);
		if (!leaf)
			return ERR_PTR(-ENOMEM);

		parent = get_parent(leaf);				/* parent is directly available */
		left_child = is_left_child(leaf->parent);

		/* nodes along this branch may be heavy towards the leaf or balanced
		 * this also applies to its direct parent, if it's leaf-heavy,
		 * deleting the leaf will propagate the change up the new branch */
		prealloc_propagate_change(ctxt, leaf, -1);
		ops->free(leaf);
	}

	ASSERT(is_new_branch(parent));

	/* clear pointer in parent pointing to the leaf */
	if (left_child)
		parent->left = NULL;
	else
		parent->right = NULL;

	/* under normal case (root of a subtree being deleted) the subtree is already in AVL form */
	/* regular bubbling (left or right) is done on an unbalanced node and won't change AVL invariants */
	/* reverse double rotations are done on balanced nodes, and these are the ones that add excessive
	 * unbalancing (and need fixing). unbalancing points in the direction the target is bubbled! */
	/* under normal circumstances, these unbalancings can be fixed by applying the reverse rotation needed,
	 * but some will be compensated by the removed node and the propagated height diff */

	/* after fixing, if the subtree loses height, this has to be propagated above by retracing */
	/* retracing starts at the parent of the subtree being modified (node/new branch)
	 * and may add to the new branch (at the same time it will add to the old nodes chain) */

	/* in the corner case of a leaf node being deleted, it is considered that the subtree represented
	 * by that node decreseas in height, so retrace creates a branch starting with its parent */

	prealloc = delete_retrace(ctxt, parent);
	if (!prealloc)
		return ERR_PTR(-ENOMEM);

	return prealloc;
}

/*
 * prealloc_delete() - delete a node from the tree
 * @root - root of the tree
 * @key - key of the node to delete
 *
 * Returns:	the extracted node on success
 *		-ENXIO - node was not found
 *		-ENOMEM - allocations failed
 *
 * On error, the tree is not modified.
 * Returns the node corresponding to the key, after extracting it from the tree.
 * The node may still be used by readers, so it's the duty of the user to free it
 * after waiting for a grace period to elapse.
 */
struct sptree_node *prealloc_delete(struct sptree_root *root, unsigned long key)
{
	struct sptree_node *target, **pbranch;
	struct sptree_node *prealloc;
	struct sptree_ctxt ctxt;

	target = search(root, key);
	if (!target)
		return ERR_PTR(-ENXIO);

	sptree_ctxt_init(&ctxt, root);

	/* may return NULL as a valid value !!! */
	prealloc = unwind_delete_retrace(&ctxt, target);
	if (IS_ERR(prealloc))
		return prealloc;

	if (prealloc) {
		pbranch = get_pnode(root, prealloc->parent);
		prealloc_connect(pbranch, prealloc);
	}
	else
		prealloc_connect_root(root);

	// this will remove the replaced nodes
	if (!llist_empty(&ctxt.old))
		prealloc_remove_old_delete(&ctxt);

	// TODO: remove once code stable
	validate_avl_balancing(root);

	return target;
}
