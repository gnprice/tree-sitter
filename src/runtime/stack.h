#ifndef RUNTIME_PARSE_STACK_H_
#define RUNTIME_PARSE_STACK_H_

#ifdef __cplusplus
extern "C" {
#endif

#include "runtime/array.h"
#include "runtime/tree.h"
#include "runtime/tree_pool.h"
#include "runtime/error_costs.h"
#include <stdio.h>

typedef struct Stack Stack;

typedef unsigned StackVersion;
#define STACK_VERSION_NONE ((StackVersion)-1)

typedef struct {
  TreeArray trees;
  StackVersion version;
} StackSlice;

typedef Array(StackSlice) StackSliceArray;

typedef struct {
  StackSliceArray slices;
} StackPopResult;

typedef unsigned StackIterateAction;
enum {
  StackIterateNone,
  StackIterateStop = 1,
  StackIteratePop = 2,
};

typedef struct {
  Length position;
  unsigned depth;
  TSStateId state;
} StackSummaryEntry;

typedef Array(StackSummaryEntry) StackSummary;

typedef StackIterateAction (*StackIterateCallback)(void *, TSStateId state,
                                                   const TreeArray *trees,
                                                   uint32_t tree_count);

/*
 *  Create a parse stack.
 */
Stack *ts_stack_new(TreePool *);

/*
 *  Release any resources reserved by a parse stack.
 */
void ts_stack_delete(Stack *);

/*
 *  Get the stack's current number of versions.
 */
uint32_t ts_stack_version_count(const Stack *);

/*
 *  Get the state at the top of the given version of the stack. If the stack is
 *  empty, this returns the initial state (0).
 */
TSStateId ts_stack_top_state(const Stack *, StackVersion);

unsigned ts_stack_push_count(const Stack *, StackVersion);

void ts_stack_decrease_push_count(Stack *, StackVersion, unsigned);

Tree *ts_stack_last_external_token(const Stack *, StackVersion);

void ts_stack_set_last_external_token(Stack *, StackVersion, Tree *);

/*
 *  Get the position at the top of the given version of the stack. If the stack
 *  is empty, this returns zero.
 */
Length ts_stack_top_position(const Stack *, StackVersion);

/*
 *  Push a tree and state onto the given head of the stack.
 */
void ts_stack_push(Stack *, StackVersion, Tree *, bool, TSStateId);

/*
 *  Pop the given number of entries from the given version of the stack. This
 *  operation can increase the number of stack versions by revealing multiple
 *  versions which had previously been merged. It returns a struct that
 *  indicates the index of each revealed version and the trees removed from that
 *  version.
 */
StackPopResult ts_stack_pop_count(Stack *, StackVersion, uint32_t count);

StackPopResult ts_stack_iterate(Stack *, StackVersion, StackIterateCallback, void *);

StackPopResult ts_stack_pop_error(Stack *, StackVersion);

StackPopResult ts_stack_pop_pending(Stack *, StackVersion);

StackPopResult ts_stack_pop_all(Stack *, StackVersion);

unsigned ts_stack_depth_since_error(Stack *, StackVersion);

void ts_stack_record_summary(Stack *, StackVersion, unsigned max_depth);

StackSummary *ts_stack_get_summary(Stack *, StackVersion);

unsigned ts_stack_error_cost(const Stack *, StackVersion version);

bool ts_stack_merge(Stack *, StackVersion, StackVersion);

bool ts_stack_can_merge(Stack *, StackVersion, StackVersion);

void ts_stack_force_merge(Stack *, StackVersion, StackVersion);

void ts_stack_halt(Stack *, StackVersion);

bool ts_stack_is_halted(Stack *, StackVersion);

void ts_stack_renumber_version(Stack *, StackVersion, StackVersion);

void ts_stack_swap_versions(Stack *, StackVersion, StackVersion);

StackVersion ts_stack_copy_version(Stack *, StackVersion);

/*
 *  Remove the given version from the stack.
 */
void ts_stack_remove_version(Stack *, StackVersion);

/*
 *  Remove all entries from the stack.
 */
void ts_stack_clear(Stack *);

bool ts_stack_print_dot_graph(Stack *, const char **, FILE *);

#ifdef __cplusplus
}
#endif

#endif  // RUNTIME_PARSE_STACK_H_
