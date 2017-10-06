#ifndef RUNTIME_TREE_POOL_H_
#define RUNTIME_TREE_POOL_H_

#include "runtime/array.h"

typedef struct Tree Tree;
typedef struct TreePoolSlab TreePoolSlab;

typedef struct {
  Array(TreePoolSlab) slabs;
  Array(Tree *) tree_stack;
  unsigned first_available_slab_index;
} TreePool;

void ts_tree_pool_init(TreePool *);
void ts_tree_pool_delete(TreePool *);
Tree *ts_tree_pool_allocate(TreePool *);
void ts_tree_pool_free(TreePool *, Tree *);

#endif  // RUNTIME_TREE_POOL_H_
