#pragma once
#include <stddef.h>

typedef void (*speak_fn)(const char *sentence, void *ud);
typedef struct { double t_first_token; double t_first_speak; double t_done; } llm_lat_t;

/* POST to OpenRouter /chat/completions with stream=true.
 * Emits each complete sentence to `speak` as it arrives.
 * Returns the full reply (malloc'd; caller frees). */
char *openrouter_chat(const char *api_key, const char *model,
                      const char *persona, const char *user_msg,
                      speak_fn speak, void *speak_ud, llm_lat_t *lat);
