
#define pr_fmt(fmt)	KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/types.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/rcupdate.h>

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

	pr_info("created range: start: %lx, length: %lx\n", start, length);

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


// recursive in-order walk function
static int inorder(struct sptree_node *node, int(*cb)(struct sptree_node *node))
{
	int result;

	if (node->left) {
		result = inorder(node->left, cb);
		if (result)
			return result;
	}

	result = cb(node);
	if (result)
		return result;

	if (node->right) {
		result = inorder(node->right, cb);
		if (result)
			return result;
	}

	return 0;
}

int inorder_walk(struct sptree_root *root, int(*nosleep)(struct sptree_node *node))
{
	int result;

	rcu_read_lock();
	result = inorder(root->root, nosleep);
	rcu_read_unlock();

	return result;
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
		pr_info("%s: found node staring at %lx\n", __func__, crnt->start);

	return crnt;
}

extern struct sptree_node *sptree_search(struct sptree_root *root, unsigned long addr)
{
	struct sptree_node *result;

	rcu_read_lock();
	result = search(root, addr);
	rcu_read_unlock();

	return result;
}



extern void sptree_iter_first(struct sptree_root *root, struct sptree_iterator *iter)
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

// TODO: functia asta s-ar putea sa fie vulnerabila la schimbari in tree daca
// se citesc de mai multe ori aceiasi pointeri (si se schimba intre citiri)
// nu mai e compatibila RCU
extern void sptree_iter_next(struct sptree_iterator *iter)
{
	while (true) {

		switch (iter->state) {
		case ITER_UP:
			// comes from parent
			if (iter->node->left) {
				// goes down left subtree
				iter->node = iter->node->left;
				iter->state = ITER_UP;
				break;
			} else {
				// no left subtree, will handle the node
				iter->state = ITER_HANDLED;
				return;
			}

		case ITER_HANDLED:
			// node has just been handled
			if (iter->node->right) {
				// goes down right subtree
				iter->node = iter->node->right;
				iter->state = ITER_UP;
				break;
			} else {
				// no right subtree, must go up
				if (is_root(iter->node)) {
					iter->node = NULL;
					iter->state = ITER_DONE;
					return;
				} else if (is_left_child(iter->node)) {
					iter->node = iter->node->parent;
					iter->state = ITER_LEFT;
					break;
				} else {
					iter->node = iter->node->parent;
					iter->state = ITER_RIGHT;
					break;
				}
			}

		case ITER_LEFT:
			// comes from the left subtree
			iter->state = ITER_HANDLED;
			return;

		case ITER_RIGHT:
			// comes from the right subtree, must go up
			if (is_root(iter->node)) {
				iter->node = NULL;
				iter->state = ITER_DONE;
				return;
			}
			else if (is_left_child(iter->node)) {
				iter->node = iter->node->parent;
				iter->state = ITER_LEFT;
				break;
			}
			else {
				iter->node = iter->node->parent;
				iter->state = ITER_RIGHT;
				break;
			}
			// TODO: same sequence as above

		case ITER_DONE:
		default:
			return;
		}
	}
}
// TODO: ce se intampla cu parcurgerea arborelui daca suntem pe un nod care tocmai isi
// schimba parintele dreapta-stanga sau invers (la o rotatie) ??!!



// root = the node that is parent of this subtree, that is replaced with its left child
// proot = pointer to this node (root->root / parent->left / parent->right)
static struct sptree_node *rotate_right(struct sptree_node *root, struct sptree_node **proot)
{
	struct sptree_node *pivot = root->left;
	struct sptree_node *new_pivot;
	struct sptree_node *new_root;

	pr_info("%s: rotate right at %lx, pivot at %lx\n", __func__, root->start, pivot->start);

	BUG_ON(*proot != root);
	BUG_ON(!root->mapping);
	BUG_ON(!pivot);
	BUG_ON(!pivot->mapping);

	// alloc new nodes
	new_pivot = kzalloc(sizeof(*new_pivot), GFP_ATOMIC);
	if (!new_pivot)
		return NULL;

	new_root = kzalloc(sizeof(*new_root), GFP_ATOMIC);
	if (!new_root) {
		kfree(new_pivot);
		return NULL;
	}

	// copy to new nodes
	memcpy(new_pivot, pivot, sizeof(struct sptree_node));
	memcpy(new_root, root, sizeof(struct sptree_node));

	// link direct pointers
	new_pivot->left = pivot->left;
	new_pivot->right = new_root;
	new_pivot->parent = root->parent;	// this will point to parent of subtree

	new_root->parent = new_pivot;
	new_root->left = pivot->right;
	new_root->right = root->right;

	// link root (of subtree)
	*proot = new_pivot;

	// link parent pointers
	if (pivot->left)
		pivot->left->parent = new_pivot;
	if (pivot->right)
		pivot->right->parent = new_root;
	if (root->right)
		root->right->parent = new_root;

	// fix balance factors
	if (pivot->balancing == 0) {
		new_pivot->balancing = 1;
		new_root->balancing = -1;
	}
	else {
		new_root->balancing = 0;
		new_pivot->balancing = 0;
	}

	// free old nodes
	kfree_rcu(root, rcu);
	kfree_rcu(pivot, rcu);

	pr_info("%s: rotated right, new root is %lx\n", __func__, new_pivot->start);

	return new_pivot;
}

// same as above, changed direction
static struct sptree_node *rotate_left(struct sptree_node *root, struct sptree_node **proot)
{
	struct sptree_node *pivot = root->right;
	struct sptree_node *new_pivot;
	struct sptree_node *new_root;

	pr_info("%s: rotate left at %lx, pivot at %lx\n", __func__, root->start, pivot->start);

	BUG_ON(*proot != root);
	BUG_ON(!root->mapping);
	BUG_ON(!pivot);
	BUG_ON(!pivot->mapping);

	// alloc new nodes
	new_pivot = kzalloc(sizeof(*new_pivot), GFP_ATOMIC);
	if (!new_pivot)
		return NULL;

	new_root = kzalloc(sizeof(*new_root), GFP_ATOMIC);
	if (!new_root) {
		kfree(new_pivot);
		return NULL;
	}

	// copy to new nodes
	memcpy(new_pivot, pivot, sizeof(struct sptree_node));
	memcpy(new_root, root, sizeof(struct sptree_node));

	// link direct pointers
	new_pivot->left = new_root;
	new_pivot->right = pivot->right;
	new_pivot->parent = root->parent;	// this will point to parent of subtree

	new_root->parent = new_pivot;
	new_root->left = root->left;
	new_root->right = pivot->left;

	// link root (of subtree)
	*proot = new_pivot;

	// link parent pointers
	if (root->left)
		root->left->parent = new_root;
	if (pivot->left)
		pivot->left->parent = new_root;
	if (pivot->right)
		pivot->right->parent = new_pivot;

	// fix balance factors
	if (pivot->balancing == 0) {
		new_pivot->balancing = 1;
		new_root->balancing = -1;
	}
	else {
		new_root->balancing = 0;
		new_pivot->balancing = 0;
	}

	// free old nodes
	kfree_rcu(root, rcu);
	kfree_rcu(pivot, rcu);

	pr_info("%s: rotated left, new root is %lx\n", __func__, new_pivot->start);

	return new_pivot;
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

	pr_info("%s: rotate right-left at %lx\n", __func__, root->start);

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
	}
	else {
		if (left->balancing == 0) {
			new_left->balancing = 0;
			new_right->balancing = 0;
		}
		else
		{
			new_left->balancing = 0;
			new_right->balancing = 1;
		}
	}

	new_root->balancing = 0;

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

	pr_info("%s: rotate left-right at %lx\n", __func__, root->start);

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
	}
	else
	{
		if (right->balancing == 0) {
			new_right->balancing = 0;
			new_left->balancing = 0;
		}
		else
		{
			new_right->balancing = 0;
			new_left->balancing = 1;
		}
	}

	new_root->balancing = 0;

	return new_root;
}

// TODO: the compound rotations are made up of simple rotations, but affect the
// balance factors in different ways
// TODO: separate the rotations from the balance factors fixing

// TODO: if one of the simple rotations fails in a compound rotation, the tree is
// left in an unbalanced state
// TODO: better implement this in a allocate-replace fashion and try to roll back
// the changes to the tree in case of failure

struct sptree_node **get_pnode(struct sptree_root *root, struct sptree_node *node)
{
	if (is_root(node))
		return &root->root;

	if (is_left_child(node))
		return &node->parent->left;
	else
		return &node->parent->right;
}

int sptree_ror(struct sptree_root *root, unsigned long addr)
{
	struct sptree_node *target, **ptarget;
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
		pr_err("found node (%lx, %lx), not a mapping\n", target->start, target->length);
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

	ptarget = get_pnode(root, target);
	subtree = rotate_right(target, ptarget);
	if (!subtree) {
		pr_err("no mem?\n");
		return -ENOMEM;
	}

	return 0;
}

int sptree_rol(struct sptree_root *root, unsigned long addr)
{
	struct sptree_node *target, **ptarget;
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
		pr_err("found node (%lx, %lx), not a mapping\n", target->start, target->length);
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

	ptarget = get_pnode(root, target);
	subtree = rotate_left(target, ptarget);
	if (!subtree) {
		pr_err("no mem?\n");
		return -ENOMEM;
	}

	return 0;
}

static int retrace(struct sptree_root *root, struct sptree_node *node)
{
	struct sptree_node *parent;
	struct sptree_node **pparent;
	struct sptree_node *result;

	pr_info("%s: starting at %lx\n", __func__, node->start);

	for (parent = node->parent; parent != NULL; node = parent, parent = node->parent) {
		pr_info("%s: loop on parent at %lx\n", __func__, parent->start);

		if (is_left_child(node)) {
			// parent is left-heavy
			if (parent->balancing < 0) {
				pparent = get_pnode(root, parent);
				if (node->balancing > 0)
					result = rotate_left_right(parent, pparent);
				else
					result = rotate_right(parent, pparent);

				if (!result)
					return -ENOMEM;
			}
			// parent is right-heavy
			else if (parent->balancing > 0) {
				parent->balancing = 0;
				break;
			}
			// parent is balanced
			else {
				parent->balancing = -1;
			}
		}
		// right child
		else {
			// parent is right heavy
			if (parent->balancing > 0) {
				pparent = get_pnode(root, parent);
				if (node->balancing < 0)
					result = rotate_right_left(parent, pparent);
				else
					result = rotate_left(parent, pparent);

				if (!result)
					return -ENOMEM;
			}
			// parent is left-heavy
			else if (parent->balancing < 0) {
				parent->balancing = 0;
				break;
			}
			// parent is balanced
			else {
				parent->balancing = +1;
			}
		}
	}

	// TODO: also analyze return value of rotations and return errors
	return 0;
}

// create a subtree centered on addr
// return the subtree or NULL
static struct sptree_node *insert_alloc_subtree(struct sptree_node *target, unsigned long addr)
{
	struct sptree_node *subtree_root = NULL;
	struct sptree_node *subtree_left = NULL;
	struct sptree_node *subtree_rite = NULL;

	pr_info("%s: splitting %lx-%lx at %lx\n", __func__,
		target->start, target->start + target->length, addr);

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

		subtree_root->left = subtree_left;
		subtree_left->parent = subtree_root;

		subtree_root->balancing -= 1;
	}

	// last page in range
	if (addr != target->start + target->length - PAGE_SIZE) {
		subtree_rite = kzalloc(sizeof(*subtree_rite), GFP_ATOMIC);
		if (!subtree_rite)
			goto error;

		subtree_rite->start = addr + PAGE_SIZE;
		subtree_rite->length = target->length - (addr - target->start + PAGE_SIZE);

		subtree_root->right = subtree_rite;
		subtree_rite->parent = subtree_root;

		subtree_root->balancing += 1;
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

// replaces a leaf node with a subtree, increasing the local depth with 1
int sptree_insert(struct sptree_root *root, unsigned long addr)
{
	struct sptree_node *target, **ptarget;
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

	// reverse, then direct link
	subtree->parent = target->parent;
	ptarget = get_pnode(root, target);
	*ptarget = subtree;

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
