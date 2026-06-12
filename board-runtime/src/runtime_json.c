#include "runtime_json.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  const char *json;
  size_t length;
  size_t position;
  int next_token;
  int current_parent;
} br_json_parser;

static void br_json_skip_ws(br_json_parser *parser) {
  while (parser->position < parser->length && isspace((unsigned char) parser->json[parser->position])) {
    parser->position += 1;
  }
}

static int br_json_alloc_token(br_json_parser *parser, br_json_token *tokens, int max_tokens) {
  if (parser->next_token >= max_tokens) {
    return -1;
  }
  br_json_token *token = &tokens[parser->next_token];
  token->type = BR_JSON_UNDEFINED;
  token->start = -1;
  token->end = -1;
  token->size = 0;
  token->parent = -1;
  return parser->next_token++;
}

static int br_json_parse_value(br_json_parser *parser, br_json_token *tokens, int max_tokens);

static int br_json_parse_string(br_json_parser *parser, br_json_token *tokens, int max_tokens) {
  int index = br_json_alloc_token(parser, tokens, max_tokens);
  if (index < 0) {
    return -1;
  }

  br_json_token *token = &tokens[index];
  token->type = BR_JSON_STRING;
  token->parent = parser->current_parent;
  token->start = (int) parser->position + 1;
  parser->position += 1;

  while (parser->position < parser->length) {
    char ch = parser->json[parser->position];
    if (ch == '\\') {
      parser->position += 2;
      continue;
    }
    if (ch == '"') {
      token->end = (int) parser->position;
      parser->position += 1;
      return index;
    }
    parser->position += 1;
  }

  return -1;
}

static int br_json_parse_primitive(br_json_parser *parser, br_json_token *tokens, int max_tokens) {
  int index = br_json_alloc_token(parser, tokens, max_tokens);
  if (index < 0) {
    return -1;
  }

  br_json_token *token = &tokens[index];
  token->type = BR_JSON_PRIMITIVE;
  token->parent = parser->current_parent;
  token->start = (int) parser->position;

  while (parser->position < parser->length) {
    char ch = parser->json[parser->position];
    if (ch == ',' || ch == '}' || ch == ']' || isspace((unsigned char) ch)) {
      break;
    }
    parser->position += 1;
  }

  token->end = (int) parser->position;
  return index;
}

static int br_json_parse_array(br_json_parser *parser, br_json_token *tokens, int max_tokens) {
  int index = br_json_alloc_token(parser, tokens, max_tokens);
  if (index < 0) {
    return -1;
  }

  br_json_token *token = &tokens[index];
  token->type = BR_JSON_ARRAY;
  token->parent = parser->current_parent;
  token->start = (int) parser->position;
  parser->position += 1;

  int previous_parent = parser->current_parent;
  parser->current_parent = index;

  br_json_skip_ws(parser);
  if (parser->position < parser->length && parser->json[parser->position] == ']') {
    token->end = (int) parser->position + 1;
    parser->position += 1;
    parser->current_parent = previous_parent;
    return index;
  }

  while (parser->position < parser->length) {
    int child = br_json_parse_value(parser, tokens, max_tokens);
    if (child < 0) {
      return -1;
    }
    token->size += 1;
    br_json_skip_ws(parser);
    if (parser->position >= parser->length) {
      return -1;
    }
    if (parser->json[parser->position] == ',') {
      parser->position += 1;
      br_json_skip_ws(parser);
      continue;
    }
    if (parser->json[parser->position] == ']') {
      token->end = (int) parser->position + 1;
      parser->position += 1;
      parser->current_parent = previous_parent;
      return index;
    }
    return -1;
  }

  return -1;
}

static int br_json_parse_object(br_json_parser *parser, br_json_token *tokens, int max_tokens) {
  int index = br_json_alloc_token(parser, tokens, max_tokens);
  if (index < 0) {
    return -1;
  }

  br_json_token *token = &tokens[index];
  token->type = BR_JSON_OBJECT;
  token->parent = parser->current_parent;
  token->start = (int) parser->position;
  parser->position += 1;

  int previous_parent = parser->current_parent;
  parser->current_parent = index;

  br_json_skip_ws(parser);
  if (parser->position < parser->length && parser->json[parser->position] == '}') {
    token->end = (int) parser->position + 1;
    parser->position += 1;
    parser->current_parent = previous_parent;
    return index;
  }

  while (parser->position < parser->length) {
    if (parser->json[parser->position] != '"') {
      return -1;
    }
    int key_index = br_json_parse_string(parser, tokens, max_tokens);
    if (key_index < 0) {
      return -1;
    }
    token->size += 1;
    br_json_skip_ws(parser);
    if (parser->position >= parser->length || parser->json[parser->position] != ':') {
      return -1;
    }
    parser->position += 1;
    br_json_skip_ws(parser);
    int value_index = br_json_parse_value(parser, tokens, max_tokens);
    if (value_index < 0) {
      return -1;
    }
    (void) value_index;
    br_json_skip_ws(parser);
    if (parser->position >= parser->length) {
      return -1;
    }
    if (parser->json[parser->position] == ',') {
      parser->position += 1;
      br_json_skip_ws(parser);
      continue;
    }
    if (parser->json[parser->position] == '}') {
      token->end = (int) parser->position + 1;
      parser->position += 1;
      parser->current_parent = previous_parent;
      return index;
    }
    return -1;
  }

  return -1;
}

static int br_json_parse_value(br_json_parser *parser, br_json_token *tokens, int max_tokens) {
  br_json_skip_ws(parser);
  if (parser->position >= parser->length) {
    return -1;
  }

  char ch = parser->json[parser->position];
  if (ch == '{') {
    return br_json_parse_object(parser, tokens, max_tokens);
  }
  if (ch == '[') {
    return br_json_parse_array(parser, tokens, max_tokens);
  }
  if (ch == '"') {
    return br_json_parse_string(parser, tokens, max_tokens);
  }
  return br_json_parse_primitive(parser, tokens, max_tokens);
}

int br_json_parse(const char *json, size_t length, br_json_token *tokens, int max_tokens) {
  if (!json || !tokens || max_tokens <= 0) {
    return -1;
  }

  memset(tokens, 0, sizeof(br_json_token) * (size_t) max_tokens);

  br_json_parser parser;
  parser.json = json;
  parser.length = length;
  parser.position = 0;
  parser.next_token = 0;
  parser.current_parent = -1;

  int root = br_json_parse_value(&parser, tokens, max_tokens);
  if (root < 0) {
    return -1;
  }
  br_json_skip_ws(&parser);
  if (parser.position != parser.length) {
    return -1;
  }
  return parser.next_token;
}

bool br_json_token_eq(const char *json, const br_json_token *token, const char *value) {
  size_t size;
  if (!json || !token || !value || token->start < 0 || token->end < token->start) {
    return false;
  }
  size = (size_t) (token->end - token->start);
  return strlen(value) == size && strncmp(json + token->start, value, size) == 0;
}

int br_json_find_key(
  const char *json,
  const br_json_token *tokens,
  int token_count,
  int object_index,
  const char *key
) {
  if (!json || !tokens || token_count <= 0 || object_index < 0 || object_index >= token_count || !key) {
    return -1;
  }
  if (tokens[object_index].type != BR_JSON_OBJECT) {
    return -1;
  }

  for (int i = object_index + 1; i + 1 < token_count; i += 1) {
    if (tokens[i].parent != object_index) {
      continue;
    }
    if (tokens[i].type != BR_JSON_STRING) {
      continue;
    }
    if (!br_json_token_eq(json, &tokens[i], key)) {
      continue;
    }
    return i + 1;
  }
  return -1;
}

static bool br_json_append_char(char *output, size_t output_size, size_t *used, char ch) {
  if (*used + 1 >= output_size) {
    return false;
  }
  output[*used] = ch;
  *used += 1;
  output[*used] = '\0';
  return true;
}

bool br_json_token_to_string(
  const char *json,
  const br_json_token *token,
  char *output,
  size_t output_size
) {
  size_t used = 0;

  if (!json || !token || !output || output_size == 0 || token->type != BR_JSON_STRING) {
    return false;
  }

  output[0] = '\0';
  for (int i = token->start; i < token->end; i += 1) {
    char ch = json[i];
    if (ch != '\\') {
      if (!br_json_append_char(output, output_size, &used, ch)) {
        return false;
      }
      continue;
    }
    i += 1;
    if (i >= token->end) {
      return false;
    }
    ch = json[i];
    switch (ch) {
      case '"':
      case '\\':
      case '/':
        if (!br_json_append_char(output, output_size, &used, ch)) {
          return false;
        }
        break;
      case 'b':
        if (!br_json_append_char(output, output_size, &used, '\b')) {
          return false;
        }
        break;
      case 'f':
        if (!br_json_append_char(output, output_size, &used, '\f')) {
          return false;
        }
        break;
      case 'n':
        if (!br_json_append_char(output, output_size, &used, '\n')) {
          return false;
        }
        break;
      case 'r':
        if (!br_json_append_char(output, output_size, &used, '\r')) {
          return false;
        }
        break;
      case 't':
        if (!br_json_append_char(output, output_size, &used, '\t')) {
          return false;
        }
        break;
      case 'u':
        /* \uXXXX — we don't actually decode the codepoint (the firmware font cache is
           UTF-8 only), so emit a placeholder. Validate that 4 hex digits actually
           follow inside the token before skipping past them; a malformed/truncated
           `\u` near token->end would otherwise let `i` jump past the loop terminator
           and could mask a follow-up unrelated escape if token boundaries shift. */
        if (!br_json_append_char(output, output_size, &used, '?')) {
          return false;
        }
        if (i + 4 >= token->end) {
          return false; /* truncated \u escape — bail rather than skip past end */
        }
        i += 4;
        break;
      default:
        if (!br_json_append_char(output, output_size, &used, ch)) {
          return false;
        }
        break;
    }
  }
  return true;
}

bool br_json_token_to_double(const char *json, const br_json_token *token, double *value) {
  char buffer[64];
  size_t length;
  char *end = NULL;

  if (!json || !token || !value || token->end <= token->start) {
    return false;
  }
  length = (size_t) (token->end - token->start);
  if (length == 0 || length >= sizeof(buffer)) {
    return false;
  }
  memcpy(buffer, json + token->start, length);
  buffer[length] = '\0';
  *value = strtod(buffer, &end);
  return end && *end == '\0';
}

bool br_json_token_to_bool(const char *json, const br_json_token *token, bool *value) {
  if (!json || !token || !value) {
    return false;
  }
  if (br_json_token_eq(json, token, "true")) {
    *value = true;
    return true;
  }
  if (br_json_token_eq(json, token, "false")) {
    *value = false;
    return true;
  }
  return false;
}

bool br_json_copy_raw(
  const char *json,
  const br_json_token *token,
  char *output,
  size_t output_size
) {
  size_t length;

  if (!json || !token || !output || output_size == 0 || token->start < 0 || token->end < token->start) {
    return false;
  }
  length = (size_t) (token->end - token->start);
  if (length + 1 > output_size) {
    return false;
  }
  memcpy(output, json + token->start, length);
  output[length] = '\0';
  return true;
}
