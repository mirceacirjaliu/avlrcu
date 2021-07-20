
#define pr_fmt(fmt)	KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/rcupdate.h>
#include <linux/compiler.h>

#include "tree.h"

#define ASSERT(_expr) BUG_ON(!(_expr))


static void sptree_ctxt_init(struct sptree_ctxt *ctxt, struct sptree_root *root)
{
	ctxt->root = root;
	init_llist_head(&ctxt->old);
	ctxt->diff = 0;
}

/*
 * _delete_prealloc() - delete prealloc branch/subtree in case of failure
 * @ctxt:		AVL operations environment
 * @prealloc:		The root of the new branch
 *
 * The prealloc branch is not yet inserted into the tree.
 * Will do a post-order walk and delete the nodes.
 */
static void _delete_prealloc(struct sptree_ctxt *ctxt, struct sptree_node *prealloc)
{
	struct sptree_iterator iter;
	struct sptree_node *next;
	struct sptree_ops *ops = ctxt->root->ops;

	pr_debug("%s: start at "NODE_FMT"\n", __func__, NODE_ARG(prealloc));
	ASSERT(prealloc);
	ASSERT(is_new_branch(prealloc));
	ASSERT(is_root(get_parent(prealloc)) || !is_new_branch(get_parent(prealloc)));

	iter.node = prealloc;
	iter.state = ITER_UP;

	while (iter.node) {
		switch (iter.state) {
		case ITER_UP:
			next = iter.node->left;			// move to left node
			if (next && is_new_branch(next)) {
				iter.node = next;
				break;				// switch
			}
			else
				iter.state = ITER_LEFT;		// just handled left
				// fallback

		case ITER_LEFT:
			next = iter.node->right;		// move to right node
			if (next && is_new_branch(next)) {
				iter.node = next;
				iter.state = ITER_UP;
				break;				// switch
			}
			else
				iter.state = ITER_RIGHT;	// just handled right
				// fallback

		case ITER_RIGHT:
			next = get_parent(iter.node);		// move to parent
			if (next && is_new_branch(next)) {
				if (is_left_child(iter.node->parent))
					iter.state = ITER_LEFT;
			}
			else {
				iter.node = NULL;		// iteration finished
				iter.state = ITER_DONE;
			}

			ops->free(iter.node);

			iter.node = next;
			break;

		default:
			pr_err("%s: unhandled iterator state\n", __func__);
			BUG();
			iter.node = NULL;
			iter.state = ITER_DONE;
			break;
		}
	}
}

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
static void prealloc_connect(struct sptree_node **pbranch, struct sptree_node *branch)
{
	struct sptree_iterator io;
	struct sptree_node *next;

	if (branch == NULL)
		pr_debug("%s: at (EMPTY)\n", __func__);
	else
		pr_debug("%s: at "NODE_FMT"\n", __func__, NODE_ARG(branch));

	/* reverse-post-order traversal of links */
	io.node = branch;
	io.state = ITER_UP;

	while (likely(io.node)) {
		switch (io.state)
		{
		case ITER_UP:
			next = io.node->right;			// move to right node
			if (next) {
				if (is_new_branch(next)) {
					io.node = next;		// continue the right path
					break;			// switch
				}
				else {
					rcu_assign_pointer(next->parent, make_right(io.node));
					io.state = ITER_RIGHT;
					// fallback
				}
			}

		case ITER_RIGHT:
			next = io.node->left;			// move to the left node
			if (next) {
				if (is_new_branch(next)) {
					io.node = next;
					io.state = ITER_UP;	// another subtree
					break;			// switch
				}
				else {
					rcu_assign_pointer(next->parent, make_left(io.node));
					io.state = ITER_LEFT;
					// fallback
				}
			}

		case ITER_LEFT:
			next = strip_flags(io.node->parent);	// move to parent
			if (next && is_new_branch(next)) {
				if (!is_left_child(io.node->parent))
					io.state = ITER_RIGHT;
			}
			else {
				io.state = ITER_DONE;		// done!
				next = NULL;			// out of new branch
			}

			io.node->new_branch = 0;

			io.node = next;				// finally move to parent
			break;					// switch

		default:
			pr_err("%s: unhandled iterator state\n", __func__);
			BUG();
			return;
		}
	}

	/* ...or degenerate case (empty new branch) */
	rcu_assign_pointer(*pbranch, branch);				// finally link root
}

/*
 * prealloc_remove_old() - remove (RCU) nodes replaced by the new branch
 * @old:	The chain of old nodes.
 *
 * All nodes in the old chain will be passed to RCU for deletion.
 */
static void prealloc_remove_old(struct sptree_ctxt *ctxt)
{
	struct sptree_ops *ops = ctxt->root->ops;
	struct llist_node *node;
	struct sptree_node *old, *temp;

	node = __llist_del_all(&ctxt->old);
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
static struct sptree_node *prealloc_replace(struct sptree_ctxt *ctxt, struct sptree_node *target)
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
static struct sptree_node *prealloc_parent(struct sptree_ctxt *ctxt, struct sptree_node *child)
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



/* return new root to be set as top of branch */
static struct sptree_node *prealloc_retrace_ror(struct sptree_node *root)
{
	struct sptree_node *pivot = root->left;
	struct sptree_node *new_root = pivot;
	struct sptree_node *new_pivot = root;

	pr_debug("%s: root at "NODE_FMT", pivot at "NODE_FMT"\n",
		__func__, NODE_ARG(root), NODE_ARG(pivot));
	BUG_ON(!is_new_branch(root));
	BUG_ON(!is_new_branch(pivot));

	// redistribute t2
	new_pivot->left = pivot->right;
	if (pivot->right && is_new_branch(pivot->right))
		pivot->right->parent = make_left(new_pivot);

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
	struct sptree_node *new_root = right;		// new Y
	struct sptree_node *new_left = left;		// new Z
	struct sptree_node *new_right = root;		// new X

	pr_debug("%s: root at "NODE_FMT"\n", __func__, NODE_ARG(root));
	BUG_ON(!is_new_branch(root));
	BUG_ON(!is_new_branch(left));
	BUG_ON(!is_new_branch(right));

	// redistribute t2, t3
	new_left->right = right->left;
	if (right->left && is_new_branch(right->left))
		right->left->parent = make_right(new_left);

	new_right->left = right->right;
	if (right->right && is_new_branch(right->right))
		right->right->parent = make_left(new_right);

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
	struct sptree_node *new_root = pivot;
	struct sptree_node *new_pivot = root;

	pr_debug("%s: root at "NODE_FMT", pivot at "NODE_FMT"\n",
		__func__, NODE_ARG(root), NODE_ARG(pivot));
	BUG_ON(!is_new_branch(root));
	BUG_ON(!is_new_branch(pivot));

	// redistribute t2
	new_pivot->right = pivot->left;
	if (pivot->left && is_new_branch(pivot->left))
		pivot->left->parent = make_right(new_pivot);

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
	struct sptree_node *new_root = left;		// new Y
	struct sptree_node *new_left = root;		// new X
	struct sptree_node *new_right = right;		// new Z

	pr_debug("%s: root at "NODE_FMT"\n", __func__, NODE_ARG(root));
	BUG_ON(!is_new_branch(root));
	BUG_ON(!is_new_branch(right));
	BUG_ON(!is_new_branch(left));

	// redistribute t2, t3
	new_left->right = left->left;
	if (left->left && is_new_branch(left->left))
		left->left->parent = make_right(new_left);

	new_right->left = left->right;
	if (left->right && is_new_branch(left->right))
		left->right->parent = make_left(new_right);

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

	// parent may contain L/R flags or NULL
	parent = prealloc->parent;
	pbranch = get_pnode(root, parent);

	// connect the preallocated branch
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
 * prealloc_child() - brings a child to the new branch
 * @ctxt - AVL operation environment
 * @parent - a leaf on the new branch
 * @which - LEFT or RIGHT child
 *
 * Only useful for unwind, cause it descents and must find a way down.
 * The parent needs to be part of the new branch (the child points to old parent).
 */
static struct sptree_node *prealloc_child(struct sptree_ctxt *ctxt, struct sptree_node *parent, int which)
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
 */
static void prealloc_propagate_change(struct sptree_ctxt *ctxt, struct sptree_node *subtree, int diff)
{
	struct sptree_node *parent;
	bool left_child, balance_before;

	pr_debug("%s: starting at "NODE_FMT"\n", __func__, NODE_ARG(subtree));
	ASSERT(is_new_branch(subtree));

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

/* generic rotation for unwind & fix steps */
static struct sptree_node *prealloc_rol(struct sptree_ctxt *ctxt, struct sptree_node *target)
{
	struct sptree_node *parent, **ptarget;
	struct sptree_node *pivot = target->right;
	struct sptree_node *new_root, *new_pivot;
	struct sptree_node *t2;
	struct balance_factors new_balance;
	int diff_height;

	pr_debug("%s: at "NODE_FMT"\n", __func__, NODE_ARG(target));
	ASSERT(is_new_branch(target));

	/* copy pivot if not already part of the new branch */
	if (!is_new_branch(pivot)) {
		pivot = prealloc_child(ctxt, target, RIGHT_CHILD);
		if (!pivot)
			return NULL;					/* only error output */
	}

	/* compute new balance factors & tree height diff after rotation */
	diff_height = rol_height_diff(target->balance, pivot->balance);
	new_balance = rol_new_balance(target->balance, pivot->balance);

	/* make the algorithm easier to read */
	t2 = pivot->left;
	new_root = pivot;
	new_pivot = target;

	/* redistribute t2 */
	new_pivot->right = t2;
	if (t2 && is_new_branch(t2))
		t2->parent = make_right(new_pivot);

	/* relink parents */
	parent = strip_flags(target->parent);
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

static struct sptree_node *prealloc_ror(struct sptree_ctxt *ctxt, struct sptree_node *target)
{
	struct sptree_node *parent, **ptarget;
	struct sptree_node *pivot = target->left;
	struct sptree_node *new_root, *new_pivot;
	struct sptree_node *t2;
	struct balance_factors new_balance;
	int diff_height;

	pr_debug("%s: at "NODE_FMT"\n", __func__, NODE_ARG(target));
	ASSERT(is_new_branch(target));

	/* copy pivot if not already part of the new branch */
	if (!is_new_branch(pivot)) {
		pivot = prealloc_child(ctxt, target, LEFT_CHILD);
		if (!pivot)
			return NULL;					/* only error output */
	}

	/* compute new balance factors & tree height diff after rotation */
	diff_height = ror_height_diff(target->balance, pivot->balance);
	new_balance = ror_new_balance(target->balance, pivot->balance);

	/* make the algorithm easier to read */
	t2 = pivot->right;
	new_root = pivot;
	new_pivot = target;

	/* redistribute t2 */
	new_pivot->left = t2;
	if (t2 && is_new_branch(t2))
		t2->parent = make_left(new_pivot);

	/* relink parents */
	parent = strip_flags(target->parent);
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

static struct sptree_node *prealloc_rrl(struct sptree_ctxt *ctxt, struct sptree_node *target)
{
	struct sptree_node *pivot = target->right;

	pr_debug("%s: at "NODE_FMT"\n", __func__, NODE_ARG(target));
	ASSERT(is_new_branch(target));

	/* copy pivot if not already part of the new branch */
	if (!is_new_branch(pivot)) {
		pivot = prealloc_child(ctxt, target, RIGHT_CHILD);
		if (!pivot)
			return NULL;
	}

	/* first rotation */
	pivot = prealloc_ror(ctxt, pivot);
	if (!pivot)
		return NULL;

	/* second rotation */
	target = prealloc_rol(ctxt, target);

	return target;
}

static struct sptree_node *prealloc_rlr(struct sptree_ctxt *ctxt, struct sptree_node *target)
{
	struct sptree_node *pivot = target->left;

	pr_debug("%s: at "NODE_FMT"\n", __func__, NODE_ARG(target));
	ASSERT(is_new_branch(target));

	/* copy pivot if not already part of the new branch */
	if (!is_new_branch(pivot)) {
		pivot = prealloc_child(ctxt, target, LEFT_CHILD);
		if (!pivot)
			return NULL;
	}

	/* first rotation */
	pivot = prealloc_rol(ctxt, pivot);
	if (!pivot)
		return NULL;

	/* second rotation */
	target = prealloc_ror(ctxt, target);

	return target;
}


/*
 * poor_balance_depth() - count the number of moves needed to rebalance a poorly balanced subtree
 * @target - first poorly balanced node (root of the subtree)
 *
 * A poorly balanced subtree can be recursive. A single rebalance will not be enough.
 * This function counts the number of recursive rebalances needed to rebalance this subtree.
 */
static int poor_balance_depth(struct sptree_node *target)
{
	struct sptree_iterator iter;
	int count = 0;

	pr_debug("%s: at "NODE_FMT"\n", __func__, NODE_ARG(target));
	ASSERT(target->balance == -1 || target->balance == 1);
	ASSERT(!is_leaf(target));	// kinda useless assert

	iter.node = target;
	iter.state = target->balance == -1 ? ITER_RIGHT : ITER_LEFT;

	while (!is_leaf(iter.node)) {
		/* check condition */
		if ((iter.state == ITER_LEFT && iter.node->balance == 1) ||
			(iter.state == ITER_RIGHT && iter.node->balance == -1))
			count++;
		else
			break;

		/* go down the branch */
		if (iter.state == ITER_LEFT) {
			iter.state = ITER_RIGHT;
			iter.node = iter.node->right;
		}
		else {
			iter.state = ITER_LEFT;
			iter.node = iter.node->left;
		}
	}

	return count;
}

/*
 * prealloc_rebalance() - recursively rebalance a poorly balanced subtree
 * @ctxt - AVL operations environment
 * @target - root of the poorly balanced subtree
 *
 * A poorly balanced subtree can be recursive. A single rebalance will not be enough.
 * This function will recursvely rebalance the poorly balanced branch.
 * This subtree will not increase in height (according to rotation tables).
 * Returns the new root of the subtree or NULL on error.
 */
static struct sptree_node *prealloc_rebalance(struct sptree_ctxt *ctxt, struct sptree_node *target)
{
	struct sptree_iterator iter;
	struct sptree_node *child;
	bool stop = false;

	pr_debug("%s: at "NODE_FMT"\n", __func__, NODE_ARG(target));
	ASSERT(is_new_branch(target));
	ASSERT(target->balance == -1 || target->balance == 1);

	iter.node = target;
	iter.state = (target->balance == 1) ? ITER_LEFT : ITER_RIGHT;

	/* descend along poorly balanced branch */
	while (true) {
		if (iter.state == ITER_LEFT && iter.node->balance == 1) {
			/* prealloc child anyway, it's either used for rotation or poorly balanced */
			child = prealloc_child(ctxt, iter.node, RIGHT_CHILD);
			if (!child)
				return NULL;

			/* still a poorly balanced child ? */
			if (child->balance == -1) {
				iter.node = child;
				iter.state = ITER_RIGHT;
				continue;	/* next poorly balanced node */
			}
			else
				break;		/* bottom poorly balanced node */
		}

		if (iter.state == ITER_RIGHT && iter.node->balance == -1) {
			/* prealloc child anyway, it's either used for rotation or poorly balanced */
			child = prealloc_child(ctxt, iter.node, LEFT_CHILD);
			if (!child)
				return NULL;

			/* still a poorly balanced child ? */
			if (child->balance == 1) {
				iter.node = child;
				iter.state = ITER_LEFT;
				continue;	/* next poorly balanced node */
			}
			else
				break;		/* bottom poorly balanced node */
		}

		/* shouldn't reach this place */
		pr_err("%s: invalid case at "NODE_FMT"\n", __func__, NODE_ARG(iter.node));
		BUG();
	}

	/* we are at the bottom poorly balanced node */
	ASSERT(is_new_branch(iter.node));

	/* ascend & rebalance the tree */
	do {
		if (iter.node == target)
			stop = true;

		if (iter.state == ITER_LEFT) {
			ASSERT(iter.node->balance == 1);

			iter.node = prealloc_rol(ctxt, iter.node);
			if (!iter.node)
				return NULL;

			if (!stop) {
				iter.node = get_parent(iter.node);
				iter.state = ITER_RIGHT;
			}
		}
		else {
			ASSERT(iter.node->balance == -1);

			iter.node = prealloc_ror(ctxt, iter.node);
			if (!iter.node)
				return NULL;

			if (!stop) {
				iter.node = get_parent(iter.node);
				iter.state = ITER_LEFT;
			}
		}
	} while (!stop);

	return iter.node;
}

static struct sptree_node *prealloc_reverse_rrl(struct sptree_ctxt *ctxt, struct sptree_node *target)
{
	struct sptree_node *subtree_root;

	pr_debug("%s: at "NODE_FMT"\n", __func__, NODE_ARG(target));

	/* the new subtree root is established after the 1st rotation */
	subtree_root = prealloc_ror(ctxt, target);
	if (!subtree_root)
		return NULL;
	target = subtree_root->right;

	/* the 2nd rotation may add to the overall unbalancing/diff in height */
	target = prealloc_rol(ctxt, target);
	if (!target)
		return NULL;
	target = target->left;

	return target;
}

static struct sptree_node *prealloc_reverse_rlr(struct sptree_ctxt *ctxt, struct sptree_node *target)
{
	struct sptree_node *subtree_root;

	pr_debug("%s: at "NODE_FMT"\n", __func__, NODE_ARG(target));

	/* the new subtree root is established after the 1st rotation */
	subtree_root = prealloc_rol(ctxt, target);
	if (!subtree_root)
		return NULL;
	target = subtree_root->left;

	/* the 2nd rotation may add to the overall unbalancing/diff in height */
	target = prealloc_ror(ctxt, target);
	if (!target)
		return NULL;
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

	target = prealloc_rol(ctxt, target);
	if (!target)
		return NULL;

	/* rotation functions return the new subtree root */
	return target->left;
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

	target = prealloc_ror(ctxt, target);
	if (!target)
		return NULL;

	/* rotation functions return the new subtree root */
	return target->right;
}

/*
 * prealloc_top() - iterate to the top of the new branch
 * @target - a node on the new branch
 *
 * Used for failure cases in unwind/fix/retrace, when one needs
 * the top of the new branch to delete it.
 * Always returns a valid value;
 */
static struct sptree_node *prealloc_top(struct sptree_node *target)
{
	struct sptree_node *parent;

	pr_debug("%s: at "NODE_FMT"\n", __func__, NODE_ARG(target));
	ASSERT(is_new_branch(target));

	for (parent = get_parent(target); !is_root(parent) && is_new_branch(parent); parent = get_parent(target))
		target = parent;

	return target;
}

/* _prealloc_unwind() - bubble a node down to a leaf
 * @ctxt - AVL operations environment
 * @target - node to be bublled
 *
 * This is called only if the node is not a leaf, so must return a preallocated branch != NULL.
 * It takes the initial target node and bubbles it down to a leaf node -> which will later be deleted.
 * Will return NULL on error.
 * WARNING: this function does not return the top of the breallocated branch, but the bottom !!
 */
static struct sptree_node *_prealloc_unwind(struct sptree_ctxt *ctxt, struct sptree_node *target)
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
		BUG_ON(target->balance < -1 || target->balance > 1);

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

// TODO: temp entry point for testing the unwind function
int prealloc_unwind(struct sptree_root *root, unsigned long key)
{
	struct sptree_node *target, *parent, **pbranch;
	struct sptree_node *prealloc;
	struct sptree_ctxt ctxt;

	target = search(root, key);
	if (!target)
		return -ENXIO;

	// nothing to do in this case
	if (is_leaf(target))
		return 0;

	sptree_ctxt_init(&ctxt, root);

	prealloc = _prealloc_unwind(&ctxt, target);
	if (!prealloc)
		return -ENOMEM;

	// the unwind function returns the bottom of the preallocated branch
	prealloc = prealloc_top(prealloc);

	pr_debug("%s: overall increase in height %d\n", __func__, ctxt.diff);

	// parent may contain L/R flags or NULL
	parent = prealloc->parent;
	pbranch = get_pnode(root, parent);

	// connect the preallocated branch
	prealloc_connect(pbranch, prealloc);

	// this will remove the replaced nodes
	if (!llist_empty(&ctxt.old))
		prealloc_remove_old(&ctxt);

	// TODO: remove once code stable
	validate_avl_balancing(root);

	return 0;
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
		leaf = _prealloc_unwind(ctxt, node);
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
 * Returns:	0 - success
 *		-ENXIO - node was not found
 *		-ENOMEM - allocations failed
 * On error, the tree is not modified.
 */
int prealloc_delete(struct sptree_root *root, unsigned long key)
{
	struct sptree_node *target, *parent, **pbranch;
	struct sptree_node *prealloc;
	struct sptree_ctxt ctxt;

	target = search(root, key);
	if (!target)
		return -ENXIO;

	// parent may contain L/R flags or NULL
	parent = target->parent;

	sptree_ctxt_init(&ctxt, root);

	/* may return NULL as a valid value !!! */
	prealloc = unwind_delete_retrace(&ctxt, target);
	if (IS_ERR(prealloc))
		return PTR_ERR(prealloc);

	/* new branch may climb after retrace & take over the old parent */
	if (prealloc)
		parent = prealloc->parent;
	pbranch = get_pnode(root, parent);

	// connect the preallocated branch
	prealloc_connect(pbranch, prealloc);

	// this will remove the replaced nodes
	if (!llist_empty(&ctxt.old))
		prealloc_remove_old(&ctxt);

	// TODO: remove once code stable
	validate_avl_balancing(root);

	return 0;
}
