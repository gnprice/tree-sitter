#include "runtime/tree_pool.h"
#include "runtime/tree.h"
#include "runtime/alloc.h"

#define SLAB_SIZE 64
#define BITMAP_TYPE uint64_t
#define FULL_BITMAP UINT64_MAX

struct TreePoolSlab {
  BITMAP_TYPE bitmap;
  Tree *trees;
};

static bool tree_pool_slab__is_full(TreePoolSlab *self) {
  return self->bitmap == FULL_BITMAP;
}

static Tree *tree_pool_slab__allocate(TreePoolSlab *self) {
  for (unsigned i = 0; i < SLAB_SIZE; i++) {
    BITMAP_TYPE mask = 1 << i;
    if ((self->bitmap & mask) == 0) {
      self->bitmap |= mask;
      return &self->trees[i];
    }
  }
  return NULL;
}

static bool tree_pool_slab__free(TreePoolSlab *self, Tree *tree) {
  long index = tree - self->trees;
  if (index < 0 || index >= SLAB_SIZE) return false;
  self->bitmap &= ~(1 << index);
  return true;
}

static void ts_tree_pool__add_slab(TreePool *self) {
  self->first_available_slab_index = self->slabs.size;
  array_push(&self->slabs, ((TreePoolSlab){
    .bitmap = 0,
    .trees = ts_calloc(SLAB_SIZE, sizeof(Tree)),
  }));
}

void ts_tree_pool_init(TreePool *self) {
  array_init(&self->slabs);
  array_init(&self->tree_stack);
  ts_tree_pool__add_slab(self);
}

void ts_tree_pool_delete(TreePool *self) {
  if (self->tree_stack.contents) {
    array_delete(&self->tree_stack);
  }
  for (unsigned i = 0, n = self->slabs.size; i < n; i++) {
    TreePoolSlab *slab = &self->slabs.contents[i];
    ts_free(slab->trees);
  }
  array_delete(&self->slabs);
}

Tree *ts_tree_pool_allocate(TreePool *self) {
  TreePoolSlab *slab = &self->slabs.contents[self->first_available_slab_index];
  Tree *tree = tree_pool_slab__allocate(slab);
  if (tree_pool_slab__is_full(slab)) {
    for (unsigned i = self->first_available_slab_index + 1; i < self->slabs.size; i++) {
      TreePoolSlab *slab = &self->slabs.contents[i];
      if (!tree_pool_slab__is_full(slab)) {
        self->first_available_slab_index = i;
        return tree;
      }
    }
    ts_tree_pool__add_slab(self);
  }
  return tree;
}

void ts_tree_pool_free(TreePool *self, Tree *tree) {
  for (unsigned i = self->slabs.size - 1; i + 1 > 0; i--) {
    TreePoolSlab *slab = &self->slabs.contents[i];
    if (tree_pool_slab__free(slab, tree)) {
      if (i < self->first_available_slab_index) self->first_available_slab_index = i;
      return;
    }
  }
  assert(!"Tree did not belong to any slab.");
}
