
#define pr_fmt(fmt)	KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/rcupdate.h>
#include <linux/compiler.h>

#include "tree.h"


/* return new root to be set as top of branch */
static struct sptree_node *rotate_right_prealloc(struct sptree_node *root)
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
	if (pivot->balancing == 0) {
		new_root->balancing = 1;
		new_pivot->balancing = -1;
	}
	else {
		new_pivot->balancing = 0;
		new_root->balancing = 0;
	}

	pr_info("%s: new root is "NODE_FMT"\n", __func__, NODE_ARG(new_root));

	return new_root;
}

/* return new root to be set as top of branch */
static struct sptree_node *rotate_left_right_prealloc(struct sptree_node *root)
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
	if (right->balancing > 0) {
		new_left->balancing = -1;
		new_right->balancing = 0;
	}
	else  if (right->balancing == 0) {
		new_left->balancing = 0;
		new_right->balancing = 0;
	}
	else {
		new_left->balancing = 0;
		new_right->balancing = 1;
	}

	new_root->balancing = 0;

	pr_info("%s: new root is "NODE_FMT"\n", __func__, NODE_ARG(new_root));

	return new_root;
}

/* return new root to be set as top of branch */
static struct sptree_node *rotate_left_prealloc(struct sptree_node *root)
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
	if (pivot->balancing == 0) {
		new_root->balancing = 1;
		new_pivot->balancing = -1;
	}
	else {
		new_pivot->balancing = 0;
		new_root->balancing = 0;
	}

	pr_info("%s: new root is "NODE_FMT"\n",
		__func__, NODE_ARG(new_root));

	return new_root;
}

/* return new root to be set as top of branch */
static struct sptree_node *rotate_right_left_prealloc(struct sptree_node *root)
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
	if (left->balancing > 0) {
		new_left->balancing = -1;
		new_right->balancing = 0;
	}
	else if (left->balancing == 0) {
		new_left->balancing = 0;
		new_right->balancing = 0;
	}
	else {
		new_left->balancing = 0;
		new_right->balancing = 1;
	}

	new_root->balancing = 0;

	pr_info("%s: new root is "NODE_FMT"\n", __func__, NODE_ARG(new_root));

	return new_root;
}

static struct sptree_node *prealloc_retrace(struct sptree_root *root, struct sptree_node *node, struct sptree_node **old)
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

		parent->old = *old;						/* this node will be replaced */
		*old = parent;							/* add to chain of old nodes */

		if (is_left_child(node->parent)) {
			pr_info("%s: node "NODE_FMT" is left child of "NODE_FMT"\n", __func__,
				NODE_ARG(node), NODE_ARG(parent));

			/* link top of branch to new parent */
			new_parent->left = branch;
			branch->parent = make_left(new_parent);

			// parent is left-heavy
			if (parent->balancing < 0) {
				// node is right-heavy
				if (branch->balancing > 0)
					branch = rotate_left_right_prealloc(new_parent);
				else
					branch = rotate_right_prealloc(new_parent);
				break;
			}
			// parent is right-heavy
			else if (parent->balancing > 0) {
				new_parent->balancing = 0;			/* parent becomes balanced */
				branch = new_parent;				/* push parent to branch */

				pr_info("%s: parent "NODE_FMT" becomes balanced, stop\n",
					__func__, NODE_ARG(parent));
				break;
			}
			// parent is balanced
			else {
				new_parent->balancing = -1;			/* parent becomes left-heavy */
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
			if (parent->balancing > 0) {
				// node is left-heavy
				if (branch->balancing < 0)
					branch = rotate_right_left_prealloc(new_parent);
				else
					branch = rotate_left_prealloc(new_parent);
				break;
			}
			// parent is left-heavy
			else if (parent->balancing < 0) {
				new_parent->balancing = 0;			/* parent becomes balanced */
				branch = new_parent;				/* push parent to branch */

				pr_info("%s: parent "NODE_FMT" becomes balanced, stop\n",
					__func__, NODE_ARG(parent));
				break;
			}
			// parent is balanced
			else {
				new_parent->balancing = 1;			/* parent becomes right-heavy */
				branch = new_parent;				/* push parent to branch */

				pr_info("%s: parent "NODE_FMT" becomes right-heavy, continue\n",
					__func__, NODE_ARG(parent));
			}
		}
	}

	return branch;

error:
	/* deletion will descend along the branch */
	while (branch) {
		node = branch;

		if (node->left && node->left->old)
			branch = node->left;
		else if (node->right && node->right->old)
			branch = node->right;
		else
			branch = NULL;

		kfree(node);
	}
	// TODO: in-order traversal on the branch & delete
	// TODO: use a general-purpose function for that (that receives callbacks)
	// TODO: the delete() code will create a tree segment that needs in-order traversal

	return ERR_PTR(-ENOMEM);
};

/**
 * prealloc_connect() - insert the new branch in a tree
 * @root:	The root of the tree
 * @branch:	The root of the new branch/subtree
 *
 * Used on insertion/deletion. The new branch can be:
 * - a single node or a branch for insertion
 * - NULL when deleting the only node in a tree (empty branch)
 * - a subtree for deletion
 */
static void prealloc_connect(struct sptree_root *root, struct sptree_node *branch)
{
	struct sptree_node **pbranch;
	struct sptree_iterator io;
	struct sptree_node *next;

	// parent may contain L/R flags or NULL
	pbranch = get_pnode(root, branch == NULL ? NULL : branch->parent);

	// first link root (iterators entering here must find a way out)
	WRITE_ONCE(*pbranch, branch);

	// in-order traversal of links
	io.node = branch;
	io.state = ITER_UP;

	while (io.node) {
		switch (io.state)
		{
		case ITER_UP:
			next = io.node->left;			// move to left node
			if (next) {
				if (is_new_branch(next)) {
					io.node = next;
					break;			// switch
				}
				else {
					WRITE_ONCE(next->parent, make_left(io.node));
					io.state = ITER_LEFT;
					// fallback
				}
			}

		case ITER_LEFT:
			next = io.node->right;			// move to right node
			if (next) {
				if (is_new_branch(next)) {
					io.node = next;
					io.state = ITER_UP;
					break;			// switch
				}
				else {
					WRITE_ONCE(next->parent, make_right(io.node));
					io.state = ITER_RIGHT;
					// fallback
				}
			}

		case ITER_RIGHT:
			next = strip_flags(io.node->parent);	// move to parent
			if (next && is_new_branch(next)) {
				if (is_left_child(io.node->parent))
					io.state = ITER_LEFT;
			}
			else {
				next = NULL;			// out of new branch
				io.state = ITER_DONE;		// done!
			}

			// delete old node before exiting this one
			//kfree_rcu(io.node->old, rcu);
			//io.node->old = NULL;
			io.node->new_branch = 0;

			io.node = next;				// finally move to parent
			break;					// switch

		default:
			pr_err("%s: unhandled iterator state\n", __func__);
			BUG();
			io.node = NULL;				// cancels iteration
			io.state = ITER_DONE;
			break;
		}
	}
}

static void remove_old_nodes(struct sptree_node *old)
{
	struct sptree_node *temp;

	while (old) {
		temp = old->old;
		kfree_rcu(old, rcu);
		old = temp;
	}
}

int prealloc_insert(struct sptree_root *root, unsigned long addr)
{
	struct sptree_node *crnt, *parent;
	struct sptree_node *node;
	struct sptree_node *prealloc;
	struct sptree_node *old = NULL;

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

	/* alloc the node & fill it - this is the first node in the preallocated branch */
	node = kzalloc(sizeof(*node), GFP_ATOMIC);
	if (!node)
		return -ENOMEM;

	node->start = addr;
	node->parent = parent;		/* only link one way */
	node->new_branch = 1;

	/* retrace generates the preallocated branch */
	prealloc = prealloc_retrace(root, node, &old);
	if (IS_ERR_OR_NULL(prealloc))
		return -ENOMEM;

	// connect the preallocated branch
	prealloc_connect(root, prealloc);

	// this will remove the replaced nodes
	remove_old_nodes(old);

	// TODO: remove once code stable
	validate_avl_balancing(root);

	return 0;
}

/* delete & retrace in a single function */
static struct sptree_node *prealloc_delete_retrace(struct sptree_root *root, struct sptree_node *node, struct sptree_node **old)
{

	return NULL;
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
 * TODO: Connecting the new branch in the tree deletes nodes not necessarily in the same order
 * as connections are made. Temporary holes may appear ?!
 * TODO: Old nodes should be chained and deleted (RCU) after insertion is done. At this point
 * no other walker can enter an old node, but old nodes can still be used by walkers.
 */
int prealloc_delete(struct sptree_root *root, unsigned long addr)
{
	struct sptree_node *target;
	struct sptree_node *prealloc;
	struct sptree_node *old = NULL;

	if (!address_valid(root, addr))
		return -EINVAL;

	target = search(root, addr);
	if (!target)
		return -ENXIO;

	/* may return NULL as a valid value */
	prealloc = prealloc_delete_retrace(root, target, &old);
	if (IS_ERR(prealloc))
		return PTR_ERR(prealloc);

	// connect the preallocated branch
	prealloc_connect(root, prealloc);

	// this will remove the replaced nodes
	remove_old_nodes(old);

	// TODO: remove once code stable
	validate_avl_balancing(root);

	return 0;
}
