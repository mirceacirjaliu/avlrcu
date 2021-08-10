
#ifndef _SPTREE_TEST_H_
#define _SPTREE_TEST_H_

#include "tree.h"
#include "internal.h"

struct test_sptree_node {
	/* task-specific fields */
	unsigned long address;

	/* link */
	struct sptree_node node;
};

#endif /* _SPTREE_TEST_H_ */
