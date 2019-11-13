
#ifndef _SPECIALIZED_TREE_H_
#define _SPECIALIZED_TREE_H_

#include <linux/types.h>
#include <linux/rcupdate.h>

struct sptree_node {
	unsigned long start;
	size_t length;
	bool mapping;		// TODO: just for assertions, remove later
	int balancing;

	struct sptree_node *parent;
	struct sptree_node *left;
	struct sptree_node *right;

	struct rcu_head rcu;
};

struct sptree_root {
	unsigned long start;	// overall information about range
	size_t length;

	struct sptree_node *root;
};

// flags set on parent pointer to fast determine on which side of the parent we are
#define RIGHT_CHILD 0
#define LEFT_CHILD 1

#define PARENT_FLAGS 1

static inline bool is_mapping(struct sptree_node *node)
{
	return node->mapping;
}

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
	return (struct sptree_node *) ((unsigned long)parent | LEFT_CHILD);
}

static inline struct sptree_node *make_right(struct sptree_node *parent)
{
	return (struct sptree_node *) ((unsigned long)parent & ~LEFT_CHILD);
}

static inline struct sptree_node *strip_flag(struct sptree_node *parent)
{
	return (struct sptree_node *) ((unsigned long)parent & ~PARENT_FLAGS);
}

/* Returns the address of the pointer pointing to current node based on node->parent. */
static inline struct sptree_node **get_pnode(struct sptree_root *root, struct sptree_node *parent)
{
	if (is_root(parent))
		return &root->root;
	else if (is_left_child(parent))
		return &strip_flag(parent)->left;
	else
		return &strip_flag(parent)->right;
}

/* For dumping purposes. */
static inline char node_balancing(const struct sptree_node *node)
{
	switch (node->balancing)
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

#define NODE_FMT "(%lx-%lx,%d)"
#define NODE_ARG(__node) (__node)->start, (__node)->start + (__node)->length, (__node)->balancing



// TODO: starea ar trebui sa zica din ce directie a venit
// TODO: cand intram intr-un nod (de sus), mai intai cautam in stanga
// TODO: cand ne-am intors din stanga, lucram pe nod
// TODO: dupa nod, cautam in dreapta
// TODO: dupa dreapta, ne intoarcem la parinte
// TODO: (intoarcerea la parinte se poate face pe ramura stanga/dreapta)
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


extern int standard_init(struct sptree_root *root, unsigned long start, size_t length);
extern int interval_init(struct sptree_root *root, unsigned long start, size_t length);
extern void sptree_free(struct sptree_root *root);

// helper for operations on an address
static inline bool address_valid(struct sptree_root *root, unsigned long addr)
{
	if (addr & ~PAGE_MASK)
		return false;
	if (addr < root->start || addr > root->start + root->length - PAGE_SIZE)
		return false;

	return true;
}

// these 2 must be protected by a lock
extern int standard_insert(struct sptree_root *root, unsigned long addr);
extern int interval_insert(struct sptree_root *root, unsigned long addr);
extern int sptree_delete(struct sptree_root *root, unsigned long addr);

// same for these
extern int sptree_ror(struct sptree_root *root, unsigned long addr);
extern int sptree_rol(struct sptree_root *root, unsigned long addr);
extern int sptree_rrl(struct sptree_root *root, unsigned long addr);
extern int sptree_rlr(struct sptree_root *root, unsigned long addr);

extern struct sptree_node *sptree_search(struct sptree_root *root, unsigned long addr);

#endif // _SPECIALIZED_TREE_H_