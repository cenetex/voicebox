/*
 * openrouter.c — bring-your-own-key LLM backend for voicebox.
 *
 * POSTs to https://openrouter.ai/api/v1/chat/completions with stream=true,
 * parses SSE deltas, and hands complete sentences to the same speak callback
 * the local llama.cpp path uses. Same persona, same telemetry shape.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <curl/curl.h>

#include "openrouter.h"

extern double now_ms(void); /* defined in voicebox.c */

typedef struct {
    /* SSE event line accumulator */
    char    buf[8192];
    size_t  buf_n;
    /* sentence-streaming state */
    char    sentence[1024];
    size_t  sentence_n;
    int     in_think;
    /* full reply (returned to caller) */
    char   *full;
    size_t  full_cap, full_n;
    /* speaker callback */
    speak_fn speak;
    void    *speak_ud;
    /* timing */
    double      t0;
    llm_lat_t  *lat;
} or_state_t;

/* JSON-string escape src into dst (to avoid pulling in cJSON). Returns bytes written. */
static size_t json_escape(char *dst, size_t cap, const char *src) {
    size_t n = 0;
    for (const char *p = src; *p && n + 8 < cap; p++) {
        switch (*p) {
            case '"':  dst[n++] = '\\'; dst[n++] = '"';  break;
            case '\\': dst[n++] = '\\'; dst[n++] = '\\'; break;
            case '\n': dst[n++] = '\\'; dst[n++] = 'n';  break;
            case '\r': /* drop */                        break;
            case '\t': dst[n++] = '\\'; dst[n++] = 't';  break;
            default:
                if ((unsigned char)*p < 0x20) {
                    n += snprintf(dst + n, cap - n, "\\u%04x", (unsigned char)*p);
                } else {
                    dst[n++] = *p;
                }
                break;
        }
    }
    dst[n] = 0;
    return n;
}

/* Extract the value of `"content":"..."` from a JSON delta line.
   Writes unescaped text into out (up to out_cap-1), returns bytes written.
   Returns -1 if no content field found. */
static int extract_content_delta(const char *json, char *out, size_t out_cap) {
    const char *p = strstr(json, "\"content\":\"");
    if (!p) return -1;
    p += 11;
    size_t n = 0;
    while (*p && *p != '"' && n + 8 < out_cap) {
        if (*p == '\\' && p[1]) {
            switch (p[1]) {
                case 'n':  out[n++] = '\n'; p += 2; break;
                case 't':  out[n++] = '\t'; p += 2; break;
                case '"':  out[n++] = '"';  p += 2; break;
                case '\\': out[n++] = '\\'; p += 2; break;
                case '/':  out[n++] = '/';  p += 2; break;
                case 'r':  /* skip */       p += 2; break;
                case 'u': {
                    if (strlen(p) < 6) { p += 2; break; }
                    unsigned int code = 0;
                    sscanf(p + 2, "%4x", &code);
                    /* trivial UTF-8 emit */
                    if (code < 0x80) {
                        out[n++] = (char)code;
                    } else if (code < 0x800) {
                        out[n++] = (char)(0xC0 | (code >> 6));
                        out[n++] = (char)(0x80 | (code & 0x3F));
                    } else {
                        out[n++] = (char)(0xE0 | (code >> 12));
                        out[n++] = (char)(0x80 | ((code >> 6) & 0x3F));
                        out[n++] = (char)(0x80 | (code & 0x3F));
                    }
                    p += 6;
                    break;
                }
                default: out[n++] = p[1]; p += 2; break;
            }
        } else {
            out[n++] = *p++;
        }
    }
    out[n] = 0;
    return (int)n;
}

/* Append a token chunk to the streaming state, emit complete sentences to speak_fn. */
static void feed_chunk(or_state_t *s, const char *chunk, size_t n) {
    if (n == 0) return;
    if (s->lat && s->lat->t_first_token < 0)
        s->lat->t_first_token = now_ms() - s->t0;

    /* echo to stdout */
    fwrite(chunk, 1, n, stdout); fflush(stdout);

    /* append to full transcript */
    if (s->full_n + n + 1 < s->full_cap) {
        memcpy(s->full + s->full_n, chunk, n);
        s->full_n += n;
        s->full[s->full_n] = 0;
    }

    /* track <think>...</think> regions */
    if (!s->in_think && strstr(s->full, "<think>") && !strstr(s->full, "</think>")) {
        s->in_think = 1;
        s->sentence_n = 0;
        return;
    }
    if (s->in_think) {
        if (strstr(s->full, "</think>")) { s->in_think = 0; s->sentence_n = 0; }
        return;
    }

    if (s->sentence_n + n < sizeof s->sentence) {
        memcpy(s->sentence + s->sentence_n, chunk, n);
        s->sentence_n += n;
    }

    /* sentence boundary: ., !, ?, or newline followed by content */
    for (size_t k = 0; k < s->sentence_n; k++) {
        char c = s->sentence[k];
        if (c == '.' || c == '!' || c == '?' || c == '\n') {
            size_t cut = k + 1;
            int alpha = 0;
            for (size_t j = 0; j < cut; j++)
                if (isalpha((unsigned char)s->sentence[j])) alpha++;
            if (alpha < 4) continue;

            char one[1024];
            size_t cn = cut < sizeof one ? cut : sizeof one - 1;
            memcpy(one, s->sentence, cn);
            one[cn] = 0;
            char *ss = one; while (*ss == ' ' || *ss == '\n' || *ss == '\t') ss++;
            if (*ss) {
                if (s->lat && s->lat->t_first_speak < 0)
                    s->lat->t_first_speak = now_ms() - s->t0;
                if (s->speak) s->speak(ss, s->speak_ud);
            }
            memmove(s->sentence, s->sentence + cut, s->sentence_n - cut);
            s->sentence_n -= cut;
            k = (size_t)-1;
        }
    }
}

/* libcurl write callback — line-buffer SSE events, parse data: lines */
static size_t on_curl_write(char *data, size_t size, size_t nm, void *ud) {
    or_state_t *s = (or_state_t *)ud;
    size_t n = size * nm;

    /* append, expand if needed (here we just truncate-and-shift if overflowing) */
    if (s->buf_n + n + 1 >= sizeof s->buf) {
        size_t shift = (s->buf_n + n + 1) - sizeof s->buf;
        if (shift > s->buf_n) shift = s->buf_n;
        memmove(s->buf, s->buf + shift, s->buf_n - shift);
        s->buf_n -= shift;
    }
    memcpy(s->buf + s->buf_n, data, n);
    s->buf_n += n;
    s->buf[s->buf_n] = 0;

    /* process complete SSE events (separated by "\n\n") */
    char *event = s->buf;
    char *sep;
    while ((sep = strstr(event, "\n\n"))) {
        *sep = 0;
        /* an event may span multiple "data: " lines but OpenRouter sends one per event */
        char *line = event;
        while (line && *line) {
            char *eol = strchr(line, '\n');
            if (eol) *eol = 0;

            if (strncmp(line, "data: ", 6) == 0) {
                const char *payload = line + 6;
                if (strncmp(payload, "[DONE]", 6) == 0) {
                    /* end of stream */
                } else {
                    char delta[2048];
                    int  dn = extract_content_delta(payload, delta, sizeof delta);
                    if (dn > 0) feed_chunk(s, delta, (size_t)dn);
                }
            } else if (strncmp(line, ": ", 2) == 0) {
                /* SSE comment / keepalive — ignore */
            }

            line = eol ? eol + 1 : NULL;
        }
        event = sep + 2;
    }
    /* shift unconsumed bytes to start */
    size_t consumed = event - s->buf;
    if (consumed > 0) {
        memmove(s->buf, event, s->buf_n - consumed);
        s->buf_n -= consumed;
        s->buf[s->buf_n] = 0;
    }
    return n;
}

char *openrouter_chat(const char *api_key, const char *model,
                      const char *persona, const char *user_msg,
                      speak_fn speak, void *speak_ud, llm_lat_t *lat)
{
    if (!api_key || !*api_key) return strdup("");
    or_state_t st = {0};
    st.full_cap = 8192;
    st.full     = calloc(st.full_cap, 1);
    st.speak    = speak;
    st.speak_ud = speak_ud;
    st.t0       = now_ms();
    st.lat      = lat;
    if (lat) { lat->t_first_token = -1; lat->t_first_speak = -1; lat->t_done = -1; }

    /* build JSON body */
    static char body[32768];
    size_t bn = 0;
    bn += snprintf(body + bn, sizeof body - bn,
                   "{\"model\":\"%s\",\"stream\":true,\"messages\":["
                   "{\"role\":\"system\",\"content\":\"", model);
    bn += json_escape(body + bn, sizeof body - bn, persona ? persona : "");
    bn += snprintf(body + bn, sizeof body - bn,
                   "\"},{\"role\":\"user\",\"content\":\"");
    bn += json_escape(body + bn, sizeof body - bn, user_msg ? user_msg : "");
    bn += snprintf(body + bn, sizeof body - bn, "\"}]}");

    CURL *curl = curl_easy_init();
    if (!curl) return st.full;

    char auth[512]; snprintf(auth, sizeof auth, "Authorization: Bearer %s", api_key);
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, auth);
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Accept: text/event-stream");
    headers = curl_slist_append(headers, "HTTP-Referer: https://github.com/cenetex/voicebox");
    headers = curl_slist_append(headers, "X-Title: voicebox");

    curl_easy_setopt(curl, CURLOPT_URL, "https://openrouter.ai/api/v1/chat/completions");
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)bn);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, on_curl_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &st);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

    CURLcode rc = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    /* flush trailing fragment as final sentence */
    if (!st.in_think && st.sentence_n > 0) {
        char one[1024];
        size_t cn = st.sentence_n < sizeof one ? st.sentence_n : sizeof one - 1;
        memcpy(one, st.sentence, cn); one[cn] = 0;
        char *s = one; while (*s == ' ' || *s == '\n' || *s == '\t') s++;
        if (*s) {
            if (st.lat && st.lat->t_first_speak < 0)
                st.lat->t_first_speak = now_ms() - st.t0;
            if (st.speak) st.speak(s, st.speak_ud);
        }
    }
    if (lat) lat->t_done = now_ms() - st.t0;
    putchar('\n');

    if (rc != CURLE_OK) {
        fprintf(stderr, "[openrouter] curl error: %s\n", curl_easy_strerror(rc));
    } else if (http_code >= 400) {
        fprintf(stderr, "[openrouter] HTTP %ld — body so far: %.200s\n",
                http_code, st.buf);
    }
    return st.full;
}
