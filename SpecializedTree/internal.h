
#ifndef _AVLRCU_INTERNAL_H_
#define _AVLRCU_INTERNAL_H_

#include "tree.h"

#ifdef AVLRCU_DEBUG
#define ASSERT(_expr) BUG_ON(!(_expr))
extern bool validate_avl_balancing(struct avlrcu_root *root);
#else /* AVLRCU_DEBUG */
#define ASSERT(_expr)
static inline bool validate_avl_balancing(struct avlrcu_root *root)
{
	return true;
}
#endif /* AVLRCU_DEBUG */

/* context for insert/delete operations */
struct avlrcu_ctxt {
	struct avlrcu_root *root;
	struct llist_head old;
	struct avlrcu_node *removed;
	int diff;
};

// flags set on parent pointer to fast determine on which side of the parent we are
#define RIGHT_CHILD 0
#define LEFT_CHILD 1
#define PARENT_FLAGS 1

static inline bool is_avl(struct avlrcu_node *node)
{
	return node->balance >= -1 && node->balance <= 1;
}

static inline bool is_leaf(struct avlrcu_node *node)
{
	return node->left == NULL && node->right == NULL;
}

static inline bool is_root(struct avlrcu_node *parent)
{
	return parent == NULL;
}

static inline bool is_left_child(struct avlrcu_node *parent)
{
	return (unsigned long)parent & LEFT_CHILD;
}

static inline struct avlrcu_node *strip_flags(struct avlrcu_node *parent)
{
	return (struct avlrcu_node *)((unsigned long)parent & ~PARENT_FLAGS);
}

static inline struct avlrcu_node *make_left(struct avlrcu_node *parent)
{
	return (struct avlrcu_node *)((unsigned long)parent | LEFT_CHILD);
}

static inline struct avlrcu_node *make_right(struct avlrcu_node *parent)
{
	return (struct avlrcu_node *)((unsigned long)parent & ~LEFT_CHILD);
}

static inline struct avlrcu_node *get_parent(struct avlrcu_node *node)
{
	return strip_flags(node->parent);
}

/* Returns the address of the pointer pointing to current node based on node->parent. */
static inline struct avlrcu_node **get_pnode(struct avlrcu_root *root, struct avlrcu_node *parent)
{
	if (is_root(parent))
		return &root->root;
	else if (is_left_child(parent))
		return &strip_flags(parent)->left;
	else
		return &strip_flags(parent)->right;
}

static inline bool is_new_branch(struct avlrcu_node *node)
{
	return !!node->new_branch;
}

#define NODE_FMT "(%lx, %ld)"
#define NODE_ARG(_node) (long)(_node), (long)(_node)->balance

extern struct avlrcu_node *prealloc_replace(struct avlrcu_ctxt *ctxt, struct avlrcu_node *target);
extern struct avlrcu_node *prealloc_parent(struct avlrcu_ctxt *ctxt, struct avlrcu_node *child);
extern struct avlrcu_node *prealloc_child(struct avlrcu_ctxt *ctxt, struct avlrcu_node *parent, int which);

extern void prealloc_propagate_change(struct avlrcu_ctxt *ctxt, struct avlrcu_node *subtree, int diff);
extern struct avlrcu_node *prealloc_rol(struct avlrcu_ctxt *ctxt, struct avlrcu_node *target);
extern struct avlrcu_node *prealloc_ror(struct avlrcu_ctxt *ctxt, struct avlrcu_node *target);
extern struct avlrcu_node *prealloc_rrl(struct avlrcu_ctxt *ctxt, struct avlrcu_node *target);
extern struct avlrcu_node *prealloc_rlr(struct avlrcu_ctxt *ctxt, struct avlrcu_node *target);

extern void avlrcu_ctxt_init(struct avlrcu_ctxt *ctxt, struct avlrcu_root *root);
extern struct avlrcu_node *prealloc_unwind(struct avlrcu_ctxt *ctxt, struct avlrcu_node *target);
extern struct avlrcu_node *prealloc_top(struct avlrcu_ctxt *ctxt, struct avlrcu_node *target);

void prealloc_connect(struct avlrcu_root *root, struct avlrcu_node *branch);
extern void prealloc_remove_old(struct avlrcu_ctxt *ctxt);
extern void _delete_prealloc(struct avlrcu_ctxt *ctxt, struct avlrcu_node *prealloc);

#endif /* _AVLRCU_INTERNAL_H_ */
