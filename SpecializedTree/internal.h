
#ifndef _SPTREE_INTERNAL_H_
#define _SPTREE_INTERNAL_H_

#include "tree.h"

#ifdef SPTREE_DEBUG
#define ASSERT(_expr) BUG_ON(!(_expr))
extern bool validate_avl_balancing(struct sptree_root *root);
#else /* SPTREE_DEBUG */
#define ASSERT(_expr)
static inline bool validate_avl_balancing(struct sptree_root *root)
{
	return true;
}
#endif /* SPTREE_DEBUG */

/* context for insert/delete operations */
struct sptree_ctxt {
	struct sptree_root *root;
	struct llist_head old;
	struct sptree_node *removed;
	int diff;
};

// flags set on parent pointer to fast determine on which side of the parent we are
#define RIGHT_CHILD 0
#define LEFT_CHILD 1
#define PARENT_FLAGS 1

static inline bool is_avl(struct sptree_node *node)
{
	return node->balance >= -1 && node->balance <= 1;
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

static inline struct sptree_node *strip_flags(struct sptree_node *parent)
{
	return (struct sptree_node *)((unsigned long)parent & ~PARENT_FLAGS);
}

static inline struct sptree_node *make_left(struct sptree_node *parent)
{
	return (struct sptree_node *)((unsigned long)parent | LEFT_CHILD);
}

static inline struct sptree_node *make_right(struct sptree_node *parent)
{
	return (struct sptree_node *)((unsigned long)parent & ~LEFT_CHILD);
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

#define NODE_FMT "(%lx, %ld)"
#define NODE_ARG(_node) (long)(_node), (long)(_node)->balance

extern struct sptree_node *prealloc_replace(struct sptree_ctxt *ctxt, struct sptree_node *target);
extern struct sptree_node *prealloc_parent(struct sptree_ctxt *ctxt, struct sptree_node *child);
extern struct sptree_node *prealloc_child(struct sptree_ctxt *ctxt, struct sptree_node *parent, int which);

extern void prealloc_propagate_change(struct sptree_ctxt *ctxt, struct sptree_node *subtree, int diff);
extern struct sptree_node *prealloc_rol(struct sptree_ctxt *ctxt, struct sptree_node *target);
extern struct sptree_node *prealloc_ror(struct sptree_ctxt *ctxt, struct sptree_node *target);
extern struct sptree_node *prealloc_rrl(struct sptree_ctxt *ctxt, struct sptree_node *target);
extern struct sptree_node *prealloc_rlr(struct sptree_ctxt *ctxt, struct sptree_node *target);

extern void sptree_ctxt_init(struct sptree_ctxt *ctxt, struct sptree_root *root);
extern struct sptree_node *prealloc_unwind(struct sptree_ctxt *ctxt, struct sptree_node *target);
extern struct sptree_node *prealloc_top(struct sptree_ctxt *ctxt, struct sptree_node *target);

void prealloc_connect(struct sptree_root *root, struct sptree_node *branch);
extern void prealloc_remove_old(struct sptree_ctxt *ctxt);
extern void _delete_prealloc(struct sptree_ctxt *ctxt, struct sptree_node *prealloc);

#endif /* _SPTREE_INTERNAL_H_ */
