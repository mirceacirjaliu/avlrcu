
#ifndef _AVLRCU_TEST_H_
#define _AVLRCU_TEST_H_

#include "tree.h"
#include "internal.h"

struct test_avlrcu_node {
	/* task-specific fields */
	unsigned long address;

	/* link */
	struct avlrcu_node node;
};

#endif /* _AVLRCU_TEST_H_ */
