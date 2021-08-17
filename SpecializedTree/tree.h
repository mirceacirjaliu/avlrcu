
#ifndef _SPECIALIZED_TREE_H_
#define _SPECIALIZED_TREE_H_

#include <linux/types.h>
#include <linux/llist.h>

struct sptree_node {
	struct sptree_node __rcu *parent;
	struct sptree_node __rcu *left;
	struct sptree_node __rcu *right;

	union {
		struct rcu_head rcu;

		struct {
			struct llist_node old;		/* chain of old nodes to be deleted */
			long balance : 63;		/* balance of current node */
			unsigned long new_branch : 1;	/* nodes on the preallocated subtree */
		};
	};
};

struct sptree_ops {
	struct sptree_node *(*alloc)(void);
	void (*free)(struct sptree_node *);
	void (*free_rcu)(struct sptree_node *);
	int (*cmp)(const struct sptree_node *, const struct sptree_node *);
	void (*copy)(struct sptree_node *, struct sptree_node *);
};

struct sptree_root {
	struct sptree_ops *ops;
	struct sptree_node __rcu *root;
};

/**
 * sptree_entry - get the struct for this entry
 * @ptr:	the &struct sptree_node pointer.
 * @type:	the type of the struct this is embedded in.
 * @member:	the name of the sptree_node within the struct.
 */
#define sptree_entry(ptr, type, member) \
	container_of(ptr, type, member)

#define sptree_entry_safe(ptr, type, member)	\
	({ typeof(ptr) ____ptr = (ptr);		\
	   ____ptr ? sptree_entry(____ptr, type, member) : NULL; \
	})

/* in-order iterator */
extern struct sptree_node *sptree_first(struct sptree_root *root);
extern struct sptree_node *sptree_next(struct sptree_node *node);

/**
 * sptree_for_each - iterate in-order over nodes in a tree
 * @pos:	the &struct sptree_node to use as a loop cursor.
 * @root:	the root of the tree.
 */
#define sptree_for_each(pos, root)	\
	for (pos = sptree_first(root); pos != NULL; pos = sptree_next(pos))

/**
 * sptree_for_each_entry - iterate in-order over tree of given type
 * @pos:	the type * to use as a loop cursor.
 * @root:	the root of the tree.
 * @member:	the name of the sptree_node within the struct.
 */
#define sptree_for_each_entry(pos, root, member)					\
	for (pos = sptree_entry_safe(sptree_first(root), typeof(*(pos)), member);	\
	     pos != NULL;								\
	     pos = sptree_entry_safe(sptree_next(&(pos)->member), typeof(*(pos)), member))


/* filters will have the same semantics as memcmp() */
typedef int (*filter)(const struct sptree_node *crnt, const void *arg);

extern struct sptree_node *sptree_first_filter(struct sptree_root *root, filter f, const void *arg);
extern struct sptree_node *sptree_next_filter(struct sptree_node *node, filter f, const void *arg);

#define sptree_for_each_entry_filter(pos, root, member, filter, arg)					\
	for (pos = sptree_entry_safe(sptree_first_filter(root, filter, arg), typeof(*(pos)), member);	\
	     pos != NULL;										\
	     pos = sptree_entry_safe(sptree_next_filter(&(pos)->member, filter, arg), typeof(*(pos)), member))


 /* post-order iterator */
extern struct sptree_node *sptree_first_po(struct sptree_root *root);
extern struct sptree_node *sptree_next_po(struct sptree_node *node);

#define sptree_for_each_po(pos, root)	\
	for (pos = sptree_first_po(root); pos != NULL; pos = sptree_next_po(pos))

#define sptree_for_each_entry_po(pos, root, member)					\
	for (pos = sptree_entry_safe(sptree_first_po(root), typeof(*(pos)), member);	\
	     pos != NULL;								\
	     pos = sptree_entry_safe(sptree_next_po(&(pos)->member), typeof(*(pos)), member))

/**
 * sptree_for_each_po_safe - iterate post-order over a tree safe against removal of nodes
 * @pos:	the &struct sptree_node to use as a loop cursor.
 * @n:		another &struct sptree_node to use as temporary storage
 * @root:	the root of the tree.
 *
 * Don't dare use this to delete nodes from a live tree.
 * Nodes must be first decoupled & made unreachable.
 */
#define sptree_for_each_po_safe(pos, n, root)		\
	for (pos = sptree_first_po(root);		\
	     pos && ({ n = sptree_next_po(pos); 1; });	\
	     pos = n)

extern void sptree_init(struct sptree_root *root, struct sptree_ops *ops);

/* write-side calls, must be protected by a lock */
extern void sptree_free(struct sptree_root *root);
extern int prealloc_insert(struct sptree_root *root, struct sptree_node *node);
extern struct sptree_node *prealloc_delete(struct sptree_root *root, const struct sptree_node *match);

/* test functions, also write-side calls, must be protected by a lock */
extern int test_unwind(struct sptree_root *root, const struct sptree_node *match);
extern int test_ror(struct sptree_root *root, const struct sptree_node *match);
extern int test_rol(struct sptree_root *root, const struct sptree_node *match);
extern int test_rrl(struct sptree_root *root, const struct sptree_node *match);
extern int test_rlr(struct sptree_root *root, const struct sptree_node *match);

/* read-side calls, must be protected by (S)RCU section */
extern struct sptree_node *search(struct sptree_root *root, const struct sptree_node *match);

#endif /* _SPECIALIZED_TREE_H_ */
