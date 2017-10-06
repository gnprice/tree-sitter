#include "runtime/parser.h"
#include <assert.h>
#include <stdio.h>
#include <limits.h>
#include <stdbool.h>
#include "tree_sitter/runtime.h"
#include "runtime/tree.h"
#include "runtime/lexer.h"
#include "runtime/length.h"
#include "runtime/array.h"
#include "runtime/language.h"
#include "runtime/alloc.h"
#include "runtime/reduce_action.h"
#include "runtime/error_costs.h"

#define LOG(...)                                                                            \
  if (self->lexer.logger.log || self->print_debugging_graphs) {                             \
    snprintf(self->lexer.debug_buffer, TREE_SITTER_SERIALIZATION_BUFFER_SIZE, __VA_ARGS__); \
    parser__log(self);                                                                      \
  }

#define LOG_STACK()                                                              \
  if (self->print_debugging_graphs) {                                            \
    ts_stack_print_dot_graph(self->stack, self->language->symbol_names, stderr); \
    fputs("\n\n", stderr);                                                       \
  }

#define LOG_TREE()                                                        \
  if (self->print_debugging_graphs) {                                     \
    ts_tree_print_dot_graph(self->finished_tree, self->language, stderr); \
    fputs("\n", stderr);                                                  \
  }

#define SYM_NAME(symbol) ts_language_symbol_name(self->language, symbol)

static const unsigned MAX_VERSION_COUNT = 6;
static const unsigned MAX_SUMMARY_DEPTH = 16;
static const unsigned MAX_COST_DIFFERENCE = 16 * ERROR_COST_PER_SKIPPED_TREE;

typedef struct {
  unsigned cost;
  unsigned push_count;
  bool is_in_error;
} ErrorStatus;

typedef enum {
  ErrorComparisonTakeLeft,
  ErrorComparisonPreferLeft,
  ErrorComparisonNone,
  ErrorComparisonPreferRight,
  ErrorComparisonTakeRight,
} ErrorComparison;

static void parser__log(Parser *self) {
  if (self->lexer.logger.log) {
    self->lexer.logger.log(
      self->lexer.logger.payload,
      TSLogTypeParse,
      self->lexer.debug_buffer
    );
  }

  if (self->print_debugging_graphs) {
    fprintf(stderr, "graph {\nlabel=\"");
    for (char *c = &self->lexer.debug_buffer[0]; *c != 0; c++) {
      if (*c == '"') fputc('\\', stderr);
      fputc(*c, stderr);
    }
    fprintf(stderr, "\"\n}\n\n");
  }
}

static bool parser__breakdown_top_of_stack(Parser *self, StackVersion version) {
  bool did_break_down = false;
  bool pending = false;

  do {
    StackPopResult pop = ts_stack_pop_pending(self->stack, version);
    if (!pop.slices.size)
      break;

    did_break_down = true;
    pending = false;
    for (uint32_t i = 0; i < pop.slices.size; i++) {
      StackSlice slice = pop.slices.contents[i];
      TSStateId state = ts_stack_top_state(self->stack, slice.version);
      Tree *parent = *array_front(&slice.trees);

      for (uint32_t j = 0; j < parent->child_count; j++) {
        Tree *child = parent->children[j];
        pending = child->child_count > 0;

        if (child->symbol == ts_builtin_sym_error) {
          state = ERROR_STATE;
        } else if (!child->extra) {
          state = ts_language_next_state(self->language, state, child->symbol);
        }

        ts_stack_push(self->stack, slice.version, child, pending, state);
      }

      for (uint32_t j = 1; j < slice.trees.size; j++) {
        Tree *tree = slice.trees.contents[j];
        ts_stack_push(self->stack, slice.version, tree, false, state);
        ts_tree_release(&self->tree_pool, tree);
      }

      LOG("breakdown_top_of_stack tree:%s", SYM_NAME(parent->symbol));
      LOG_STACK();

      ts_stack_decrease_push_count(self->stack, slice.version, parent->child_count + 1);

      ts_tree_release(&self->tree_pool, parent);
      array_delete(&slice.trees);
    }
  } while (pending);

  return did_break_down;
}

static void parser__breakdown_lookahead(Parser *self, Tree **lookahead,
                                        TSStateId state,
                                        ReusableNode *reusable_node) {
  bool did_break_down = false;
  while (reusable_node->tree->child_count > 0 && reusable_node->tree->parse_state != state) {
    LOG("state_mismatch sym:%s", SYM_NAME(reusable_node->tree->symbol));
    reusable_node_breakdown(reusable_node);
    did_break_down = true;
  }

  if (did_break_down) {
    ts_tree_release(&self->tree_pool, *lookahead);
    ts_tree_retain(*lookahead = reusable_node->tree);
  }
}

static ErrorComparison parser__compare_versions(Parser *self, ErrorStatus a, ErrorStatus b) {
  if (!a.is_in_error && b.is_in_error) {
    if (a.cost < b.cost) {
      return ErrorComparisonTakeLeft;
    } else {
      return ErrorComparisonPreferLeft;
    }
  }

  if (a.is_in_error && !b.is_in_error) {
    if (b.cost < a.cost) {
      return ErrorComparisonTakeRight;
    } else {
      return ErrorComparisonPreferRight;
    }
  }

  if (a.cost < b.cost) {
    if ((b.cost - a.cost) * (1 + a.push_count) > MAX_COST_DIFFERENCE) {
      return ErrorComparisonTakeLeft;
    } else {
      return ErrorComparisonPreferLeft;
    }
  }

  if (b.cost < a.cost) {
    if ((a.cost - b.cost) * (1 + b.push_count) > MAX_COST_DIFFERENCE) {
      return ErrorComparisonTakeRight;
    } else {
      return ErrorComparisonPreferRight;
    }
  }

  return ErrorComparisonNone;
}

static bool parser__better_version_exists(Parser *self, StackVersion version,
                                          bool is_in_error, unsigned cost) {
  if (self->finished_tree && self->finished_tree->error_cost <= cost) return true;

  ErrorStatus status = {.cost = cost, .is_in_error = is_in_error, .push_count = 0};

  for (StackVersion i = 0, n = ts_stack_version_count(self->stack); i < n; i++) {
    if (i == version || ts_stack_is_halted(self->stack, i)) continue;
    ErrorStatus status_i = {
      .cost = ts_stack_error_cost(self->stack, i),
      .is_in_error = ts_stack_top_state(self->stack, i) == ERROR_STATE,
      .push_count = ts_stack_push_count(self->stack, i)
    };
    switch (parser__compare_versions(self, status, status_i)) {
      case ErrorComparisonTakeRight:
        return true;
      case ErrorComparisonPreferRight:
        if (ts_stack_can_merge(self->stack, i, version)) return true;
      default:
        break;
    }
  }

  return false;
}

static bool parser__condense_stack(Parser *self) {
  bool made_changes = false;
  unsigned min_error_cost = UINT_MAX;
  bool all_versions_have_error = true;
  for (StackVersion i = 0; i < ts_stack_version_count(self->stack); i++) {
    if (ts_stack_is_halted(self->stack, i)) {
      ts_stack_remove_version(self->stack, i);
      i--;
      continue;
    }

    ErrorStatus status_i = {
      .cost = ts_stack_error_cost(self->stack, i),
      .push_count = ts_stack_push_count(self->stack, i),
      .is_in_error = ts_stack_top_state(self->stack, i) == ERROR_STATE,
    };
    if (!status_i.is_in_error) all_versions_have_error = false;
    if (status_i.cost < min_error_cost) min_error_cost = status_i.cost;

    for (StackVersion j = 0; j < i; j++) {
      ErrorStatus status_j = {
        .cost = ts_stack_error_cost(self->stack, j),
        .push_count = ts_stack_push_count(self->stack, j),
        .is_in_error = ts_stack_top_state(self->stack, j) == ERROR_STATE,
      };

      bool can_merge = ts_stack_can_merge(self->stack, j, i);
      switch (parser__compare_versions(self, status_j, status_i)) {
        case ErrorComparisonTakeLeft:
          made_changes = true;
          ts_stack_remove_version(self->stack, i);
          i--;
          j = i;
          break;
        case ErrorComparisonPreferLeft:
          if (can_merge) {
            made_changes = true;
            ts_stack_remove_version(self->stack, i);
            i--;
            j = i;
          }
          break;
        case ErrorComparisonNone:
          if (can_merge) {
            made_changes = true;
            ts_stack_force_merge(self->stack, j, i);
            i--;
            j = i;
          }
          break;
        case ErrorComparisonPreferRight:
          made_changes = true;
          if (can_merge) {
            ts_stack_remove_version(self->stack, j);
            i--;
            j--;
          } else {
            ts_stack_swap_versions(self->stack, i, j);
            j = i;
          }
          break;
        case ErrorComparisonTakeRight:
          made_changes = true;
          ts_stack_remove_version(self->stack, j);
          i--;
          j--;
          break;
      }
    }
  }

  while (ts_stack_version_count(self->stack) > MAX_VERSION_COUNT) {
    ts_stack_remove_version(self->stack, MAX_VERSION_COUNT);
    made_changes = true;
  }

  if (made_changes) {
    LOG("condense");
    LOG_STACK();
  }

  return
    (all_versions_have_error && ts_stack_version_count(self->stack) > 0) ||
    (self->finished_tree && self->finished_tree->error_cost < min_error_cost);
}

static void parser__restore_external_scanner(Parser *self, Tree *external_token) {
  if (external_token) {
    self->language->external_scanner.deserialize(
      self->external_scanner_payload,
      ts_external_token_state_data(&external_token->external_token_state),
      external_token->external_token_state.length
    );
  } else {
    self->language->external_scanner.deserialize(self->external_scanner_payload, NULL, 0);
  }
}

static Tree *parser__lex(Parser *self, StackVersion version, TSStateId parse_state) {
  Length start_position = ts_stack_top_position(self->stack, version);
  Tree *external_token = ts_stack_last_external_token(self->stack, version);
  TSLexMode lex_mode = self->language->lex_modes[parse_state];
  const bool *valid_external_tokens = ts_language_enabled_external_tokens(
    self->language,
    lex_mode.external_lex_state
  );

  bool found_external_token = false;
  bool skipped_error = false;
  bool error_mode = parse_state == ERROR_STATE;
  int32_t first_error_character = 0;
  Length error_start_position, error_end_position;
  uint32_t last_byte_scanned = start_position.bytes;
  ts_lexer_reset(&self->lexer, start_position);

  for (;;) {
    Length current_position = self->lexer.current_position;

    if (valid_external_tokens) {
      LOG(
        "lex_external state:%d, row:%u, column:%u",
        lex_mode.external_lex_state,
        current_position.extent.row,
        current_position.extent.column
      );
      ts_lexer_start(&self->lexer);
      parser__restore_external_scanner(self, external_token);
      if (self->language->external_scanner.scan(
        self->external_scanner_payload,
        &self->lexer.data,
        valid_external_tokens
      )) {
        if (length_has_unknown_chars(self->lexer.token_end_position)) {
          self->lexer.token_end_position = self->lexer.current_position;
        }

        if (error_mode && self->lexer.token_end_position.bytes <= current_position.bytes) {
          LOG("disregard_empty_token");
        } else {
          found_external_token = true;
          break;
        }
      }

      if (self->lexer.current_position.bytes > last_byte_scanned) {
        last_byte_scanned = self->lexer.current_position.bytes;
      }
      ts_lexer_reset(&self->lexer, current_position);
    }

    LOG(
      "lex_internal state:%d, row:%u, column:%u",
      lex_mode.lex_state,
      current_position.extent.row,
      current_position.extent.column
    );
    ts_lexer_start(&self->lexer);
    if (self->language->lex_fn(&self->lexer.data, lex_mode.lex_state)) {
      if (length_has_unknown_chars(self->lexer.token_end_position)) {
        self->lexer.token_end_position = self->lexer.current_position;
      }
      break;
    }

    if (!error_mode) {
      LOG("retry_in_error_mode");
      error_mode = true;
      lex_mode = self->language->lex_modes[ERROR_STATE];
      valid_external_tokens = ts_language_enabled_external_tokens(
        self->language,
        lex_mode.external_lex_state
      );
      if (self->lexer.current_position.bytes > last_byte_scanned) {
        last_byte_scanned = self->lexer.current_position.bytes;
      }
      ts_lexer_reset(&self->lexer, start_position);
      continue;
    }

    if (!skipped_error) {
      LOG("skip_unrecognized_character");
      skipped_error = true;
      error_start_position = self->lexer.token_start_position;
      error_end_position = self->lexer.token_start_position;
      first_error_character = self->lexer.data.lookahead;
    }

    if (self->lexer.current_position.bytes == error_end_position.bytes) {
      if (self->lexer.data.lookahead == 0) {
        self->lexer.data.result_symbol = ts_builtin_sym_error;
        break;
      }
      self->lexer.data.advance(&self->lexer, false);
    }

    error_end_position = self->lexer.current_position;
  }

  Tree *result;
  if (skipped_error) {
    Length padding = length_sub(error_start_position, start_position);
    Length size = length_sub(error_end_position, error_start_position);
    result = ts_tree_make_error(&self->tree_pool, size, padding, first_error_character, self->language);
  } else {
    TSSymbol symbol = self->lexer.data.result_symbol;
    if (found_external_token) {
      symbol = self->language->external_scanner.symbol_map[symbol];
    }

    Length padding = length_sub(self->lexer.token_start_position, start_position);
    Length size = length_sub(self->lexer.token_end_position, self->lexer.token_start_position);
    result = ts_tree_make_leaf(&self->tree_pool, symbol, padding, size, self->language);

    if (found_external_token) {
      result->has_external_tokens = true;
      unsigned length = self->language->external_scanner.serialize(
        self->external_scanner_payload,
        self->lexer.debug_buffer
      );
      ts_external_token_state_init(&result->external_token_state, self->lexer.debug_buffer, length);
    }
  }

  if (self->lexer.current_position.bytes > last_byte_scanned) {
    last_byte_scanned = self->lexer.current_position.bytes;
  }
  result->bytes_scanned = last_byte_scanned - start_position.bytes + 1;
  result->parse_state = parse_state;
  result->first_leaf.lex_mode = lex_mode;

  LOG("lexed_lookahead sym:%s, size:%u", SYM_NAME(result->symbol), result->size.bytes);
  return result;
}

static Tree *parser__get_cached_token(Parser *self, size_t byte_index, Tree *last_external_token) {
  TokenCache *cache = &self->token_cache;
  if (cache->token &&
      cache->byte_index == byte_index &&
      ts_tree_external_token_state_eq(cache->last_external_token, last_external_token)) {
    return cache->token;
  } else {
    return NULL;
  }
}

static void parser__set_cached_token(Parser *self, size_t byte_index, Tree *last_external_token,
                                     Tree *token) {
  TokenCache *cache = &self->token_cache;
  if (token) ts_tree_retain(token);
  if (last_external_token) ts_tree_retain(last_external_token);
  if (cache->token) ts_tree_release(&self->tree_pool, cache->token);
  if (cache->last_external_token) ts_tree_release(&self->tree_pool, cache->last_external_token);
  cache->token = token;
  cache->byte_index = byte_index;
  cache->last_external_token = last_external_token;
}

static bool parser__can_reuse_first_leaf(Parser *self, TSStateId state, Tree *tree,
                                         TableEntry *table_entry) {
  TSLexMode current_lex_mode = self->language->lex_modes[state];
  return
    (tree->first_leaf.lex_mode.lex_state == current_lex_mode.lex_state &&
     tree->first_leaf.lex_mode.external_lex_state == current_lex_mode.external_lex_state) ||
    (current_lex_mode.external_lex_state == 0 &&
     tree->size.bytes > 0 &&
     table_entry->is_reusable &&
     (!table_entry->depends_on_lookahead || (tree->child_count > 1 && tree->error_cost == 0)));
}

static Tree *parser__get_lookahead(Parser *self, StackVersion version, TSStateId *state,
                                   ReusableNode *reusable_node, TableEntry *table_entry) {
  Length position = ts_stack_top_position(self->stack, version);
  Tree *last_external_token = ts_stack_last_external_token(self->stack, version);

  Tree *result;
  while ((result = reusable_node->tree)) {
    if (reusable_node->byte_index > position.bytes) {
      LOG("before_reusable_node symbol:%s", SYM_NAME(result->symbol));
      break;
    }

    if (reusable_node->byte_index < position.bytes) {
      LOG("past_reusable_node symbol:%s", SYM_NAME(result->symbol));
      reusable_node_pop(reusable_node);
      continue;
    }

    if (!ts_tree_external_token_state_eq(reusable_node->last_external_token, last_external_token)) {
      LOG("reusable_node_has_different_external_scanner_state symbol:%s", SYM_NAME(result->symbol));
      reusable_node_pop(reusable_node);
      continue;
    }

    const char *reason = NULL;
    if (result->has_changes) {
      reason = "has_changes";
    } else if (result->symbol == ts_builtin_sym_error) {
      reason = "is_error";
    } else if (result->fragile_left || result->fragile_right) {
      reason = "is_fragile";
    } else if (self->in_ambiguity && result->child_count) {
      reason = "in_ambiguity";
    }

    if (reason) {
      LOG("cant_reuse_node_%s tree:%s", reason, SYM_NAME(result->symbol));
      if (!reusable_node_breakdown(reusable_node)) {
        reusable_node_pop(reusable_node);
        parser__breakdown_top_of_stack(self, version);
        *state = ts_stack_top_state(self->stack, version);
      }
      continue;
    }

    ts_language_table_entry(self->language, *state, result->first_leaf.symbol, table_entry);
    if (!parser__can_reuse_first_leaf(self, *state, result, table_entry)) {
      LOG(
        "cant_reuse_node symbol:%s, first_leaf_symbol:%s",
        SYM_NAME(result->symbol),
        SYM_NAME(result->first_leaf.symbol)
      );
      reusable_node_pop_leaf(reusable_node);
      break;
    }

    LOG("reuse_node symbol:%s", SYM_NAME(result->symbol));
    ts_tree_retain(result);
    return result;
  }

  if ((result = parser__get_cached_token(self, position.bytes, last_external_token))) {
    ts_language_table_entry(self->language, *state, result->first_leaf.symbol, table_entry);
    if (parser__can_reuse_first_leaf(self, *state, result, table_entry)) {
      ts_tree_retain(result);
      return result;
    }
  }

  result = parser__lex(self, version, *state);
  parser__set_cached_token(self, position.bytes, last_external_token, result);
  ts_language_table_entry(self->language, *state, result->symbol, table_entry);
  return result;
}

static bool parser__select_tree(Parser *self, Tree *left, Tree *right) {
  if (!left) return true;
  if (!right) return false;

  if (right->error_cost < left->error_cost) {
    LOG("select_smaller_error symbol:%s, over_symbol:%s",
        SYM_NAME(right->symbol), SYM_NAME(left->symbol));
    return true;
  }

  if (left->error_cost < right->error_cost) {
    LOG("select_smaller_error symbol:%s, over_symbol:%s",
        SYM_NAME(left->symbol), SYM_NAME(right->symbol));
    return false;
  }

  if (right->dynamic_precedence > left->dynamic_precedence) {
    LOG("select_higher_precedence symbol:%s, prec:%u, over_symbol:%s, other_prec:%u",
        SYM_NAME(right->symbol), right->dynamic_precedence, SYM_NAME(left->symbol),
        left->dynamic_precedence);
    return true;
  }

  if (left->dynamic_precedence > right->dynamic_precedence) {
    LOG("select_higher_precedence symbol:%s, prec:%u, over_symbol:%s, other_prec:%u",
        SYM_NAME(left->symbol), left->dynamic_precedence, SYM_NAME(right->symbol),
        right->dynamic_precedence);
    return false;
  }

  if (left->error_cost > 0) return true;

  int comparison = ts_tree_compare(left, right);
  switch (comparison) {
    case -1:
      LOG("select_earlier symbol:%s, over_symbol:%s", SYM_NAME(left->symbol),
          SYM_NAME(right->symbol));
      return false;
      break;
    case 1:
      LOG("select_earlier symbol:%s, over_symbol:%s", SYM_NAME(right->symbol),
          SYM_NAME(left->symbol));
      return true;
    default:
      LOG("select_existing symbol:%s, over_symbol:%s", SYM_NAME(left->symbol),
          SYM_NAME(right->symbol));
      return false;
  }
}

static void parser__shift(Parser *self, StackVersion version, TSStateId state,
                          Tree *lookahead, bool extra) {
  if (extra != lookahead->extra) {
    if (ts_stack_version_count(self->stack) > 1) {
      lookahead = ts_tree_make_copy(&self->tree_pool, lookahead);
    } else {
      ts_tree_retain(lookahead);
    }
    lookahead->extra = extra;
  } else {
    ts_tree_retain(lookahead);
  }

  bool is_pending = lookahead->child_count > 0;
  ts_stack_push(self->stack, version, lookahead, is_pending, state);
  if (lookahead->has_external_tokens) {
    ts_stack_set_last_external_token(
      self->stack, version, ts_tree_last_external_token(lookahead)
    );
  }
  ts_tree_release(&self->tree_pool, lookahead);
}

static bool parser__replace_children(Parser *self, Tree *tree, Tree **children, uint32_t count) {
  self->scratch_tree = *tree;
  self->scratch_tree.child_count = 0;
  ts_tree_set_children(&self->scratch_tree, count, children, self->language);
  if (parser__select_tree(self, tree, &self->scratch_tree)) {
    *tree = self->scratch_tree;
    return true;
  } else {
    return false;
  }
}

static StackPopResult parser__reduce(Parser *self, StackVersion version, TSSymbol symbol,
                                     uint32_t count, int dynamic_precedence,
                                     uint16_t alias_sequence_id, bool fragile) {
  uint32_t initial_version_count = ts_stack_version_count(self->stack);

  StackPopResult pop = ts_stack_pop_count(self->stack, version, count);

  for (uint32_t i = 0; i < pop.slices.size; i++) {
    StackSlice slice = pop.slices.contents[i];

    // Extra tokens on top of the stack should not be included in this new parent
    // node. They will be re-pushed onto the stack after the parent node is
    // created and pushed.
    uint32_t child_count = slice.trees.size;
    while (child_count > 0 && slice.trees.contents[child_count - 1]->extra) {
      child_count--;
    }

    Tree *parent = ts_tree_make_node(&self->tree_pool,
      symbol, child_count, slice.trees.contents, alias_sequence_id, self->language
    );

    // This pop operation may have caused multiple stack versions to collapse
    // into one, because they all diverged from a common state. In that case,
    // choose one of the arrays of trees to be the parent node's children, and
    // delete the rest of the tree arrays.
    while (i + 1 < pop.slices.size) {
      StackSlice next_slice = pop.slices.contents[i + 1];
      if (next_slice.version != slice.version) break;
      i++;

      uint32_t child_count = next_slice.trees.size;
      while (child_count > 0 && next_slice.trees.contents[child_count - 1]->extra) {
        child_count--;
      }

      if (parser__replace_children(self, parent, next_slice.trees.contents, child_count)) {
        ts_tree_array_delete(&self->tree_pool, &slice.trees);
        slice = next_slice;
      } else {
        ts_tree_array_delete(&self->tree_pool, &next_slice.trees);
      }
    }

    parent->dynamic_precedence += dynamic_precedence;
    parent->alias_sequence_id = alias_sequence_id;

    TSStateId state = ts_stack_top_state(self->stack, slice.version);
    TSStateId next_state = ts_language_next_state(self->language, state, symbol);
    if (fragile || self->in_ambiguity || pop.slices.size > 1 || initial_version_count > 1) {
      parent->fragile_left = true;
      parent->fragile_right = true;
      parent->parse_state = TS_TREE_STATE_NONE;
    } else {
      parent->parse_state = state;
    }

    // Push the parent node onto the stack, along with any extra tokens that
    // were previously on top of the stack.
    ts_stack_push(self->stack, slice.version, parent, false, next_state);
    ts_tree_release(&self->tree_pool, parent);
    for (uint32_t j = parent->child_count; j < slice.trees.size; j++) {
      Tree *tree = slice.trees.contents[j];
      ts_stack_push(self->stack, slice.version, tree, false, next_state);
      ts_tree_release(&self->tree_pool, tree);
    }
  }

  for (StackVersion i = initial_version_count; i < ts_stack_version_count(self->stack); i++) {
    for (StackVersion j = initial_version_count; j < i; j++) {
      if (ts_stack_merge(self->stack, j, i)) {
        i--;
        break;
      }
    }
  }

  return pop;
}

static void parser__start(Parser *self, TSInput input, Tree *previous_tree) {
  if (previous_tree) {
    LOG("parse_after_edit");
  } else {
    LOG("new_parse");
  }

  if (self->language->external_scanner.deserialize) {
    self->language->external_scanner.deserialize(self->external_scanner_payload, NULL, 0);
  }

  ts_lexer_set_input(&self->lexer, input);
  ts_stack_clear(self->stack);
  self->reusable_node = reusable_node_new(previous_tree);
  self->finished_tree = NULL;
}

static void parser__accept(Parser *self, StackVersion version,
                           Tree *lookahead) {
  lookahead->extra = true;
  assert(lookahead->symbol == ts_builtin_sym_end);
  ts_stack_push(self->stack, version, lookahead, false, 1);
  StackPopResult pop = ts_stack_pop_all(self->stack, version);

  for (uint32_t i = 0; i < pop.slices.size; i++) {
    StackSlice slice = pop.slices.contents[i];
    TreeArray trees = slice.trees;

    Tree *root = NULL;
    if (trees.size == 1) {
      root = trees.contents[0];
      array_delete(&trees);
    } else {
      for (uint32_t j = trees.size - 1; j + 1 > 0; j--) {
        Tree *child = trees.contents[j];
        if (!child->extra) {
          root = ts_tree_make_copy(&self->tree_pool, child);
          root->child_count = 0;
          for (uint32_t k = 0; k < child->child_count; k++)
            ts_tree_retain(child->children[k]);
          array_splice(&trees, j, 1, child->child_count, child->children);
          ts_tree_set_children(root, trees.size, trees.contents, self->language);
          ts_tree_release(&self->tree_pool, child);
          break;
        }
      }
    }

    assert(root && root->ref_count > 0);

    if (self->finished_tree) {
      if (parser__select_tree(self, self->finished_tree, root)) {
        ts_tree_release(&self->tree_pool, self->finished_tree);
        self->finished_tree = root;
      } else {
        ts_tree_release(&self->tree_pool, root);
      }
    } else {
      self->finished_tree = root;
    }
  }

  ts_stack_remove_version(self->stack, pop.slices.contents[0].version);
  ts_stack_halt(self->stack, version);
}

static bool parser__do_potential_reductions(Parser *self, StackVersion version) {
  bool has_shift_action = false;
  TSStateId state = ts_stack_top_state(self->stack, version);
  uint32_t previous_version_count = ts_stack_version_count(self->stack);

  array_clear(&self->reduce_actions);
  for (TSSymbol symbol = 0; symbol < self->language->token_count; symbol++) {
    TableEntry entry;
    ts_language_table_entry(self->language, state, symbol, &entry);
    for (uint32_t i = 0; i < entry.action_count; i++) {
      TSParseAction action = entry.actions[i];
      if (action.params.extra)
        continue;
      switch (action.type) {
        case TSParseActionTypeShift:
        case TSParseActionTypeRecover:
          has_shift_action = true;
          break;
        case TSParseActionTypeReduce:
          if (action.params.child_count > 0)
            ts_reduce_action_set_add(&self->reduce_actions, (ReduceAction){
              .symbol = action.params.symbol,
              .count = action.params.child_count,
              .dynamic_precedence = action.params.dynamic_precedence,
              .alias_sequence_id = action.params.alias_sequence_id,
            });
        default:
          break;
      }
    }
  }

  bool did_reduce = false;
  for (uint32_t i = 0; i < self->reduce_actions.size; i++) {
    ReduceAction action = self->reduce_actions.contents[i];
    parser__reduce(
      self, version, action.symbol, action.count,
      action.dynamic_precedence, action.alias_sequence_id,
      true
    );
    did_reduce = true;
  }

  if (did_reduce) {
    if (has_shift_action) {
      return true;
    } else {
      ts_stack_renumber_version(self->stack, previous_version_count, version);
      return false;
    }
  } else {
    return true;
  }
}

static void parser__handle_error(Parser *self, StackVersion version, TSSymbol lookahead_symbol) {
  // If there are other stack versions that are clearly better than this one,
  // just halt this version.
  unsigned new_cost = ts_stack_error_cost(self->stack, version) + ERROR_COST_PER_SKIPPED_TREE;
  if (parser__better_version_exists(self, version, true, new_cost)) {
    ts_stack_halt(self->stack, version);
    LOG("bail_on_error");
    return;
  }

  LOG("handle_error");

  // Perform any reductions that could have happened in this state, regardless
  // of the lookahead.
  uint32_t previous_version_count = ts_stack_version_count(self->stack);
  for (StackVersion v = version; v < ts_stack_version_count(self->stack);) {
    if (parser__do_potential_reductions(self, v)) {
      if (v == version) {
        v = previous_version_count;
      } else {
        v++;
      }
    }
  }

  // Push a discontinuity onto the stack. Merge all of the stack versions that
  // were created in the previous step.
  ts_stack_push(self->stack, version, NULL, false, ERROR_STATE);
  while (ts_stack_version_count(self->stack) > previous_version_count) {
    ts_stack_push(self->stack, previous_version_count, NULL, false, ERROR_STATE);
    ts_stack_force_merge(self->stack, version, previous_version_count);
  }

  ts_stack_record_summary(self->stack, version, MAX_SUMMARY_DEPTH);
  LOG_STACK();
}

static void parser__halt_parse(Parser *self) {
  LOG("halting_parse");
  LOG_STACK();

  ts_lexer_advance_to_end(&self->lexer);
  Length remaining_length = length_sub(
    self->lexer.current_position,
    ts_stack_top_position(self->stack, 0)
  );

  Tree *filler_node = ts_tree_make_error(&self->tree_pool, remaining_length, length_zero(), 0, self->language);
  filler_node->visible = false;
  ts_stack_push(self->stack, 0, filler_node, false, 0);
  ts_tree_release(&self->tree_pool, filler_node);

  TreeArray children = array_new();
  Tree *root_error = ts_tree_make_error_node(&self->tree_pool, &children, self->language);
  ts_stack_push(self->stack, 0, root_error, false, 0);
  ts_tree_release(&self->tree_pool, root_error);

  Tree *eof = ts_tree_make_leaf(&self->tree_pool, ts_builtin_sym_end, length_zero(), length_zero(), self->language);
  parser__accept(self, 0, eof);
  ts_tree_release(&self->tree_pool, eof);
}

static void parser__recover(Parser *self, StackVersion version, Tree *lookahead) {
  bool did_recover = false;
  unsigned previous_version_count = ts_stack_version_count(self->stack);
  Length position = ts_stack_top_position(self->stack, version);
  StackSummary *summary = ts_stack_get_summary(self->stack, version);
  for (unsigned i = 0; i < summary->size; i++) {
    StackSummaryEntry entry = summary->contents[i];
    if (entry.state == ERROR_STATE) continue;
    unsigned depth = entry.depth + ts_stack_depth_since_error(self->stack, version);

    unsigned new_cost =
      depth * ERROR_COST_PER_SKIPPED_TREE +
      (position.chars - entry.position.chars) * ERROR_COST_PER_SKIPPED_CHAR +
      (position.extent.row - entry.position.extent.row) * ERROR_COST_PER_SKIPPED_LINE;
    if (parser__better_version_exists(self, version, false, new_cost)) break;

    unsigned count = 0;
    if (ts_language_actions(self->language, entry.state, lookahead->symbol, &count) && count > 0) {
      LOG("recover state:%u, depth:%u", entry.state, depth);
      StackPopResult pop = ts_stack_pop_count(self->stack, version, depth);
      StackVersion previous_version = STACK_VERSION_NONE;
      for (unsigned j = 0; j < pop.slices.size; j++) {
        StackSlice slice = pop.slices.contents[j];
        if (slice.version == previous_version) {
          ts_tree_array_delete(&self->tree_pool, &slice.trees);
          continue;
        }

        if (ts_stack_top_state(self->stack, slice.version) != entry.state) {
          ts_tree_array_delete(&self->tree_pool, &slice.trees);
          ts_stack_halt(self->stack, slice.version);
          continue;
        }

        StackPopResult error_pop = ts_stack_pop_error(self->stack, slice.version);
        if (error_pop.slices.size > 0) {
          StackSlice error_slice = error_pop.slices.contents[0];
          array_push_all(&error_slice.trees, &slice.trees);
          array_delete(&slice.trees);
          slice.trees = error_slice.trees;
          ts_stack_renumber_version(self->stack, error_slice.version, slice.version);
        }

        TreeArray trailing_extras = ts_tree_array_remove_trailing_extras(&slice.trees);
        if (slice.trees.size > 0) {
          Tree *error = ts_tree_make_error_node(&self->tree_pool, &slice.trees, self->language);
          error->extra = true;
          ts_stack_push(self->stack, slice.version, error, false, entry.state);
          ts_tree_release(&self->tree_pool, error);
        } else {
          array_delete(&slice.trees);
        }
        previous_version = slice.version;

        for (unsigned k = 0; k < trailing_extras.size; k++) {
          Tree *tree = trailing_extras.contents[k];
          ts_stack_push(self->stack, slice.version, tree, false, entry.state);
          ts_tree_release(&self->tree_pool, tree);
        }

        array_delete(&trailing_extras);
        did_recover = true;
      }
      break;
    }
  }

  for (unsigned i = previous_version_count; i < ts_stack_version_count(self->stack); i++) {
    if (ts_stack_is_halted(self->stack, i)) {
      ts_stack_remove_version(self->stack, i);
      i--;
    } else {
      for (unsigned j = 0; j < i; j++) {
        if (ts_stack_can_merge(self->stack, j, i)) {
          ts_stack_remove_version(self->stack, i);
          i--;
          break;
        }
      }
    }
  }

  if (did_recover && ts_stack_version_count(self->stack) > MAX_VERSION_COUNT) {
    ts_stack_halt(self->stack, version);
    return;
  }

  if (lookahead->symbol == ts_builtin_sym_end) {
    LOG("recover_eof");
    TreeArray children = array_new();
    Tree *parent = ts_tree_make_error_node(&self->tree_pool, &children, self->language);
    ts_stack_push(self->stack, version, parent, false, 1);
    ts_tree_release(&self->tree_pool, parent);
    parser__accept(self, version, lookahead);
    return;
  }

  LOG("skip_token symbol:%s", SYM_NAME(lookahead->symbol));
  unsigned n;
  const TSParseAction *actions = ts_language_actions(self->language, 1, lookahead->symbol, &n);
  bool extra = n > 0 && actions[n - 1].type == TSParseActionTypeShift && actions[n - 1].params.extra;
  parser__shift(self, version, ERROR_STATE, lookahead, extra);

  if (parser__better_version_exists(self, version, true, ts_stack_error_cost(self->stack, version))) {
    ts_stack_halt(self->stack, version);
  }
}

static void parser__advance(Parser *self, StackVersion version, ReusableNode *reusable_node) {
  TSStateId state = ts_stack_top_state(self->stack, version);
  TableEntry table_entry;
  Tree *lookahead = parser__get_lookahead(self, version, &state, reusable_node, &table_entry);

  for (;;) {
    StackVersion last_reduction_version = STACK_VERSION_NONE;

    for (uint32_t i = 0; i < table_entry.action_count; i++) {
      TSParseAction action = table_entry.actions[i];

      switch (action.type) {
        case TSParseActionTypeShift: {
          TSStateId next_state;
          if (action.params.extra) {
            next_state = state;
            LOG("shift_extra");
          } else {
            next_state = action.params.state;
            LOG("shift state:%u", next_state);
          }

          if (lookahead->child_count > 0) {
            parser__breakdown_lookahead(self, &lookahead, state, reusable_node);
            next_state = ts_language_next_state(self->language, state, lookahead->symbol);
          }

          parser__shift(self, version, next_state, lookahead, action.params.extra);
          if (lookahead == reusable_node->tree) reusable_node_pop(reusable_node);
          ts_tree_release(&self->tree_pool, lookahead);
          return;
        }

        case TSParseActionTypeReduce: {
          LOG("reduce sym:%s, child_count:%u", SYM_NAME(action.params.symbol), action.params.child_count);
          StackPopResult reduction = parser__reduce(
            self, version, action.params.symbol, action.params.child_count,
            action.params.dynamic_precedence, action.params.alias_sequence_id,
            action.params.fragile
          );
          StackSlice slice = *array_front(&reduction.slices);
          last_reduction_version = slice.version;
          break;
        }

        case TSParseActionTypeAccept: {
          LOG("accept");
          parser__accept(self, version, lookahead);
          ts_tree_release(&self->tree_pool, lookahead);
          return;
        }

        case TSParseActionTypeRecover: {
          while (lookahead->child_count > 0) {
            parser__breakdown_lookahead(self, &lookahead, state, reusable_node);
          }
          parser__recover(self, version, lookahead);
          if (lookahead == reusable_node->tree) reusable_node_pop(reusable_node);
          ts_tree_release(&self->tree_pool, lookahead);
          return;
        }
      }
    }

    if (last_reduction_version != STACK_VERSION_NONE) {
      ts_stack_renumber_version(self->stack, last_reduction_version, version);
      LOG_STACK();
    } else if (!parser__breakdown_top_of_stack(self, version)) {
      if (state == ERROR_STATE) {
        ts_stack_push(self->stack, version, lookahead, false, ERROR_STATE);
        ts_tree_release(&self->tree_pool, lookahead);
        return;
      }

      parser__handle_error(self, version, lookahead->first_leaf.symbol);
      if (ts_stack_is_halted(self->stack, version)) {
        ts_tree_release(&self->tree_pool, lookahead);
        return;
      } else if (lookahead->size.bytes == 0) {
        ts_tree_release(&self->tree_pool, lookahead);
        state = ts_stack_top_state(self->stack, version);
        lookahead = parser__get_lookahead(self, version, &state, reusable_node, &table_entry);
      }
    }

    state = ts_stack_top_state(self->stack, version);
    ts_language_table_entry(self->language, state, lookahead->first_leaf.symbol, &table_entry);
  }
}

bool parser_init(Parser *self) {
  ts_lexer_init(&self->lexer);
  array_init(&self->reduce_actions);
  array_init(&self->tree_path1);
  array_init(&self->tree_path2);
  array_grow(&self->reduce_actions, 4);
  ts_tree_pool_init(&self->tree_pool);
  self->stack = ts_stack_new(&self->tree_pool);
  self->finished_tree = NULL;
  parser__set_cached_token(self, 0, NULL, NULL);
  return true;
}

void parser_set_language(Parser *self, const TSLanguage *language) {
  if (self->external_scanner_payload && self->language->external_scanner.destroy)
    self->language->external_scanner.destroy(self->external_scanner_payload);

  if (language && language->external_scanner.create)
    self->external_scanner_payload = language->external_scanner.create();
  else
    self->external_scanner_payload = NULL;

  self->language = language;
}

void parser_destroy(Parser *self) {
  if (self->stack)
    ts_stack_delete(self->stack);
  if (self->reduce_actions.contents)
    array_delete(&self->reduce_actions);
  if (self->tree_path1.contents)
    array_delete(&self->tree_path1);
  if (self->tree_path2.contents)
    array_delete(&self->tree_path2);
  ts_tree_pool_delete(&self->tree_pool);
  parser_set_language(self, NULL);
}

Tree *parser_parse(Parser *self, TSInput input, Tree *old_tree, bool halt_on_error) {
  parser__start(self, input, old_tree);

  StackVersion version = STACK_VERSION_NONE;
  uint32_t position = 0, last_position = 0;
  ReusableNode reusable_node;

  do {
    for (version = 0; version < ts_stack_version_count(self->stack); version++) {
      reusable_node = self->reusable_node;

      while (!ts_stack_is_halted(self->stack, version)) {
        position = ts_stack_top_position(self->stack, version).bytes;
        if (position > last_position || (version > 0 && position == last_position)) {
          last_position = position;
          break;
        }

        LOG("process version:%d, version_count:%u, state:%d, row:%u, col:%u",
            version, ts_stack_version_count(self->stack),
            ts_stack_top_state(self->stack, version),
            ts_stack_top_position(self->stack, version).extent.row,
            ts_stack_top_position(self->stack, version).extent.column);

        parser__advance(self, version, &reusable_node);
        LOG_STACK();
      }
    }

    self->reusable_node = reusable_node;

    bool should_halt = parser__condense_stack(self);
    if (should_halt) {
      if (self->finished_tree) {
        break;
      } else if (halt_on_error) {
        parser__halt_parse(self);
        break;
      }
    }

    self->in_ambiguity = version > 1;
  } while (version != 0);

  LOG("done");
  LOG_TREE();
  ts_stack_clear(self->stack);
  parser__set_cached_token(self, 0, NULL, NULL);
  ts_tree_assign_parents(self->finished_tree, &self->tree_path1, self->language);
  return self->finished_tree;
}
