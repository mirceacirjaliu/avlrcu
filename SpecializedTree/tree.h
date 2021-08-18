
#ifndef _AVLRCU_H_
#define _AVLRCU_H_

#include <linux/types.h>
#include <linux/llist.h>

struct avlrcu_node {
	struct avlrcu_node __rcu *parent;
	struct avlrcu_node __rcu *left;
	struct avlrcu_node __rcu *right;

	union {
		struct rcu_head rcu;

		struct {
			struct llist_node old;		/* chain of old nodes to be deleted */
			long balance : 63;		/* balance of current node */
			unsigned long new_branch : 1;	/* nodes on the preallocated subtree */
		};
	};
};

struct avlrcu_ops {
	struct avlrcu_node *(*alloc)(void);
	void (*free)(struct avlrcu_node *);
	void (*free_rcu)(struct avlrcu_node *);
	int (*cmp)(const struct avlrcu_node *, const struct avlrcu_node *);
	void (*copy)(struct avlrcu_node *, struct avlrcu_node *);
};

struct avlrcu_root {
	struct avlrcu_ops *ops;
	struct avlrcu_node __rcu *root;
};

/**
 * avlrcu_entry - get the struct for this entry
 * @ptr:	the &struct avlrcu_node pointer.
 * @type:	the type of the struct this is embedded in.
 * @member:	the name of the avlrcu_node within the struct.
 */
#define avlrcu_entry(ptr, type, member) \
	container_of(ptr, type, member)

#define avlrcu_entry_safe(ptr, type, member)	\
	({ typeof(ptr) ____ptr = (ptr);		\
	   ____ptr ? avlrcu_entry(____ptr, type, member) : NULL; \
	})

/* in-order iterator */
extern struct avlrcu_node *avlrcu_first(struct avlrcu_root *root);
extern struct avlrcu_node *avlrcu_next(struct avlrcu_node *node);

/**
 * avlrcu_for_each - iterate in-order over nodes in a tree
 * @pos:	the &struct avlrcu_node to use as a loop cursor.
 * @root:	the root of the tree.
 */
#define avlrcu_for_each(pos, root)	\
	for (pos = avlrcu_first(root); pos != NULL; pos = avlrcu_next(pos))

/**
 * avlrcu_for_each_entry - iterate in-order over tree of given type
 * @pos:	the type * to use as a loop cursor.
 * @root:	the root of the tree.
 * @member:	the name of the avlrcu_node within the struct.
 */
#define avlrcu_for_each_entry(pos, root, member)					\
	for (pos = avlrcu_entry_safe(avlrcu_first(root), typeof(*(pos)), member);	\
	     pos != NULL;								\
	     pos = avlrcu_entry_safe(avlrcu_next(&(pos)->member), typeof(*(pos)), member))


/* filters will have the same semantics as memcmp() */
typedef int (*filter)(const struct avlrcu_node *crnt, const void *arg);

extern struct avlrcu_node *avlrcu_first_filter(struct avlrcu_root *root, filter f, const void *arg);
extern struct avlrcu_node *avlrcu_next_filter(struct avlrcu_node *node, filter f, const void *arg);

#define avlrcu_for_each_entry_filter(pos, root, member, filter, arg)					\
	for (pos = avlrcu_entry_safe(avlrcu_first_filter(root, filter, arg), typeof(*(pos)), member);	\
	     pos != NULL;										\
	     pos = avlrcu_entry_safe(avlrcu_next_filter(&(pos)->member, filter, arg), typeof(*(pos)), member))


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
 * @pos:	the &struct avlrcu_node to use as a loop cursor.
 * @n:		another &struct avlrcu_node to use as temporary storage
 * @root:	the root of the tree.
 *
 * Don't dare use this to delete nodes from a live tree.
 * Nodes must be first decoupled & made unreachable.
 */
#define avlrcu_for_each_po_safe(pos, n, root)		\
	for (pos = avlrcu_first_po(root);		\
	     pos && ({ n = avlrcu_next_po(pos); 1; });	\
	     pos = n)

extern void avlrcu_init(struct avlrcu_root *root, struct avlrcu_ops *ops);

/* write-side calls, must be protected by a lock */
extern void avlrcu_free(struct avlrcu_root *root);
extern int avlrcu_insert(struct avlrcu_root *root, struct avlrcu_node *node);
extern struct avlrcu_node *avlrcu_delete(struct avlrcu_root *root, const struct avlrcu_node *match);

/* test functions, also write-side calls, must be protected by a lock */
extern int avlrcu_test_unwind(struct avlrcu_root *root, const struct avlrcu_node *match);
extern int avlrcu_test_ror(struct avlrcu_root *root, const struct avlrcu_node *match);
extern int avlrcu_test_rol(struct avlrcu_root *root, const struct avlrcu_node *match);
extern int avlrcu_test_rrl(struct avlrcu_root *root, const struct avlrcu_node *match);
extern int avlrcu_test_rlr(struct avlrcu_root *root, const struct avlrcu_node *match);

/* read-side calls, must be protected by (S)RCU section */
extern struct avlrcu_node *avlrcu_search(struct avlrcu_root *root, const struct avlrcu_node *match);

#endif /* _AVLRCU_H_ */
