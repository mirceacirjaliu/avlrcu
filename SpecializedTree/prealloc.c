
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
	ctxt->old = NULL;
	ctxt->diff = 0;
}

/**
 * _delete_prealloc() - delete prealloc branch/subtree in case of failure
 * @prealloc:		The root of the new branch
 *
 * The prealloc branch is not yet inserted into the tree.
 * Will do a post-order walk and delete the nodes.
 */
/* TODO: will have to receive thr root arg for ops->free() instead of kfree */
static void _delete_prealloc(struct sptree_node *prealloc)
{
	struct sptree_iterator iter;
	struct sptree_node *next;

	pr_info("%s: start at "NODE_FMT"\n", __func__, NODE_ARG(prealloc));
	BUG_ON(prealloc == NULL);
	BUG_ON(!is_new_branch(prealloc));

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

			kfree(iter.node);			// delete current node

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
		pr_info("%s: at (EMPTY)\n", __func__);
	else
		pr_info("%s: at "NODE_FMT"\n", __func__, NODE_ARG(branch));

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
					WRITE_ONCE(next->parent, make_right(io.node));
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
					WRITE_ONCE(next->parent, make_left(io.node));
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
	WRITE_ONCE(*pbranch, branch);				// finally link root
}

/**
 * prealloc_remove_old() - remove (RCU) nodes replaced by the new branch
 * @old:	The chain of old nodes.
 *
 * All nodes in the old chain will be passed to RCU for deletion.
 */
static void prealloc_remove_old(struct sptree_node *old)
{
	struct sptree_node *temp;

	while (old) {
		temp = old->old;
		kfree_rcu(old, rcu);
		old = temp;
	}
}



/* return new root to be set as top of branch */
static struct sptree_node *prealloc_retrace_ror(struct sptree_node *root)
{
	struct sptree_node *pivot = root->left;
	struct sptree_node *new_root = pivot;
	struct sptree_node *new_pivot = root;

	pr_info("%s: root at "NODE_FMT", pivot at "NODE_FMT"\n",
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

	pr_info("%s: new root is "NODE_FMT"\n", __func__, NODE_ARG(new_root));

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

	pr_info("%s: root at "NODE_FMT"\n", __func__, NODE_ARG(root));
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

	pr_info("%s: new root is "NODE_FMT"\n", __func__, NODE_ARG(new_root));

	return new_root;
}

/* return new root to be set as top of branch */
static struct sptree_node *prealloc_retrace_rol(struct sptree_node *root)
{
	struct sptree_node *pivot = root->right;
	struct sptree_node *new_root = pivot;
	struct sptree_node *new_pivot = root;

	pr_info("%s: root at "NODE_FMT", pivot at "NODE_FMT"\n",
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

	pr_info("%s: new root is "NODE_FMT"\n",
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

	pr_info("%s: root at "NODE_FMT"\n", __func__, NODE_ARG(root));
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

	pr_info("%s: new root is "NODE_FMT"\n", __func__, NODE_ARG(new_root));

	return new_root;
}

/* retrace for insert */
static struct sptree_node *prealloc_retrace(struct sptree_ctxt *ctxt, struct sptree_node *node)
{
	struct sptree_node *parent;
	struct sptree_node *branch = node;
	struct sptree_node *new_parent = NULL;

	/* node is the latest member of branch */
	pr_info("%s: starting at "NODE_FMT"\n", __func__, NODE_ARG(node));

	for (parent = strip_flags(node->parent); parent != NULL; node = parent, parent = strip_flags(node->parent)) {

		pr_info("%s: loop on parent "NODE_FMT", node is "NODE_FMT"\n",
			__func__, NODE_ARG(parent), NODE_ARG(node));

		new_parent = kzalloc(sizeof(*new_parent), GFP_ATOMIC);
		if (!new_parent)
			goto error;

		memcpy(new_parent, parent, sizeof(*new_parent));
		new_parent->new_branch = 1;

		parent->old = ctxt->old;					/* this node will be replaced */
		ctxt->old = parent;						/* add to chain of old nodes */

		if (is_left_child(node->parent)) {
			pr_info("%s: node "NODE_FMT" is left child of "NODE_FMT"\n", __func__,
				NODE_ARG(node), NODE_ARG(parent));

			/* link top of branch to new parent */
			new_parent->left = branch;
			branch->parent = make_left(new_parent);

			// parent is left-heavy
			if (parent->balance < 0) {
				// node is right-heavy
				if (branch->balance > 0)
					branch = prealloc_retrace_rlr(new_parent);
				else
					branch = prealloc_retrace_ror(new_parent);
				break;
			}
			// parent is right-heavy
			else if (parent->balance > 0) {
				new_parent->balance = 0;			/* parent becomes balanced */
				branch = new_parent;				/* push parent to branch */

				pr_info("%s: parent "NODE_FMT" becomes balanced, stop\n",
					__func__, NODE_ARG(parent));
				break;
			}
			// parent is balanced
			else {
				new_parent->balance = -1;			/* parent becomes left-heavy */
				branch = new_parent;				/* push parent to branch */

				pr_info("%s: parent "NODE_FMT" becomes left-heavy, continue\n",
					__func__, NODE_ARG(parent));
			}
		}
		// is right child
		else {
			pr_info("%s: node "NODE_FMT" is right child of "NODE_FMT"\n", __func__,
				NODE_ARG(node), NODE_ARG(parent));

			/* link top of branch to new parent */
			new_parent->right = branch;
			branch->parent = make_right(new_parent);

			// parent is right heavy
			if (parent->balance > 0) {
				// node is left-heavy
				if (branch->balance < 0)
					branch = prealloc_retrace_rrl(new_parent);
				else
					branch = prealloc_retrace_rol(new_parent);
				break;
			}
			// parent is left-heavy
			else if (parent->balance < 0) {
				new_parent->balance = 0;			/* parent becomes balanced */
				branch = new_parent;				/* push parent to branch */

				pr_info("%s: parent "NODE_FMT" becomes balanced, stop\n",
					__func__, NODE_ARG(parent));
				break;
			}
			// parent is balanced
			else {
				new_parent->balance = 1;			/* parent becomes right-heavy */
				branch = new_parent;				/* push parent to branch */

				pr_info("%s: parent "NODE_FMT" becomes right-heavy, continue\n",
					__func__, NODE_ARG(parent));
			}
		}
	}

	return branch;

error:
	_delete_prealloc(branch);

	return NULL;
};

/* entry point */
int prealloc_insert(struct sptree_root *root, unsigned long addr)
{
	struct sptree_node *crnt, *parent, **pbranch;
	struct sptree_node *prealloc;
	struct sptree_ctxt ctxt;

	if (!address_valid(root, addr))
		return -EINVAL;

	/* look for a parent */
	for (crnt = root->root, parent = NULL; crnt != NULL; ) {
		if (unlikely(addr == crnt->start))
			return -EINVAL;
		else if (addr < crnt->start) {
			parent = make_left(crnt);
			crnt = crnt->left;
		}
		else {
			parent = make_right(crnt);
			crnt = crnt->right;
		}
	}

	/* alloc the new node & fill it - this is the first node in the preallocated branch */
	prealloc = kzalloc(sizeof(*prealloc), GFP_ATOMIC);
	if (!prealloc)
		return -ENOMEM;

	prealloc->start = addr;
	prealloc->parent = parent;		/* only link one way */
	prealloc->new_branch = 1;

	sptree_ctxt_init(&ctxt, root);

	/* retrace generates the preallocated branch */
	prealloc = prealloc_retrace(&ctxt, prealloc);
	if (!prealloc)
		return -ENOMEM;

	// parent may contain L/R flags or NULL
	parent = prealloc->parent;
	pbranch = get_pnode(root, parent);

	// connect the preallocated branch
	prealloc_connect(pbranch, prealloc);

	// this will remove the replaced nodes
	if (ctxt.old)
		prealloc_remove_old(ctxt.old);

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

/* brings a parent to the new branch (one of the children is already on the new branch) */
/* useful for retrace where the algorithm ascends */
static struct sptree_node *prealloc_parent(struct sptree_node *parent, struct sptree_node **old)
{
	struct sptree_node *new_parent;

	pr_info("%s: at "NODE_FMT"\n", __func__, NODE_ARG(parent));
	BUG_ON(is_new_branch(parent));

	new_parent = kzalloc(sizeof(*new_parent), GFP_ATOMIC);
	if (!new_parent)
		return NULL;						/* only error output */

	memcpy(new_parent, parent, sizeof(*new_parent));
	new_parent->new_branch = 1;

	/* one of the children is on the new branch, but we test both */
	if (is_new_branch(parent->left))
		parent->left->parent = make_left(new_parent);

	if (is_new_branch(parent->right))
		parent->right->parent = make_right(new_parent);

	parent->old = *old;						/* this node will be replaced */
	*old = parent;							/* add to chain of old nodes */

	return new_parent;
}

/* brings a child to the new branch  */
/* only useful for unwind, cause it descents and must find a way down */
/* the parent needs to be part of the new branch (the child points to old parent) */
static struct sptree_node *prealloc_child(struct sptree_node *target, int which_child, struct sptree_node **old)
{
	struct sptree_node *child = which_child == LEFT_CHILD ? target->left : target->right;
	struct sptree_node *new_child;

	pr_info("%s: at "NODE_FMT"\n", __func__, NODE_ARG(target));
	BUG_ON(!is_new_branch(target));
	BUG_ON(!child);
	BUG_ON(is_new_branch(child));

	new_child = kzalloc(sizeof(*new_child), GFP_ATOMIC);
	if (!new_child)
		return NULL;						/* only error output */

	memcpy(new_child, child, sizeof(*new_child));			/* copy pivot */
	new_child->new_branch = 1;

	if (which_child == LEFT_CHILD) {
		target->left = new_child;
		new_child->parent = make_left(target);			/* create links on the new branch */
	}
	else {
		target->right = new_child;
		new_child->parent = make_right(target);			/* create links on the new branch */
	}

	child->old = *old;						/* this node will be replaced */
	*old = child;							/* add to chain of old nodes */

	return new_child;
}

/*
 * prealloc_change_height() - propagate height diff up the new branch
 * @subtree:	- root of the subtree that changed height during a rotation
 * @diff	- height diff +-1
 *
 * Under certain operations, the height of the subtree changes, and
 * the changes must be propagated up to the root (of the new branch).
 * If a subtree is rotated, the balances of the root & pivot are changed by
 * the rotation, but the height diff is propagated starting with the parent.
 * On a node deletion, the node is bubbled down to a leaf node and deleted,
 * there is a decrease in height starting with the parent of the deleted leaf.
 */
static void prealloc_change_height(struct sptree_node *subtree, int diff)
{
	struct sptree_node *parent;
	bool left, balanced_before;

	pr_info("%s: starting at "NODE_FMT"\n", __func__, NODE_ARG(subtree));

	// parent may contain L/R flags or NULL, strip flags before using as pointer
	for (parent = subtree->parent; !is_root(parent); parent = parent->parent) {
		left = is_left_child(parent);
		parent = strip_flags(parent);

		// act only on the new branch
		if (!is_new_branch(parent))
			break;

		// parent was balanced before applying the diff
		balanced_before = (parent->balance == 0);

		if (left)
			parent->balance -= diff;
		else
			parent->balance += diff;

		// on delete, the propagation stops when it encounters a balanced parent
		// (the height of the subtree remains unchanged)
		if (diff == -1 && balanced_before) {
			pr_info("%s: stop update balance factor at "NODE_FMT"\n",
				__func__, NODE_ARG(parent));
			break;
		}

		pr_info("%s: updated balance factor for "NODE_FMT"\n",
			__func__, NODE_ARG(parent));
	}
}

/* generic rotation for unwind & fix steps */
static struct sptree_node *prealloc_rol(struct sptree_root *root, struct sptree_node *target, struct sptree_node **old)
{
	struct sptree_node *parent, **ptarget;
	struct sptree_node *pivot = target->right;
	struct sptree_node *new_root, *new_pivot;
	struct sptree_node *t2;
	struct balance_factors new_balance;
	int diff_height;

	pr_info("%s: at "NODE_FMT"\n", __func__, NODE_ARG(target));
	BUG_ON(!is_new_branch(target));

	/* copy pivot if not already part of the new branch */
	if (!is_new_branch(pivot)) {
		pivot = prealloc_child(target, RIGHT_CHILD, old);
		if (!pivot)
			return ERR_PTR(-ENOMEM);				/* only error output */
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
	ptarget = get_pnode(root, target->parent);

	new_root->parent = target->parent;
	if (parent && is_new_branch(parent))
		*ptarget = new_root;

	new_root->left = new_pivot;
	new_pivot->parent = make_left(new_root);

	/* fix balance factors & height diff */
	new_root->balance = new_balance.root;
	new_pivot->balance = new_balance.pivot;
	if (diff_height)
		prealloc_change_height(new_root, diff_height);

	return new_root;
}

static struct sptree_node *prealloc_ror(struct sptree_root *root, struct sptree_node *target, struct sptree_node **old)
{
	struct sptree_node *parent, **ptarget;
	struct sptree_node *pivot = target->left;
	struct sptree_node *new_root, *new_pivot;
	struct sptree_node *t2;
	struct balance_factors new_balance;
	int diff_height;

	pr_info("%s: at "NODE_FMT"\n", __func__, NODE_ARG(target));
	BUG_ON(!is_new_branch(target));

	/* copy pivot if not already part of the new branch */
	if (!is_new_branch(pivot)) {
		pivot = prealloc_child(target, LEFT_CHILD, old);
		if (!pivot)
			return ERR_PTR(-ENOMEM);				/* only error output */
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
	ptarget = get_pnode(root, target->parent);

	new_root->parent = target->parent;
	if (parent && is_new_branch(parent))
		*ptarget = new_root;

	new_root->right = new_pivot;
	new_pivot->parent = make_right(new_root);

	/* fix balance factors & height diff */
	new_root->balance = new_balance.root;
	new_pivot->balance = new_balance.pivot;
	if (diff_height)
		prealloc_change_height(new_root, diff_height);

	return new_root;
}

static struct sptree_node *prealloc_rrl(struct sptree_root *root, struct sptree_node *target, struct sptree_node **old)
{
	struct sptree_node *pivot = target->right;

	pr_info("%s: at "NODE_FMT"\n", __func__, NODE_ARG(target));
	BUG_ON(!is_new_branch(target));

	/* copy pivot if not already part of the new branch */
	if (!is_new_branch(pivot)) {
		pivot = prealloc_child(target, RIGHT_CHILD, old);
		if (!pivot)
			return ERR_PTR(-ENOMEM);				/* only error output */
	}

	/* first rotation */
	pivot = prealloc_ror(root, pivot, old);
	if (!pivot)
		return ERR_PTR(-ENOMEM);

	/* second rotation */
	target = prealloc_rol(root, target, old);

	return target;
}

static struct sptree_node *prealloc_rlr(struct sptree_root *root, struct sptree_node *target, struct sptree_node **old)
{
	struct sptree_node *pivot = target->left;

	pr_info("%s: at "NODE_FMT"\n", __func__, NODE_ARG(target));
	BUG_ON(!is_new_branch(target));

	/* copy pivot if not already part of the new branch */
	if (!is_new_branch(pivot)) {
		pivot = prealloc_child(target, LEFT_CHILD, old);
		if (!pivot)
			return ERR_PTR(-ENOMEM);				/* only error output */
	}

	/* first rotation */
	pivot = prealloc_rol(root, pivot, old);
	if (!pivot)
		return ERR_PTR(-ENOMEM);

	/* second rotation */
	target = prealloc_ror(root, target, old);

	return target;
}







static struct sptree_node *prealloc_reverse_rrl(struct sptree_root *root, struct sptree_node *target, struct sptree_node **old)
{
	struct sptree_node *subtree_root;

	pr_info("%s: at "NODE_FMT"\n", __func__, NODE_ARG(target));

	/* the new subtree root is established after the 1st rotation */
	subtree_root = prealloc_ror(root, target, old);
	if (IS_ERR(subtree_root))
		return subtree_root;
	target = subtree_root->right;

	/* the 2nd rotation may add to the overall unbalancing/diff in height */
	target = prealloc_rol(root, target, old);
	if (IS_ERR(target))
		return target;
	target = target->left;

	return target;
}

static struct sptree_node *prealloc_reverse_rlr(struct sptree_root *root, struct sptree_node *target, struct sptree_node **old)
{
	struct sptree_node *subtree_root;

	pr_info("%s: at "NODE_FMT"\n", __func__, NODE_ARG(target));

	/* the new subtree root is established after the 1st rotation */
	subtree_root = prealloc_rol(root, target, old);
	if (IS_ERR(subtree_root))
		return subtree_root;
	target = subtree_root->left;

	/* the 2nd rotation may add to the overall unbalancing/diff in height */
	target = prealloc_ror(root, target, old);
	if (IS_ERR(target))
		return target;
	target = target->right;

	return target;
}

static struct sptree_node *prealloc_unwind_double(struct sptree_root *root, struct sptree_node *target, struct sptree_node **old)
{
	struct sptree_node *left = target->left;
	struct sptree_node *right = target->right;
	struct sptree_node *temp;

	pr_info("%s: at "NODE_FMT"\n", __func__, NODE_ARG(target));
	BUG_ON(is_leaf(target));

	/* prealloc the left and right pivots,
	 * both will be used in reverse compound rotations */
	left = prealloc_child(target, LEFT_CHILD, old);
	if (!left)
		return ERR_PTR(-ENOMEM);

	right = prealloc_child(target, RIGHT_CHILD, old);
	if (!right)
		return ERR_PTR(-ENOMEM);

	/* choose direction based on the balance of the subtrees
	 * a rotation will give the new pivot the nearest subtree of the old pivot */

	// reverse RRL
	if (left->balance == -1)
		return prealloc_reverse_rrl(root, target, old);

	// reverse RLR
	if (right->balance == 1)
		return prealloc_reverse_rlr(root, target, old);

	// don't care: so reverse RRL
	if (right->balance == 0 && left->balance == 0)
		return prealloc_reverse_rrl(root, target, old);

	// both poorly balanced: rebalance one of them
	if (right->balance == -1 && left->balance == 1) {
		// rebalance the right subtree, then apply case 2
		temp = prealloc_ror(root, right, old);
		if (IS_ERR(temp))
			return temp;
		return prealloc_reverse_rlr(root, target, old);
	}

	// rebalance left
	if (right->balance == 0 && left->balance == 1) {
		// rebalance the left subtree, then apply case 1
		temp = prealloc_rol(root, left, old);
		if (IS_ERR(temp))
			return temp;
		return prealloc_reverse_rrl(root, target, old);
	}

	// rebalance right
	if (right->balance == -1 && left->balance == 0) {
		// rebalance the right subtree, then apply case 2
		temp = prealloc_ror(root, right, old);
		if (IS_ERR(temp))
			return temp;
		return prealloc_reverse_rlr(root, target, old);
	} // TODO: this is the same case as "both poorly balanced", merge

	/* did I miss something ? */
	pr_err("%s: invalid case at "NODE_FMT", left "NODE_FMT", right "NODE_FMT"\n",
		__func__, NODE_ARG(target), NODE_ARG(left), NODE_ARG(right));
	BUG();
	return ERR_PTR(-EINVAL);
}

/* target->balance == 1 */
static struct sptree_node *prealloc_unwind_left(struct sptree_root *root, struct sptree_node *target, struct sptree_node **old)
{
	struct sptree_node *pivot = target->right;

	pr_info("%s: at "NODE_FMT"\n", __func__, NODE_ARG(target));
	BUG_ON(!pivot);

	if (pivot->balance == -1)
		target = prealloc_rrl(root, target, old);
	else
		target = prealloc_rol(root, target, old);

	/* allocations inside these funcs may fail */
	if (IS_ERR(target))
		return target;

	/* rotation functions return the new subtree root */
	return target->left;
}

/* target->balance == -1 */
static struct sptree_node *prealloc_unwind_right(struct sptree_root *root, struct sptree_node *target, struct sptree_node **old)
{
	struct sptree_node *pivot = target->left;

	pr_info("%s: at "NODE_FMT"\n", __func__, NODE_ARG(target));
	BUG_ON(!pivot);

	if (pivot->balance == 1)
		target = prealloc_rlr(root, target, old);
	else
		target = prealloc_ror(root, target, old);

	/* allocations inside these funcs may fail */
	if (IS_ERR(target))
		return target;

	/* rotation functions return the new subtree root */
	return target->right;
}

/* this is called only if the node is not a leaf, so must return a preallocated branch != NULL */
/* it takes the initial target node and bubbles it down to a leaf node -> which will later be deleted */
/* will return NULL on error */
/* WARNING: this function does not return the top of the breallocated branch, but the bottom */
static struct sptree_node *_prealloc_unwind(struct sptree_root *root, struct sptree_node *target, struct sptree_node **old)
{
	struct sptree_node *prealloc, *parent;

	pr_info("%s: at "NODE_FMT"\n", __func__, NODE_ARG(target));
	BUG_ON(is_leaf(target));

	/* start by allocating a node that replaces target */
	prealloc = kzalloc(sizeof(*prealloc), GFP_ATOMIC);
	if (!prealloc)
		goto error;

	memcpy(prealloc, target, sizeof(*prealloc));
	prealloc->new_branch = 1;

	target->old = *old;						/* this node will be replaced */
	*old = target;							/* add to chain of old nodes */

	/* walk the new branch, add nodes that represent the bubbling sequence */
	do {
		/* target contains the top of the prealloc branch */
		target = prealloc;

		/* each unwinding step must start with a normal balance */
		BUG_ON(target->balance < -1 || target->balance > 1);

		switch (target->balance) {
		case -1:
			prealloc = prealloc_unwind_right(root, target, old);
			break;

		case 0:
			prealloc = prealloc_unwind_double(root, target, old);
			break;

		case 1:
			prealloc = prealloc_unwind_left(root, target, old);
			break;

		default:
			pr_err("%s: invalid case at "NODE_FMT"\n", __func__, NODE_ARG(target));
			BUG();
			return NULL;
		}

		/* check if the allocation succeeded */
		if (IS_ERR(prealloc))
			goto error;

	} while (!is_leaf(prealloc));

	/* return the bottom of the preallocated branch */
	return prealloc;

error:
	// walk the parent to the top of the preallocated branch
	for (parent = get_parent(prealloc); !is_root(parent) && is_new_branch(parent); parent = get_parent(prealloc))
		prealloc = parent;

	_delete_prealloc(prealloc);

	return NULL;
}

// TODO: temp entry point for testing the unwind function
int prealloc_unwind(struct sptree_root *root, unsigned long addr)
{
	struct sptree_node *target, *parent, **pbranch;
	struct sptree_node *prealloc;
	struct sptree_node *old = NULL;

	if (!address_valid(root, addr))
		return -EINVAL;

	target = search(root, addr);
	if (!target)
		return -ENXIO;

	// nothing to do in this case
	if (is_leaf(target))
		return 0;

	prealloc = _prealloc_unwind(root, target, &old);
	if (!prealloc)
		return -ENOMEM;

	// the unwind function returns the bottom of the preallocated branch
	for (parent = get_parent(prealloc); !is_root(parent) && is_new_branch(parent); parent = get_parent(prealloc))
		prealloc = parent;
	// walk the parent to the top of the preallocated branch

	// parent may contain L/R flags or NULL
	parent = prealloc->parent;
	pbranch = get_pnode(root, parent);

	// connect the preallocated branch
	prealloc_connect(pbranch, prealloc);

	// this will remove the replaced nodes
	prealloc_remove_old(old);

	// TODO: remove once code stable
	validate_avl_balancing(root);

	return 0;
}

/**
 * delete_retrace() - deletes a leaf node from the old/new branch and does retrace
 * @root - root of the tree
 * @leaf - leaf node to be removed (may belong to old/new branch)
 * @stop - parent (NULL if root) where fixing the new branch stops and retracing begins (TODO: is this needed?)
 * @old	 - chain of old nodes (TODO: put this in a context)
 *
 * Leaf may not be root, it must have a parent (this has been checked in the caller).
 * If the leaf is not already in the new branch, it creates an ascending new branch starting at its parent.
 * Then it decouples leaf from the new branch.
 * If the new branch exists, its height may decrease and must be fixed after delete.
 * Then a retrace must be done to propagate the diff in height.
 * Can not return NULL as a valid value.
 */
static struct sptree_node *delete_retrace(struct sptree_root *root, struct sptree_node *leaf, struct sptree_node **old)
{
	struct sptree_node *parent = get_parent(leaf);
	struct sptree_iterator iter;

	pr_info("%s: at "NODE_FMT"\n", __func__, NODE_ARG(leaf));

	/* node was already leaf, no unwinding happened */
	if (!is_new_branch(parent)) {
		parent = prealloc_parent(parent, old);
		if (!parent)
			return ERR_PTR(-ENOMEM);
	}
	/* else, we're already working on the new branch */

	/* clear pointer in parent pointing to the leaf & init parent iterator */
	if (is_left_child(leaf->parent)) {
		iter.state = ITER_LEFT;
		parent->left = NULL;
	}
	else {
		iter.state = ITER_RIGHT;
		parent->right = NULL;
	}
	iter.node = parent;

	/* fix step - running only on the new branch */
	ASSERT(is_new_branch(parent));

	/* nodes along this branch may be heavy towards the leaf or balanced
	 * this also applies to its direct parent, if it's leaf-heavy,
	 * deleting the leaf will propagate the change up the new branch */
	prealloc_change_height(leaf, -1);
	// TODO: this case was included in prealloc_change_height()
	// https://en.wikipedia.org/wiki/AVL_tree#:~:text=The%20retracing%20can%20stop%20if%20the%20balance%20factor%20becomes%20%C2%B11%20(it%20must%20have%20been%200)%20meaning%20that%20the%20height%20of%20that%20subtree%20remains%20unchanged.
	// but it will have to be implemented overall in the fix/retrace algorithm

	/* delete the leaf directly if it's part of the new branch */
	if (is_new_branch(leaf))
		kfree(leaf);

	/* walk up the new branch & look for excessive unbalancing */
	while (true) {
		switch (iter.node->balance) {
		case -2:
			ASSERT(iter.state = ITER_LEFT);
			iter.node = prealloc_rlr(root, iter.node, old);
			break;

		case 2:
			ASSERT(iter.state = ITER_RIGHT);
			iter.node = prealloc_rrl(root, iter.node, old);
			break;
		}

		parent = get_parent(iter.node);

		/* stop & allow the func to return the root of the new branch */
		if (is_root(parent) || !is_new_branch(parent))
			break;

		/* go up the new branch */
		iter.node = parent;
		if (is_left_child(iter.node->parent))
			iter.state = ITER_LEFT;
		else
			iter.state = ITER_RIGHT;
	}

	// TODO: rest of the retracing (parent contains the link point of the new branch)
	// ...

	// TODO: how do we realize if the subtree whose root is deleted loses height ???
	// ...
	// TODO: prealloc_change_height() can account for the overall change of height when the context will be created!!!

	return iter.node;
}

/* delete & retrace in a single function */
/* WARNING: can return NULL as a valid value (check corner case) */
static struct sptree_node *unwind_delete_retrace(struct sptree_root *root, struct sptree_node *node, struct sptree_node **old)
{
	struct sptree_node *prealloc;
	struct sptree_node *leaf;

	// TODO: or mix fixing with retrace and don't care about this parent, ascend as needed
	// (right now, height propagation is done only on the new branch)

	if (is_leaf(node)) {
		node->old = *old;						/* this node will be deleted */
		*old = node;							/* add to chain of old nodes */

		/* if that node is the only node in the tree, the new branch is empty (NULL) */
		if (is_root(node->parent))
			return NULL;

		leaf = node;
	}
	else {
		// bubble to bottom
		prealloc = _prealloc_unwind(root, node, old);
		if (!prealloc)
			return ERR_PTR(-ENOMEM);
		leaf = prealloc;
	}

	/* at this moment the subtree must represent a leaf node (bottom) */
	BUG_ON(!is_leaf(leaf));

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

	prealloc = delete_retrace(root, leaf, old);
	if (IS_ERR(prealloc))
		return prealloc;

	return prealloc;
}

/*
 * TODO: If the subtree whose root is being deleted changes height,
 * this change must propagate above the previous parent - retrace !!!!
 *
 * TODO: When deleting a leaf node, that node itself (the old node) has to be deleted (RCU delete).
 * When deleting a subtree root, it has to bubble down the tree, delete the node on the new path,
 * (the node on the old path also has to be deleted), then retrace can create new nodes on the
 * new path.
 *
 */
int prealloc_delete(struct sptree_root *root, unsigned long addr)
{
	struct sptree_node *target, *parent, **pbranch;
	struct sptree_node *prealloc;
	struct sptree_node *old = NULL;

	if (!address_valid(root, addr))
		return -EINVAL;

	target = search(root, addr);
	if (!target)
		return -ENXIO;

	// parent may contain L/R flags or NULL
	parent = target->parent;

	/* may return NULL as a valid value */
	prealloc = unwind_delete_retrace(root, target, &old);
	if (IS_ERR(prealloc))
		return PTR_ERR(prealloc);

	if (prealloc)
		parent = prealloc->parent;
	pbranch = get_pnode(root, parent);

	// connect the preallocated branch
	prealloc_connect(pbranch, prealloc);

	// this will remove the replaced nodes
	prealloc_remove_old(old);

	// TODO: remove once code stable
	validate_avl_balancing(root);

	return 0;
}
