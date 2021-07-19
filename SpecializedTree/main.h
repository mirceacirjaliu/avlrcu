
#ifndef _SPECIALIZED_TREE_TEST_H_
#define _SPECIALIZED_TREE_TEST_H_

#include "tree.h"

struct test_sptree_node {
	/* task-specific fields */
	unsigned long address;

	/* link */
	struct sptree_node node;
};

#endif /* _SPECIALIZED_TREE_TEST_H_ */
