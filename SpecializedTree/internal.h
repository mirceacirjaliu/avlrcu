// SPDX-License-Identifier: GPL-2.0
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

/* post-order iterator */
extern struct avlrcu_node *avlrcu_first_po(struct avlrcu_root *root);
extern struct avlrcu_node *avlrcu_next_po(struct avlrcu_node *node);

#define avlrcu_for_each_po(pos, root)	\
	for (pos = avlrcu_first_po(root); pos != NULL; pos = avlrcu_next_po(pos))

#define avlrcu_for_each_entry_po(pos, root, member)					\
	for (pos = avlrcu_entry_safe(avlrcu_first_po(root), typeof(*(pos)), member);	\
	     pos != NULL;								\
	     pos = avlrcu_entry_safe(avlrcu_next_po(&(pos)->member), typeof(*(pos)), member))

/**
 * avlrcu_for_each_po_safe - iterate post-order over a tree safe against removal of nodes
 * @pos:	the struct avlrcu_node * to use as a loop cursor.
 * @n:		another struct avlrcu_node * to use as temporary storage
 * @root:	the root of the tree.
 *
 * Don't dare use this to delete nodes from a live tree.
 * Nodes must be first decoupled & made unreachable.
 */
#define avlrcu_for_each_po_safe(pos, n, root)		\
	for (pos = avlrcu_first_po(root);		\
	     pos && ({ n = avlrcu_next_po(pos); 1; });	\
	     pos = n)

/* post-order iterator on the preallocated branch */
extern struct avlrcu_node *prealloc_first_po(struct avlrcu_node *node);
extern struct avlrcu_node *prealloc_next_po(struct avlrcu_node *node);

#define avlrcu_for_each_prealloc_po(pos, root)	\
	for (pos = prealloc_first_po(root);	\
	     pos;				\
	     pos = prealloc_next_po(pos))

#define avlrcu_for_each_prealloc_po_safe(pos, n, root)		\
	for (pos = prealloc_first_po(root);			\
	     pos && ({ n = prealloc_next_po(pos); 1; });	\
	     pos = n)

/* reverse-in-order iteration on the preallocated branch */
extern struct avlrcu_node *prealloc_first_rin(struct avlrcu_node *node);
extern struct avlrcu_node *prealloc_next_rin(struct avlrcu_node *node);

#define avlrcu_for_each_prealloc_rin(pos, root)	\
	for (pos = prealloc_first_rin(root);	\
	     pos != NULL;			\
	     pos = prealloc_next_rin(pos))	\

/* the preallocated branch/subtree doesn't have a root, just a root node */

#endif /* _AVLRCU_INTERNAL_H_ */
