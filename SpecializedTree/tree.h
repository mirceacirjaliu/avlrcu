
#ifndef _SPECIALIZED_TREE_H_
#define _SPECIALIZED_TREE_H_

#include <linux/types.h>
#include <linux/rcupdate.h>

struct sptree_node {
	unsigned long start;

	struct sptree_node *parent;
	struct sptree_node *left;
	struct sptree_node *right;

	union {
		struct rcu_head rcu;

		struct {
			/* chain of old nodes to be deleted */
			struct sptree_node *old;

			// TODO: in case of all levels balanced, a lot of reverse double rotations will be needed
			// TODO: this will propagate a lot of unbalancing along the branch & this number may increase/decrease
			// TODO: must find a way not to overflow this variable
			int balance : 15;
			int new_branch : 1;
		};
	};
};

struct sptree_root {

	// TODO: sptree_ops pointer goes here

	struct sptree_node *root;
};

extern void validate_avl_balancing(struct sptree_root *root);

// flags set on parent pointer to fast determine on which side of the parent we are
#define RIGHT_CHILD 0
#define LEFT_CHILD 1

#define PARENT_FLAGS 1

static inline bool is_leaf(struct sptree_node *node)
{
	return node->left == NULL && node->right == NULL;
}

static inline bool is_root(struct sptree_node *parent)
{
	return parent == NULL;
}

static inline bool is_left_child(struct sptree_node *parent)
{
	return (unsigned long)parent & LEFT_CHILD;
}

static inline struct sptree_node *make_left(struct sptree_node *parent)
{
	return (struct sptree_node *)((unsigned long)parent | LEFT_CHILD);
}

static inline struct sptree_node *make_right(struct sptree_node *parent)
{
	return (struct sptree_node *)((unsigned long)parent & ~LEFT_CHILD);
}

static inline struct sptree_node *strip_flags(struct sptree_node *parent)
{
	return (struct sptree_node *)((unsigned long)parent & ~PARENT_FLAGS);
}

static inline struct sptree_node *get_parent(struct sptree_node *node)
{
	return strip_flags(node->parent);
}

/* Returns the address of the pointer pointing to current node based on node->parent. */
static inline struct sptree_node **get_pnode(struct sptree_root *root, struct sptree_node *parent)
{
	if (is_root(parent))
		return &root->root;
	else if (is_left_child(parent))
		return &strip_flags(parent)->left;
	else
		return &strip_flags(parent)->right;
}

static inline bool is_new_branch(struct sptree_node *node)
{
	return !!node->new_branch;
}

/* For dumping purposes. */
static inline char node_balancing(const struct sptree_node *node)
{
	switch (node->balance)
	{
	case -1:
		return 'L';
	case 0:
		return 'B';
	case 1:
		return 'R';
	default:
		return '?';
	}
}

#define NODE_FMT "(%lx, %d)"
#define NODE_ARG(__node) (__node)->start, (__node)->balance


/*
 * The state tells the direction the iteration came from.
 * We enter a node from parent (up).
 * After entering a node, we first look left.
 * After handling left, we handle the node (in-order).
 * After handling the node, we look right.
 * After handling right, we return to parent.
 * (Returning to parent can be done from the left/right child.)
 */
enum sptree_iter_state {
	ITER_UP,
	ITER_LEFT,
	ITER_HANDLED,
	ITER_RIGHT,
	ITER_DONE
};

struct sptree_iterator {
	struct sptree_node *node;
	enum sptree_iter_state state;
};

/* In-order iteration (should be immune to tree operations) */
extern void sptree_iter_first_io(struct sptree_root *root, struct sptree_iterator *iter);
extern void sptree_iter_next_io(struct sptree_iterator *iter);

#define sptree_for_each_node_io(_iter, _root)	\
	for (sptree_iter_first_io(_root, _iter); (_iter)->node != NULL; sptree_iter_next_io(_iter))


/* Pre-order iteration (don't know if immune to tree operations) */
extern void sptree_iter_first_po(struct sptree_root *root, struct sptree_iterator *iter);
extern void sptree_iter_next_po(struct sptree_iterator *iter);

#define sptree_for_each_node_po(_iter, _root)	\
	for (sptree_iter_first_po(_root, _iter); (_iter)->node != NULL; sptree_iter_next_po(_iter))


extern int standard_init(struct sptree_root *root);
extern void sptree_free(struct sptree_root *root);

// helper for operations on an address
static inline bool address_valid(struct sptree_root *root, unsigned long addr)
{
	if (addr & ~PAGE_MASK)
		return false;

	return true;
}

extern struct sptree_node *search(struct sptree_root *root, unsigned long addr);
//extern int rol_height_diff(struct sptree_node *root);
//extern int ror_height_diff(struct sptree_node *root);

// these must be protected by a lock
extern int standard_insert(struct sptree_root *root, unsigned long addr);
extern int standard_delete(struct sptree_root *root, unsigned long addr);

extern int prealloc_insert(struct sptree_root *root, unsigned long addr);
extern int prealloc_delete(struct sptree_root *root, unsigned long addr);
extern int prealloc_unwind(struct sptree_root *root, unsigned long addr);

// same for these
extern int sptree_ror(struct sptree_root *root, unsigned long addr);
extern int sptree_rol(struct sptree_root *root, unsigned long addr);
extern int sptree_rrl(struct sptree_root *root, unsigned long addr);
extern int sptree_rlr(struct sptree_root *root, unsigned long addr);

extern struct sptree_node *sptree_search(struct sptree_root *root, unsigned long addr);

#endif // _SPECIALIZED_TREE_H_