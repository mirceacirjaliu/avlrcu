
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


static inline bool is_root(struct sptree_node *node)
{
	return node->parent == NULL;
}

static inline bool is_leaf(struct sptree_node *node)
{
	return node->left == NULL && node->right == NULL;
}

static inline bool is_left_child(struct sptree_node *node)
{
	if (is_root(node))
		return false;
	if (node->parent->left == node)
		return true;
	return false;
}

static inline bool is_right_child(struct sptree_node *node)
{
	if (is_root(node))
		return false;
	if (node->parent->right == node)
		return true;
	return false;
}

static inline bool has_children(struct sptree_node *node)
{
	return node->left || node->right;
}

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

extern void sptree_iter_first(struct sptree_root *root, struct sptree_iterator *iter);
extern void sptree_iter_next(struct sptree_iterator *iter);

#define sptree_for_each_node(_iter, _root)	\
	for (sptree_iter_first(_root, _iter); (_iter)->state != ITER_DONE; sptree_iter_next(_iter))


extern int sptree_init(struct sptree_root *root, unsigned long start, size_t length);
extern void sptree_free(struct sptree_root *root);

// these 2 must be protected by a lock
extern int sptree_insert(struct sptree_root *root, unsigned long addr);
extern int sptree_delete(struct sptree_root *root, unsigned long addr);

// same for these
extern int sptree_ror(struct sptree_root *root, unsigned long addr);
extern int sptree_rol(struct sptree_root *root, unsigned long addr);

extern struct sptree_node *sptree_search(struct sptree_root *root, unsigned long addr);

extern int inorder_walk(struct sptree_root *root, int (*nosleep)(struct sptree_node *node));

#endif // _SPECIALIZED_TREE_H_