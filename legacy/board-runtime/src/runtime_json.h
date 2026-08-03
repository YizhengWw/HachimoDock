#ifndef BOARD_RUNTIME_JSON_H
#define BOARD_RUNTIME_JSON_H

#include <stdbool.h>
#include <stddef.h>

typedef enum {
  BR_JSON_UNDEFINED = 0,
  BR_JSON_OBJECT = 1,
  BR_JSON_ARRAY = 2,
  BR_JSON_STRING = 3,
  BR_JSON_PRIMITIVE = 4
} br_json_type;

typedef struct {
  br_json_type type;
  int start;
  int end;
  int size;
  int parent;
} br_json_token;

int br_json_parse(const char *json, size_t length, br_json_token *tokens, int max_tokens);
bool br_json_token_eq(const char *json, const br_json_token *token, const char *value);
int br_json_find_key(
  const char *json,
  const br_json_token *tokens,
  int token_count,
  int object_index,
  const char *key
);
bool br_json_token_to_string(
  const char *json,
  const br_json_token *token,
  char *output,
  size_t output_size
);
bool br_json_token_to_double(const char *json, const br_json_token *token, double *value);
bool br_json_token_to_bool(const char *json, const br_json_token *token, bool *value);
bool br_json_copy_raw(
  const char *json,
  const br_json_token *token,
  char *output,
  size_t output_size
);

#endif
