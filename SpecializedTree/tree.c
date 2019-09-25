
#define pr_fmt(fmt)	KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/types.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/rcupdate.h>
#include <linux/compiler.h>

#include "tree.h"

int sptree_init(struct sptree_root *root, unsigned long start, size_t length)
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



void sptree_iter_first(struct sptree_root *root, struct sptree_iterator *iter)
{
	if (unlikely(root->root == NULL)) {
		iter->node = NULL;
		iter->state = ITER_DONE;
		return;
	}

	iter->node = root->root;
	iter->state = ITER_UP;

	sptree_iter_next(iter);
}

void sptree_iter_next(struct sptree_iterator *iter)
{
	struct sptree_node *next;

	// INFO: left/right pointers of a node are overwritten by
	// WRITE_ONCE(*pnode, pointer) so they must be READ_ONCE()

	while (iter->node) {
		switch (iter->state) {
		case ITER_UP:					// comes from parent...
			next = READ_ONCE(iter->node->left);	// goes down left subtree
			if (next)
				iter->node = next;		// state stays the same
			else {
				iter->state = ITER_HANDLED;	// no left subtree, will handle the node
				return;
			}
			break;

		case ITER_HANDLED:				// node has just been handled...
			next = READ_ONCE(iter->node->right);	// goes down right subtree
			if (next) {
				iter->node = next;
				iter->state = ITER_UP;
			} else {
				goto back_to_parent;		// no right subtree, must go up
			}
			break;

		case ITER_RIGHT:				// comes from right subtree
back_to_parent:
			next = READ_ONCE(iter->node->parent);	// may contain L/R flags
			iter->node = strip_flag(next);		// goes up anyway

			if (is_root(next)) {
				iter->state = ITER_DONE;
				return;
			} else if (is_left_child(next)) {
				iter->state = ITER_HANDLED;	// directly handle the node
				return;
			} else {
				iter->state = ITER_RIGHT;	// right subtree has been handled
			}
			break;

		default:
			pr_warn("%s: unhandled iterator state\n", __func__);
			iter->node = NULL;			// cancels iteration
			return;
		}
	}
}

// root = the node that is parent of this subtree, that is replaced with its left child
// proot = pointer to this node (root->root / parent->left / parent->right)
static struct sptree_node *rotate_right(struct sptree_node *root, struct sptree_node **proot)
{
	struct sptree_node *pivot = root->left;
	struct sptree_node *new_root;
	struct sptree_node *new_pivot;

	pr_info("%s: rotate right at "NODE_FMT", pivot at "NODE_FMT"\n",
		__func__, NODE_ARG(root), NODE_ARG(pivot));

	BUG_ON(*proot != root);
	BUG_ON(!root->mapping);
	BUG_ON(!pivot);
	BUG_ON(!pivot->mapping);

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

	// fix balance factors
	if (pivot->balancing == 0) {
		new_root->balancing = 1;
		new_pivot->balancing = -1;
	} else {
		new_pivot->balancing = 0;
		new_root->balancing = 0;
	}

	// free old nodes
	kfree_rcu(root, rcu);
	kfree_rcu(pivot, rcu);

	pr_info("%s: rotated right, new root is "NODE_FMT"\n",
		__func__, NODE_ARG(new_root));

	return new_root;
}

// same as above, changed direction
static struct sptree_node *rotate_left(struct sptree_node *root, struct sptree_node **proot)
{
	struct sptree_node *pivot = root->right;
	struct sptree_node *new_root;
	struct sptree_node *new_pivot;

	pr_info("%s: rotate left at "NODE_FMT", pivot at "NODE_FMT"\n",
		__func__, NODE_ARG(root), NODE_ARG(pivot));

	BUG_ON(*proot != root);
	BUG_ON(!root->mapping);
	BUG_ON(!pivot);
	BUG_ON(!pivot->mapping);

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

	// fix balance factors
	if (pivot->balancing == 0) {
		new_root->balancing = 1;
		new_pivot->balancing = -1;
	} else {
		new_pivot->balancing = 0;
		new_root->balancing = 0;
	}

	// free old nodes
	kfree_rcu(root, rcu);
	kfree_rcu(pivot, rcu);

	pr_info("%s: rotated left, new root is "NODE_FMT"\n",
		__func__, NODE_ARG(new_root));

	return new_root;
}

static struct sptree_node *rotate_right_left(struct sptree_node *root, struct sptree_node **proot)
{
	// root = X
	struct sptree_node *right = root->right;	// Z
	struct sptree_node **pright = &root->right;
	struct sptree_node *left = right->left;		// Y
	struct sptree_node *new_root;
	struct sptree_node *new_left;			// new X
	struct sptree_node *new_right;			// new Z

	pr_info("%s: rotate right-left at "NODE_FMT"\n", __func__, NODE_ARG(root));

	// rotate right on Z
	if (!rotate_right(right, pright))
		return NULL;

	// rotate left on X
	new_root = rotate_left(root, proot);
	if (!new_root)
		return NULL;

	new_left = new_root->left;
	new_right = new_root->right;

	// fix balance factors
	if (left->balancing > 0) {
		new_left->balancing = -1;
		new_right->balancing = 0;
	} else {
		if (left->balancing == 0) {
			new_left->balancing = 0;
			new_right->balancing = 0;
		} else {
			new_left->balancing = 0;
			new_right->balancing = 1;
		}
	}

	new_root->balancing = 0;

	pr_info("%s: rotated right-left, new root is "NODE_FMT"\n",
		__func__, NODE_ARG(new_root));

	return new_root;
}

static struct sptree_node *rotate_left_right(struct sptree_node *root, struct sptree_node **proot)
{
	// root = X
	struct sptree_node *left = root->left;		// Z
	struct sptree_node **pleft = &root->left;
	struct sptree_node *right = left->right;	// Y
	struct sptree_node *new_root;
	struct sptree_node *new_left;			// new Z
	struct sptree_node *new_right;			// new X

	pr_info("%s: rotate left-right at "NODE_FMT"\n", __func__, NODE_ARG(root));

	// rotate left on Z
	if (!rotate_left(left, pleft))
		return NULL;

	// rotate right on X
	new_root = rotate_right(root, proot);
	if (!new_root)
		return NULL;

	new_left = new_root->left;
	new_right = new_root->right;

	// fix balance factors
	if (right->balancing > 0) {
		new_right->balancing = -1;
		new_left->balancing = 0;
	} else {
		if (right->balancing == 0) {
			new_right->balancing = 0;
			new_left->balancing = 0;
		} else {
			new_right->balancing = 0;
			new_left->balancing = 1;
		}
	}

	new_root->balancing = 0;

	pr_info("%s: rotated left-right, new root is "NODE_FMT"\n",
		__func__, NODE_ARG(new_root));

	return new_root;
}

// TODO: the compound rotations are made up of simple rotations, but affect the
// balance factors in different ways
// TODO: separate the rotations from the balance factors fixing

// TODO: if one of the simple rotations fails in a compound rotation, the tree is
// left in an unbalanced state
// TODO: better implement this in a allocate-replace fashion and try to roll back
// the changes to the tree in case of failure


int sptree_ror(struct sptree_root *root, unsigned long addr)
{
	struct sptree_node *target, *parent;
	struct sptree_node **ptarget;
	struct sptree_node *subtree;

	if (addr & ~PAGE_MASK)
		return -EINVAL;
	if (addr < root->start || addr > root->start + root->length - PAGE_SIZE)
		return -EINVAL;

	target = search(root, addr);
	if (!target) {
		pr_err("couldn't find subtree rooted at %lx\n", addr);
		return -ENOENT;
	}

	// validate the node
	if (!target->mapping) {
		pr_err("found node "NODE_FMT", not a mapping\n", NODE_ARG(target));
		return -EINVAL;
	}
	if (!target->left) {
		pr_err("node too low\n");
		return -EINVAL;
	}
	if (!target->left->mapping) {
		pr_err("pivot not a mapping\n");
		return -EINVAL;
	}

	// may contain L/R flags or NULL
	parent = READ_ONCE(target->parent);
	ptarget = get_pnode(root, parent);

	subtree = rotate_right(target, ptarget);
	if (!subtree) {
		pr_err("no mem?\n");
		return -ENOMEM;
	}

	return 0;
}

int sptree_rol(struct sptree_root *root, unsigned long addr)
{
	struct sptree_node *target, *parent;
	struct sptree_node **ptarget;
	struct sptree_node *subtree;

	if (addr & ~PAGE_MASK)
		return -EINVAL;
	if (addr < root->start || addr > root->start + root->length - PAGE_SIZE)
		return -EINVAL;

	target = search(root, addr);
	if (!target) {
		pr_err("couldn't find subtree rooted at %lx\n", addr);
		return -ENOENT;
	}

	if (!target->mapping) {
		pr_err("found node "NODE_FMT", not a mapping\n", NODE_ARG(target));
		return -EINVAL;
	}
	if (!target->right) {
		pr_err("node too low\n");
		return -EINVAL;
	}
	if (!target->right->mapping) {
		pr_err("pivot not a mapping\n");
		return -EINVAL;
	}

	// may contain L/R flags or NULL
	parent = READ_ONCE(target->parent);
	ptarget = get_pnode(root, parent);

	subtree = rotate_left(target, ptarget);
	if (!subtree) {
		pr_err("no mem?\n");
		return -ENOMEM;
	}

	return 0;
}

static int retrace(struct sptree_root *root, struct sptree_node *node)
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
					parent = rotate_left_right(parent, pparent);
				else
					parent = rotate_right(parent, pparent);

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
					parent = rotate_right_left(parent, pparent);
				else
					parent = rotate_left(parent, pparent);

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

// create a subtree centered on addr
// return the subtree or NULL
static struct sptree_node *insert_alloc_subtree(struct sptree_node *target, unsigned long addr)
{
	struct sptree_node *subtree_root = NULL;
	struct sptree_node *subtree_left = NULL;
	struct sptree_node *subtree_rite = NULL;

	pr_info("%s: splitting "NODE_FMT" at %lx\n", __func__, NODE_ARG(target), addr);

	// this will be the mapping
	subtree_root = kzalloc(sizeof(*subtree_root), GFP_ATOMIC);
	if (!subtree_root)
		return NULL;

	subtree_root->start = addr;
	subtree_root->length = PAGE_SIZE;
	subtree_root->mapping = true;

	// first page in range
	if (addr != target->start) {
		subtree_left = kzalloc(sizeof(*subtree_left), GFP_ATOMIC);
		if (!subtree_left)
			goto error;

		subtree_left->start = target->start;
		subtree_left->length = addr - target->start;

		subtree_root->balancing -= 1;
		subtree_root->left = subtree_left;
		subtree_left->parent = make_left(subtree_root);

		pr_info("%s: to the left we have "NODE_FMT"\n", __func__, NODE_ARG(subtree_left));
	}

	// last page in range
	if (addr != target->start + target->length - PAGE_SIZE) {
		subtree_rite = kzalloc(sizeof(*subtree_rite), GFP_ATOMIC);
		if (!subtree_rite)
			goto error;

		subtree_rite->start = addr + PAGE_SIZE;
		subtree_rite->length = target->length - (addr - target->start + PAGE_SIZE);

		subtree_root->balancing += 1;
		subtree_root->right = subtree_rite;
		subtree_rite->parent = make_right(subtree_root);

		pr_info("%s: to the right we have "NODE_FMT"\n", __func__, NODE_ARG(subtree_rite));
	}

	return subtree_root;

error:
	if (subtree_root)
		kfree(subtree_root);
	if (subtree_left)
		kfree(subtree_left);
	if (subtree_rite)
		kfree(subtree_rite);

	return NULL;
}
// TODO: should I make sure the subtree is all linked up before returning ?
// TODO: a barrier or something ??

// replaces a leaf node with a subtree, increasing the local depth with 1
int sptree_insert(struct sptree_root *root, unsigned long addr)
{
	struct sptree_node *target, *parent;
	struct sptree_node **ptarget;
	struct sptree_node *subtree;

	if (addr & ~PAGE_MASK)
		return -EINVAL;
	if (addr < root->start || addr > root->start + root->length - PAGE_SIZE)
		return -EINVAL;

	target = search(root, addr);
	BUG_ON(!target);

	if (target->mapping)
		return -EALREADY;

	// a mapping can also be a leaf if it replaced a segment of same size
	BUG_ON(!is_leaf(target));
	// but a segment can't be a node

	// alloc & insert subtree
	subtree = insert_alloc_subtree(target, addr);
	if (!subtree)
		return -ENOMEM;

	// may contain L/R flags or NULL
	parent = READ_ONCE(target->parent);
	ptarget = get_pnode(root, parent);

	// reverse, then direct link
	WRITE_ONCE(subtree->parent, parent);
	WRITE_ONCE(*ptarget, subtree);

	// rebalance tree
	retrace(root, subtree);

	// finally free the replaced node
	kfree_rcu(target, rcu);

	return 0;
}

int sptree_delete(struct sptree_root *root, unsigned long addr)
{




	return 0;
}
