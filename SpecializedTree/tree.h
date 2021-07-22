
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
	unsigned long (*get_key)(struct sptree_node *);
	void (*copy)(struct sptree_node *, struct sptree_node *);
};

struct sptree_root {
	struct sptree_ops *ops;
	struct sptree_node __rcu *root;
};

/* context for insert/delete operations */
struct sptree_ctxt {
	struct sptree_root *root;
	struct llist_head old;
	int diff;
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

#define NODE_FMT "(%lx, %ld)"
#define NODE_ARG(__node) (long)&(__node), (long)(__node)->balance


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


/* new in-order iterator based only on pointer */
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


extern int sptree_init(struct sptree_root *root, struct sptree_ops *ops);
extern void sptree_free(struct sptree_root *root);

// these must be protected by a lock
extern int prealloc_insert(struct sptree_root *root, struct sptree_node *node);
extern int prealloc_delete(struct sptree_root *root, unsigned long key);
extern int prealloc_unwind(struct sptree_root *root, unsigned long key);

// same for these
extern int sptree_ror(struct sptree_root *root, unsigned long key);
extern int sptree_rol(struct sptree_root *root, unsigned long key);
extern int sptree_rrl(struct sptree_root *root, unsigned long key);
extern int sptree_rlr(struct sptree_root *root, unsigned long key);

// non-RCU protected, you need to protect it
extern struct sptree_node *search(struct sptree_root *root, unsigned long key);

#endif // _SPECIALIZED_TREE_H_