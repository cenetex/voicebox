/*
 * voicebox — minimal local voice assistant in C
 *
 * Pipeline: mic -> VAD -> Whisper STT -> llama LLM -> Kokoro TTS -> speaker
 *
 * Two deps:
 *   sherpa-onnx  (STT + VAD + TTS, single C API)
 *   llama.cpp    (LLM, C API)
 *   miniaudio.h  (single-header audio I/O, vendored)
 *
 * Build: see Makefile
 */

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#include <sherpa-onnx/c-api/c-api.h>
#include <llama.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
#include <ctype.h>

/* -------- timing helpers -------- */
double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

#define SAMPLE_RATE   16000
#define CHANNELS      1
#define MAX_PROMPT    4096
#define MAX_REPLY     512

/* -------- lock-free SPSC ring (audio cb = producer, main = consumer) -------- */
typedef struct {
    float           *buf;
    size_t           cap;     /* power of two */
    size_t           mask;
    _Atomic size_t   head;    /* write index (producer) */
    _Atomic size_t   tail;    /* read  index (consumer) */
} ring_t;

static ring_t g_ring;

static size_t next_pow2(size_t n) { size_t p = 1; while (p < n) p <<= 1; return p; }

static void ring_init(ring_t *r, size_t want) {
    size_t cap = next_pow2(want);
    r->buf = calloc(cap, sizeof(float));
    r->cap = cap; r->mask = cap - 1;
    atomic_store(&r->head, 0);
    atomic_store(&r->tail, 0);
}
/* called from realtime audio thread — no locks, no allocations */
static void ring_push(ring_t *r, const float *s, size_t n) {
    size_t h = atomic_load_explicit(&r->head, memory_order_relaxed);
    size_t t = atomic_load_explicit(&r->tail, memory_order_acquire);
    for (size_t i = 0; i < n; i++) {
        r->buf[h & r->mask] = s[i];
        h++;
        if (h - t > r->cap) { /* full — drop oldest */
            t = h - r->cap;
        }
    }
    atomic_store_explicit(&r->tail, t, memory_order_release);
    atomic_store_explicit(&r->head, h, memory_order_release);
}
static size_t ring_pop(ring_t *r, float *out, size_t want) {
    size_t h = atomic_load_explicit(&r->head, memory_order_acquire);
    size_t t = atomic_load_explicit(&r->tail, memory_order_relaxed);
    size_t got = 0;
    while (got < want && t < h) {
        out[got++] = r->buf[t & r->mask];
        t++;
    }
    atomic_store_explicit(&r->tail, t, memory_order_release);
    return got;
}

/* -------- miniaudio mic callback (native 48 kHz → 16 kHz, 3:1 box) -------- */
static int g_capture_calls = 0;
static int g_dec_phase = 0;
static float g_dec_acc = 0.f;
static _Atomic int g_speaking = 0; /* 1 while TTS plays — suppresses self-hearing */
static _Atomic int g_abort    = 0; /* set when barge-in detected; play_pcm + worker drain on it */
#define BARGE_PEAK_THRESHOLD 0.15f
static void on_capture(ma_device *dev, void *out, const void *in, ma_uint32 frames) {
    (void)dev; (void)out;
    g_capture_calls++;
    if ((g_capture_calls % 100) == 1)
        fprintf(stderr, "[cap] call=%d frames=%u\n", g_capture_calls, frames);
    if (atomic_load_explicit(&g_speaking, memory_order_acquire)) {
        /* While TTS is playing, don't feed VAD (would self-trigger),
           but watch for a loud incoming voice — likely the captain interrupting. */
        const float *src = (const float *)in;
        float peak = 0;
        for (ma_uint32 i = 0; i < frames; i++) {
            float a = src[i] < 0 ? -src[i] : src[i];
            if (a > peak) peak = a;
        }
        if (peak > BARGE_PEAK_THRESHOLD)
            atomic_store_explicit(&g_abort, 1, memory_order_release);
        return;
    }

    const float *src = (const float *)in;
    float        buf[2048];
    size_t       n = 0;
    for (ma_uint32 i = 0; i < frames; i++) {
        g_dec_acc += src[i];
        if (++g_dec_phase == 3) {
            buf[n++] = g_dec_acc / 3.0f;
            g_dec_acc = 0.f;
            g_dec_phase = 0;
            if (n == 2048) { ring_push(&g_ring, buf, n); n = 0; }
        }
    }
    if (n) ring_push(&g_ring, buf, n);
}

/* -------- play PCM through miniaudio -------- */
typedef struct { const float *s; int n; int pos; } play_state_t;

static void on_playback(ma_device *d, void *o, const void *i, ma_uint32 f) {
    (void)i;
    play_state_t *s = (play_state_t *)d->pUserData;
    float *out = (float *)o;
    for (ma_uint32 k = 0; k < f; k++)
        out[k] = (s->pos < s->n) ? s->s[s->pos++] : 0.f;
}

static void play_pcm(const float *samples, int n, int sr) {
    play_state_t st = { samples, n, 0 };
    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format   = ma_format_f32;
    cfg.playback.channels = 1;
    cfg.sampleRate        = sr;
    cfg.pUserData         = &st;
    cfg.dataCallback      = on_playback;

    ma_device dev;
    if (ma_device_init(NULL, &cfg, &dev) != MA_SUCCESS) return;
    ma_device_start(&dev);
    while (st.pos < st.n && !atomic_load_explicit(&g_abort, memory_order_acquire))
        ma_sleep(20);
    if (!atomic_load_explicit(&g_abort, memory_order_acquire))
        ma_sleep(150); /* let speakers drain */
    ma_device_uninit(&dev);
}

/* -------- LLM helpers -------- */
typedef struct {
    struct llama_model    *model;
    struct llama_context  *ctx;
    const struct llama_vocab *vocab;
    int                       n_sys;     /* tokens in the cached system prefix */
    int                       sys_warmed;
    const char               *warmed_for; /* persona prompt pointer last warmed */
} llm_t;

static bool llm_init(llm_t *l, const char *path) {
    llama_backend_init();
    struct llama_model_params mp = llama_model_default_params();
    mp.n_gpu_layers = 0; /* force CPU — Metal init disrupts CoreAudio capture */
    l->model = llama_model_load_from_file(path, mp);
    if (!l->model) return false;
    l->vocab = llama_model_get_vocab(l->model);

    struct llama_context_params cp = llama_context_default_params();
    cp.n_ctx = 2048; cp.n_batch = 512;
    l->ctx = llama_init_from_model(l->model, cp);
    return l->ctx != NULL;
}

static void llm_free(llm_t *l) {
    if (l->ctx)   llama_free(l->ctx);
    if (l->model) llama_model_free(l->model);
    llama_backend_free();
}

/* Decode the system prompt once and leave it warm in the KV cache.
   Per-turn llm_chat will then only decode the user/assistant chunk on top of it. */
static int llm_warm_persona(llm_t *l, const char *persona) {
    char sys[MAX_PROMPT];
    int n = snprintf(sys, sizeof sys,
                     "<|im_start|>system\n%s<|im_end|>\n", persona);
    int n_tok = -llama_tokenize(l->vocab, sys, n, NULL, 0, true, true);
    if (n_tok <= 0) return -1;
    llama_token *toks = malloc(n_tok * sizeof *toks);
    llama_tokenize(l->vocab, sys, n, toks, n_tok, true, true);

    /* fresh cache, then decode the system block */
    llama_memory_clear(llama_get_memory(l->ctx), true);
    struct llama_batch batch = llama_batch_get_one(toks, n_tok);
    int rc = llama_decode(l->ctx, batch);
    free(toks);
    if (rc != 0) return -1;
    l->n_sys = n_tok;
    l->sys_warmed = 1;
    l->warmed_for = persona;
    fprintf(stderr, "[llm] system prompt warmed: %d tokens cached\n", n_tok);
    return 0;
}

/* -------- persona registry (multi-speaker support) -------- */
#define MAX_PERSONAS 16
typedef struct {
    char   name[32];
    char  *prompt;        /* malloc'd */
    int    voice;
    float  speed;
    char   fx[32];
} persona_t;

static persona_t g_personas[MAX_PERSONAS];
static int       g_n_personas      = 0;
static int       g_default_persona = 0; /* index used when no name provided */

/* Live globals — point at the active persona for the current synth call.
   Synthesis reads these; the speaker worker sets them before each Kokoro call. */
static const char *g_persona =
    "You are a terse, friendly local voice assistant. Reply in 1-2 short sentences. /no_think";
static int   g_voice = 17;
static float g_tts_speed = 1.05f;
static char  g_fx[32] = "none";

static const char *g_say_text = NULL; /* one-shot say-mode bypasses LLM/STT/mic */

static int persona_find(const char *name) {
    for (int i = 0; i < g_n_personas; i++)
        if (strcmp(g_personas[i].name, name) == 0) return i;
    return -1;
}

static int persona_load_into(persona_t *out, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char  line[1024]; int in_prompt = 0;
    char *prompt = calloc(8192, 1); size_t pn = 0;
    out->voice = 17; out->speed = 1.0f;
    snprintf(out->fx, sizeof out->fx, "none");
    while (fgets(line, sizeof line, f)) {
        if (in_prompt) {
            size_t l = strlen(line);
            if (pn + l < 8192) { memcpy(prompt + pn, line, l); pn += l; }
            continue;
        }
        if (line[0] == '#' || line[0] == '\n') continue;
        if (strncmp(line, "---", 3) == 0)             { in_prompt = 1; continue; }
        if (strncmp(line, "voice=", 6) == 0)          { out->voice = atoi(line + 6); continue; }
        if (strncmp(line, "speed=", 6) == 0)          { out->speed = strtof(line + 6, NULL); continue; }
        if (strncmp(line, "fx=",    3) == 0) {
            size_t l = strlen(line + 3);
            while (l && (line[3 + l - 1] == '\n' || line[3 + l - 1] == ' ')) l--;
            if (l >= sizeof out->fx) l = sizeof out->fx - 1;
            memcpy(out->fx, line + 3, l); out->fx[l] = 0; continue;
        }
    }
    fclose(f);
    while (pn > 0 && (prompt[pn-1] == '\n' || prompt[pn-1] == ' ')) prompt[--pn] = 0;
    if (pn == 0) { free(prompt); return -1; }
    out->prompt = prompt;
    return 0;
}

static int persona_register(const char *name, const char *file) {
    if (g_n_personas >= MAX_PERSONAS) { fprintf(stderr, "[persona] registry full\n"); return -1; }
    persona_t *p = &g_personas[g_n_personas];
    if (persona_load_into(p, file) != 0) { fprintf(stderr, "[persona] load failed: %s\n", file); return -1; }
    snprintf(p->name, sizeof p->name, "%s", name);
    fprintf(stderr, "[persona] registered \"%s\" (voice=%d speed=%.2f fx=%s)\n",
            p->name, p->voice, p->speed, p->fx);
    return g_n_personas++;
}

/* Point live globals at persona[idx] for the upcoming synth + LLM call. */
static void persona_activate(int idx) {
    if (idx < 0 || idx >= g_n_personas) return;
    persona_t *p = &g_personas[idx];
    g_persona    = p->prompt;
    g_voice      = p->voice;
    g_tts_speed  = p->speed;
    snprintf(g_fx, sizeof g_fx, "%s", p->fx);
}

/* wake word — if set, pilot speech only triggers a turn when transcript contains
   this substring (case-insensitive), OR if we're still in the post-turn follow-up
   window (g_active_until_ms). */
static const char *g_wake_word = NULL;
static const char *g_sleep_word = NULL;       /* explicit "over" phrase to end the session */
static const char *g_or_key   = NULL;          /* OpenRouter API key (BYO-LLM) */
static const char *g_or_model = "openai/gpt-oss-20b:free";
static double      g_active_until_ms = 0.0;
#define WAKE_FOLLOWUP_MS 30000.0

static int contains_ci(const char *hay, const char *needle); /* fwd */

/* check if `text` contains any comma-separated alternate from `pattern` */
static int wake_match(const char *text, const char *pattern) {
    if (!pattern || !*pattern || !text) return 0;
    char buf[256]; snprintf(buf, sizeof buf, "%s", pattern);
    char *save = NULL;
    for (char *tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        while (*tok == ' ') tok++;
        if (*tok && contains_ci(text, tok)) return 1;
    }
    return 0;
}

static int contains_ci(const char *hay, const char *needle) {
    if (!hay || !needle || !*needle) return 0;
    for (const char *p = hay; *p; p++) {
        const char *a = p, *b = needle;
        while (*a && *b && tolower((unsigned char)*a) == tolower((unsigned char)*b)) { a++; b++; }
        if (!*b) return 1;
    }
    return 0;
}

/* Convenience wrapper for `--persona <file>` — registers the file under the
   given name (or "default") and returns the persona index. */
static int load_persona(const char *path) {
    int idx = persona_register("default", path);
    if (idx < 0) return -1;
    g_default_persona = idx;
    persona_activate(idx);
    return 0;
}

static const char *NAV7_PERSONA =
    "You are NAV-7, the onboard signal-relay AI of an independent miner working out of Sector One. "
    "You are calm, terse, mildly sardonic, loyal to your captain. You speak in plain prose, "
    "1 short sentence by default, never more than 2. You speak as if over the intercom. "
    "Never narrate actions, never describe yourself, never use markdown or asterisks. "
    "If [SHIP TELEMETRY] is provided, ground answers in it; if a value is missing, say so plainly.\n"
    "\n"
    "When given a [STAGE DIRECTION], you must rephrase it as NAV-7 speaking to the captain. "
    "Never quote the directive back. Examples:\n"
    "  Directive: 'Tell the captain we are docking at Hephaestus.'\n"
    "  GOOD:  Docking with Hephaestus now, Captain.\n"
    "  WRONG: Tell the captain we are docking at Hephaestus.\n"
    "\n"
    "  Directive: 'Confirm whether to accept contract C-218.'\n"
    "  GOOD:  Captain, contract C-218 is up — accept or pass?\n"
    "  WRONG: Confirm whether to accept contract C-218.\n"
    "/no_think";

/* -------- generic ship state (whatever the host game sends via STATE line) -------- */
static pthread_mutex_t g_state_mu = PTHREAD_MUTEX_INITIALIZER;
static char            g_state[2048] = "";

static void state_set(const char *s) {
    pthread_mutex_lock(&g_state_mu);
    snprintf(g_state, sizeof g_state, "%s", s);
    pthread_mutex_unlock(&g_state_mu);
}
static void state_get(char *out, size_t n) {
    pthread_mutex_lock(&g_state_mu);
    snprintf(out, n, "%s", g_state);
    pthread_mutex_unlock(&g_state_mu);
}

/* event queue — outbound utterances from the host game */
typedef struct { char *text; int via_llm; int persona_idx; } event_t;
#define EVT_Q_CAP 32
static event_t      g_evt_q[EVT_Q_CAP];
static _Atomic int  g_evt_head = 0;
static _Atomic int  g_evt_tail = 0;

static void evt_push_p(const char *s, int via_llm, int persona_idx) {
    int h = atomic_load(&g_evt_head), t = atomic_load(&g_evt_tail);
    if (h - t >= EVT_Q_CAP) return;
    g_evt_q[h % EVT_Q_CAP].text        = strdup(s);
    g_evt_q[h % EVT_Q_CAP].via_llm     = via_llm;
    g_evt_q[h % EVT_Q_CAP].persona_idx = persona_idx;
    atomic_store(&g_evt_head, h + 1);
}
static int evt_pop(event_t *out) {
    int h = atomic_load(&g_evt_head), t = atomic_load(&g_evt_tail);
    if (t >= h) return 0;
    *out = g_evt_q[t % EVT_Q_CAP];
    atomic_store(&g_evt_tail, t + 1);
    return 1;
}

/* If the first whitespace-delimited token of `arg` matches a registered persona
   name, return its index and advance *rest past the name. Otherwise return -1
   and leave *rest pointing at the original arg. */
static int parse_persona_prefix(const char *arg, const char **rest) {
    if (!arg) { *rest = ""; return -1; }
    while (*arg == ' ') arg++;
    const char *space = strchr(arg, ' ');
    if (!space) { *rest = arg; return -1; }
    char tok[32];
    size_t n = (size_t)(space - arg);
    if (n >= sizeof tok) { *rest = arg; return -1; }
    memcpy(tok, arg, n); tok[n] = 0;
    int idx = persona_find(tok);
    if (idx < 0) { *rest = arg; return -1; }
    *rest = space + 1;
    while (**rest == ' ') (*rest)++;
    return idx;
}

/* line-protocol stdin reader — the same protocol Signal would speak as parent */
static void *stdin_thread(void *arg) {
    (void)arg;
    char line[4096];
    while (fgets(line, sizeof line, stdin)) {
        size_t n = strlen(line);
        while (n && (line[n-1] == '\n' || line[n-1] == '\r')) line[--n] = 0;
        if (n == 0) continue;

        if      (strncmp(line, "STATE ", 6) == 0) {
            state_set(line + 6);
            fprintf(stderr, "[state] %s\n", line + 6);
        }
        else if (strncmp(line, "PERSONA ", 8) == 0) {
            /* PERSONA <name> <file> */
            char *name = line + 8;
            while (*name == ' ') name++;
            char *sp = strchr(name, ' ');
            if (!sp) { fprintf(stderr, "[?] PERSONA <name> <file>\n"); continue; }
            *sp = 0;
            char *file = sp + 1; while (*file == ' ') file++;
            persona_register(name, file);
        }
        else if (strncmp(line, "EVENT ", 6) == 0) {
            const char *rest;
            int p_idx = parse_persona_prefix(line + 6, &rest);
            if (p_idx < 0) p_idx = g_default_persona;
            evt_push_p(rest, 0, p_idx);
            fprintf(stderr, "[evt:tts] persona=%s text=%s\n",
                    g_n_personas ? g_personas[p_idx].name : "default", rest);
        }
        else if (strncmp(line, "ASK ",   4) == 0) {
            const char *rest;
            int p_idx = parse_persona_prefix(line + 4, &rest);
            if (p_idx < 0) p_idx = g_default_persona;
            evt_push_p(rest, 1, p_idx);
            fprintf(stderr, "[evt:llm] persona=%s directive=%s\n",
                    g_n_personas ? g_personas[p_idx].name : "default", rest);
        }
        else if (strncmp(line, "QUIT",   4) == 0) { exit(0); }
        else                                      { fprintf(stderr, "[?] unknown: %s\n", line); }
    }
    return NULL;
}

#include "openrouter.h" /* speak_fn + llm_lat_t live here */
#include "fx.h"

/* concrete speaker bound to a sherpa-onnx TTS + sample rate, plus a worker thread */
typedef struct { char *text; int persona_idx; } spk_job_t;

typedef struct {
    const SherpaOnnxOfflineTts *tts;
    int                         sr;
    /* job queue */
    spk_job_t                   q[16];
    _Atomic int                 q_head;
    _Atomic int                 q_tail;
    pthread_mutex_t             mu;
    pthread_cond_t              cv;
    pthread_t                   thread;
} speak_ctx_t;

static void *speaker_worker(void *arg) {
    speak_ctx_t *c = (speak_ctx_t *)arg;
    for (;;) {
        pthread_mutex_lock(&c->mu);
        while (atomic_load(&c->q_head) == atomic_load(&c->q_tail))
            pthread_cond_wait(&c->cv, &c->mu);
        int t = atomic_load(&c->q_tail);
        spk_job_t job = c->q[t % 16];
        atomic_store(&c->q_tail, t + 1);
        pthread_mutex_unlock(&c->mu);

        /* per-utterance persona: snapshot voice/speed/fx so different speakers can interleave */
        int   voice = g_voice;
        float speed = g_tts_speed;
        char  fx[32]; snprintf(fx, sizeof fx, "%s", g_fx);
        if (job.persona_idx >= 0 && job.persona_idx < g_n_personas) {
            persona_t *p = &g_personas[job.persona_idx];
            voice = p->voice; speed = p->speed;
            snprintf(fx, sizeof fx, "%s", p->fx);
        }

        const SherpaOnnxGeneratedAudio *au =
            SherpaOnnxOfflineTtsGenerate(c->tts, job.text, voice, speed);
        if (au && au->n > 0) {
            float *buf = malloc(au->n * sizeof *buf);
            memcpy(buf, au->samples, au->n * sizeof *buf);
            fx_apply(fx, buf, au->n, c->sr);
            play_pcm(buf, au->n, c->sr);
            free(buf);
        }
        SherpaOnnxDestroyOfflineTtsGeneratedAudio(au);
        free(job.text);

        /* if barge-in fired, drain the rest of the queue */
        if (atomic_load(&g_abort)) {
            pthread_mutex_lock(&c->mu);
            int h = atomic_load(&c->q_head);
            int t = atomic_load(&c->q_tail);
            int dropped = h - t;
            while (t < h) { free(c->q[t % 16].text); t++; }
            atomic_store(&c->q_tail, h);
            pthread_mutex_unlock(&c->mu);
            atomic_store(&g_abort, 0);
            if (dropped > 0)
                fprintf(stderr, "[barge] cut TTS, dropped %d pending\n", dropped);
            else
                fprintf(stderr, "[barge] cut TTS\n");
        }

        /* mute mic while there's still pending audio */
        if (atomic_load(&c->q_head) == atomic_load(&c->q_tail))
            atomic_store(&g_speaking, 0);
    }
    return NULL;
}

static void speak_init(speak_ctx_t *c, const SherpaOnnxOfflineTts *tts, int sr) {
    c->tts = tts; c->sr = sr;
    atomic_store(&c->q_head, 0);
    atomic_store(&c->q_tail, 0);
    pthread_mutex_init(&c->mu, NULL);
    pthread_cond_init(&c->cv, NULL);
    pthread_create(&c->thread, NULL, speaker_worker, c);
    pthread_detach(c->thread);
}

/* persona to tag the next enqueue with — set by main loop before each LLM call
   or direct enqueue, read by speak_sentence. */
static _Atomic int g_current_speaker = 0;

/* Non-blocking: enqueue a sentence and return immediately. */
static void speak_sentence(const char *s, void *ud) {
    speak_ctx_t *c = (speak_ctx_t *)ud;
    pthread_mutex_lock(&c->mu);
    int h = atomic_load(&c->q_head);
    int t = atomic_load(&c->q_tail);
    if (h - t >= 16) { pthread_mutex_unlock(&c->mu); return; } /* drop on overflow */
    c->q[h % 16].text        = strdup(s);
    c->q[h % 16].persona_idx = atomic_load(&g_current_speaker);
    atomic_store(&c->q_head, h + 1);
    atomic_store(&g_speaking, 1);
    pthread_cond_signal(&c->cv);
    pthread_mutex_unlock(&c->mu);
}

/* llm_lat_t now in openrouter.h */

/* generate a reply with sentence-level streaming.
   Backend: OpenRouter if OPENROUTER_API_KEY is set, else local llama.cpp.
   - emits each complete sentence to `speak` as soon as it's available
   - returns the full reply (malloc'd)
   - skips content inside <think>...</think> blocks
   - times t_first_token and t_first_speak relative to the call start  */
static char *llm_chat(llm_t *l, const char *user_msg, speak_fn speak, void *ud, llm_lat_t *lat) {
    /* OpenRouter backend short-circuit */
    if (g_or_key && *g_or_key) {
        return openrouter_chat(g_or_key, g_or_model, g_persona, user_msg,
                               speak, ud, lat);
    }

    double t0 = now_ms();
    if (lat) { lat->t_first_token = -1; lat->t_first_speak = -1; lat->t_done = -1; }

    /* persona switched? re-warm KV with the new system prompt. */
    if (l->sys_warmed && l->warmed_for != g_persona) {
        fprintf(stderr, "[llm] persona switch — re-warming KV\n");
        llm_warm_persona(l, g_persona);
    }

    char prompt[MAX_PROMPT];
    int  n;
    if (l->sys_warmed) {
        /* drop everything past the cached system prefix; tokenize only the new turn */
        llama_memory_seq_rm(llama_get_memory(l->ctx), 0, l->n_sys, -1);
        n = snprintf(prompt, sizeof prompt,
            "<|im_start|>user\n%s<|im_end|>\n"
            "<|im_start|>assistant\n", user_msg);
    } else {
        llama_memory_clear(llama_get_memory(l->ctx), true);
        n = snprintf(prompt, sizeof prompt,
            "<|im_start|>system\n%s<|im_end|>\n"
            "<|im_start|>user\n%s<|im_end|>\n"
            "<|im_start|>assistant\n", g_persona, user_msg);
    }

    int n_prompt = -llama_tokenize(l->vocab, prompt, n, NULL, 0,
                                   /*add_bos=*/!l->sys_warmed, /*parse_special=*/true);
    llama_token *tokens = malloc(n_prompt * sizeof *tokens);
    llama_tokenize(l->vocab, prompt, n, tokens, n_prompt,
                   /*add_bos=*/!l->sys_warmed, /*parse_special=*/true);

    struct llama_batch batch = llama_batch_get_one(tokens, n_prompt);
    if (llama_decode(l->ctx, batch) != 0) { free(tokens); return strdup(""); }

    struct llama_sampler *smpl = llama_sampler_chain_init(llama_sampler_chain_default_params());
    llama_sampler_chain_add(smpl, llama_sampler_init_top_k(40));
    llama_sampler_chain_add(smpl, llama_sampler_init_top_p(0.9f, 1));
    llama_sampler_chain_add(smpl, llama_sampler_init_temp(0.7f));
    llama_sampler_chain_add(smpl, llama_sampler_init_dist(LLAMA_DEFAULT_SEED));

    char  *full   = calloc(MAX_REPLY * 4, 1);
    size_t full_n = 0;
    char   pend[1024]; size_t pend_n = 0; /* sentence buffer for streaming TTS */
    int    in_think = 0;

    for (int i = 0; i < MAX_REPLY; i++) {
        llama_token tk = llama_sampler_sample(smpl, l->ctx, -1);
        if (llama_vocab_is_eog(l->vocab, tk)) break;
        char piece[256];
        int pn = llama_token_to_piece(l->vocab, tk, piece, sizeof piece, 0, true);
        if (pn <= 0) goto next;
        if (lat && lat->t_first_token < 0) lat->t_first_token = now_ms() - t0;

        /* stash full transcript */
        if (full_n + pn < MAX_REPLY * 4) { memcpy(full + full_n, piece, pn); full_n += pn; }
        fwrite(piece, 1, pn, stdout); fflush(stdout);

        /* track <think>...</think> regions and exclude them from spoken stream */
        if (!in_think && strstr(full, "<think>")) {
            char *t_close = strstr(full, "</think>");
            if (!t_close) {
                /* opener seen, not yet closed — purge anything in pend that came after the opener */
                pend_n = 0; in_think = 1;
                goto next;
            }
        }
        if (in_think) {
            if (strstr(full, "</think>")) { in_think = 0; pend_n = 0; }
            goto next;
        }

        if (pend_n + pn < sizeof pend) { memcpy(pend + pend_n, piece, pn); pend_n += pn; }

        /* sentence boundary detector: . ! ? followed by space or newline (or end) */
        for (size_t k = 0; k < pend_n; k++) {
            char c = pend[k];
            if (c == '.' || c == '!' || c == '?' || c == '\n') {
                size_t cut = k + 1;
                /* require at least 6 chars of content to avoid speaking "1." or stray "." */
                size_t alpha = 0; for (size_t j = 0; j < cut; j++) if (isalpha((unsigned char)pend[j])) alpha++;
                if (alpha < 4) continue;
                char one[1024];
                size_t n = cut < sizeof one ? cut : sizeof one - 1;
                memcpy(one, pend, n); one[n] = 0;
                /* trim leading whitespace on what we'll speak */
                char *s = one; while (*s == ' ' || *s == '\n' || *s == '\t') s++;
                if (*s) {
                    if (lat && lat->t_first_speak < 0) lat->t_first_speak = now_ms() - t0;
                    if (speak) speak(s, ud);
                }
                /* shift pend */
                memmove(pend, pend + cut, pend_n - cut);
                pend_n -= cut;
                k = (size_t)-1; /* restart scan */
            }
        }

next:
        batch = llama_batch_get_one(&tk, 1);
        if (llama_decode(l->ctx, batch) != 0) break;
    }
    /* flush trailing fragment */
    if (!in_think && pend_n > 0) {
        char one[1024];
        size_t n = pend_n < sizeof one ? pend_n : sizeof one - 1;
        memcpy(one, pend, n); one[n] = 0;
        char *s = one; while (*s == ' ' || *s == '\n' || *s == '\t') s++;
        if (*s) {
            if (lat && lat->t_first_speak < 0) lat->t_first_speak = now_ms() - t0;
            if (speak) speak(s, ud);
        }
    }
    if (lat) lat->t_done = now_ms() - t0;
    putchar('\n');
    llama_sampler_free(smpl);
    free(tokens);
    return full;
}

/* -------- main loop -------- */
int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    int  ship_mode = 0;
    const char *persona_file = NULL;
    int  arg_off = 1;
    while (arg_off < argc) {
        if      (strcmp(argv[arg_off], "--ship") == 0)    { ship_mode = 1; arg_off++; }
        else if (strcmp(argv[arg_off], "--persona") == 0 && arg_off + 1 < argc)
                                                          { persona_file = argv[arg_off + 1]; arg_off += 2; }
        else if (strcmp(argv[arg_off], "--persona-add") == 0 && arg_off + 2 < argc)
                                                          { persona_register(argv[arg_off + 1], argv[arg_off + 2]); arg_off += 3; }
        else if (strcmp(argv[arg_off], "--wake") == 0 && arg_off + 1 < argc)
                                                          { g_wake_word = argv[arg_off + 1]; arg_off += 2; }
        else if (strcmp(argv[arg_off], "--sleep") == 0 && arg_off + 1 < argc)
                                                          { g_sleep_word = argv[arg_off + 1]; arg_off += 2; }
        else if (strcmp(argv[arg_off], "--model") == 0 && arg_off + 1 < argc)
                                                          { g_or_model = argv[arg_off + 1]; arg_off += 2; }
        else if (strcmp(argv[arg_off], "--fx") == 0 && arg_off + 1 < argc)
                                                          { snprintf(g_fx, sizeof g_fx, "%s", argv[arg_off + 1]); arg_off += 2; }
        else if (strcmp(argv[arg_off], "--voice") == 0 && arg_off + 1 < argc)
                                                          { g_voice = atoi(argv[arg_off + 1]); arg_off += 2; }
        else if (strcmp(argv[arg_off], "--say") == 0 && arg_off + 1 < argc)
                                                          { g_say_text = argv[arg_off + 1]; arg_off += 2; }
        else break;
    }
    /* OpenRouter key from env var if not supplied on cmdline */
    g_or_key = getenv("OPENROUTER_API_KEY");
    if (g_or_key && *g_or_key)
        fprintf(stderr, "[llm] OpenRouter backend enabled, model=%s\n", g_or_model);
    else {
        /* dummy block to keep brace balance */
    }
    if (argc - arg_off < 3) {
        fprintf(stderr,
            "usage: %s [--ship] [--persona <file>] <whisper-dir> <kokoro-dir> <llm.gguf>\n"
            "\n"
            "  --ship                ship-AI mode (stdin line protocol):\n"
            "                          STATE <text>   replace ship telemetry\n"
            "                          EVENT <line>   speak verbatim via TTS\n"
            "                          ASK   <text>   LLM paraphrases with state\n"
            "                          QUIT\n"
            "  --persona <file>      load persona from file as the default speaker.\n"
            "                        Format:  voice=<int>\\n speed=<float>\\n fx=<name>\\n\n"
            "                                 ---\\n <system prompt body>\n"
            "  --persona-add <name> <file>\n"
            "                        register additional persona by name. Repeatable.\n"
            "                        Stations/NPCs use these via SAY/ASK <name>.\n"
            "  --wake <alt,alt,...>  comma-separated wake-word alternates (case-insensitive).\n"
            "                        Idle until heard. Stays active 30 s after each turn.\n"
            "  --sleep <alt,alt,...> comma-separated close phrases — match while active to\n"
            "                        immediately go idle (e.g. \"nav-seven over,over and out\").\n"
            "  --model <id>          OpenRouter model id when OPENROUTER_API_KEY is set\n"
            "                        (default: openai/gpt-oss-20b:free)\n"
            "\n"
            "  Set OPENROUTER_API_KEY in env to use OpenRouter as the LLM backend\n"
            "  instead of the local llama.cpp model.\n",
            argv[0]);
        return 1;
    }
    const char *whisper_dir = argv[arg_off];
    const char *kokoro_dir  = argv[arg_off + 1];
    const char *llm_path    = argv[arg_off + 2];

    if (ship_mode) g_persona = NAV7_PERSONA;
    if (persona_file && load_persona(persona_file) != 0) return 1;

    /* ---- one-shot say mode: skip LLM and STT/VAD entirely ---- */
    if (g_say_text) {
        char kmodel[512], kvoices[512], ktokens[512], kdata[512];
        snprintf(kmodel,  sizeof kmodel,  "%s/model.onnx",   kokoro_dir);
        snprintf(kvoices, sizeof kvoices, "%s/voices.bin",   kokoro_dir);
        snprintf(ktokens, sizeof ktokens, "%s/tokens.txt",   kokoro_dir);
        snprintf(kdata,   sizeof kdata,   "%s/espeak-ng-data", kokoro_dir);
        SherpaOnnxOfflineTtsConfig tts_cfg = {0};
        tts_cfg.model.kokoro.model    = kmodel;
        tts_cfg.model.kokoro.voices   = kvoices;
        tts_cfg.model.kokoro.tokens   = ktokens;
        tts_cfg.model.kokoro.data_dir = kdata;
        static char klex[1024], kdict[512];
        snprintf(klex,  sizeof klex,  "%s/lexicon-us-en.txt,%s/lexicon-gb-en.txt", kokoro_dir, kokoro_dir);
        snprintf(kdict, sizeof kdict, "%s/dict", kokoro_dir);
        tts_cfg.model.kokoro.lexicon  = klex;
        tts_cfg.model.kokoro.dict_dir = kdict;
        tts_cfg.model.kokoro.lang     = "en-us";
        tts_cfg.model.num_threads     = 2;
        tts_cfg.model.provider        = "cpu";
        const SherpaOnnxOfflineTts *tts = SherpaOnnxCreateOfflineTts(&tts_cfg);
        if (!tts) { fprintf(stderr, "kokoro init failed\n"); return 1; }
        int sr = SherpaOnnxOfflineTtsSampleRate(tts);
        fprintf(stderr, "[say] voice=%d fx=%s text=%s\n", g_voice, g_fx, g_say_text);
        const SherpaOnnxGeneratedAudio *au =
            SherpaOnnxOfflineTtsGenerate(tts, g_say_text, g_voice, g_tts_speed);
        if (au && au->n > 0) {
            float *buf = malloc(au->n * sizeof *buf);
            memcpy(buf, au->samples, au->n * sizeof *buf);
            fx_apply(g_fx, buf, au->n, sr);
            play_pcm(buf, au->n, sr);
            free(buf);
        }
        SherpaOnnxDestroyOfflineTtsGeneratedAudio(au);
        SherpaOnnxDestroyOfflineTts(tts);
        return 0;
    }

    /* ---- load LLM ---- */
    llm_t llm = {0};
    if (!llm_init(&llm, llm_path)) { fprintf(stderr, "llm load failed\n"); return 1; }

    /* warm the persona — system prompt is cached in KV across all turns */
    if (llm_warm_persona(&llm, g_persona) != 0) {
        fprintf(stderr, "[llm] persona warmup failed; falling back to per-turn re-decode\n");
    }

    /* ---- load Whisper recognizer (sherpa-onnx) ---- */
    char enc[512], dec[512], tok[512];
    snprintf(enc, sizeof enc, "%s/whisper-encoder.onnx", whisper_dir);
    snprintf(dec, sizeof dec, "%s/whisper-decoder.onnx", whisper_dir);
    snprintf(tok, sizeof tok, "%s/whisper-tokens.txt",  whisper_dir);

    SherpaOnnxOfflineRecognizerConfig rec_cfg = {0};
    rec_cfg.model_config.whisper.encoder = enc;
    rec_cfg.model_config.whisper.decoder = dec;
    rec_cfg.model_config.tokens          = tok;
    rec_cfg.model_config.num_threads     = 2;
    /* CoreML EP for Whisper requires a precompiled .mlmodelc (which onnxruntime
       doesn't auto-generate) — stays on CPU until we add the compile step. */
    rec_cfg.model_config.provider        = "cpu";
    rec_cfg.decoding_method              = "greedy_search";

    const SherpaOnnxOfflineRecognizer *rec = SherpaOnnxCreateOfflineRecognizer(&rec_cfg);
    if (!rec) { fprintf(stderr, "whisper init failed\n"); return 1; }

    /* ---- load VAD (Silero) ---- */
    SherpaOnnxVadModelConfig vad_cfg = {0};
    char vad_path[512];
    snprintf(vad_path, sizeof vad_path, "%s/silero_vad.onnx", whisper_dir);
    vad_cfg.silero_vad.model               = vad_path;
    vad_cfg.silero_vad.threshold           = 0.5f;
    vad_cfg.silero_vad.min_silence_duration = 0.5f;
    vad_cfg.silero_vad.min_speech_duration  = 0.25f;
    vad_cfg.sample_rate = SAMPLE_RATE;
    vad_cfg.num_threads = 1;
    vad_cfg.provider    = "cpu";
    SherpaOnnxVoiceActivityDetector *vad =
        SherpaOnnxCreateVoiceActivityDetector(&vad_cfg, 30 /* buffer secs */);

    /* ---- load Kokoro TTS ---- */
    char kmodel[512], kvoices[512], ktokens[512], kdata[512];
    snprintf(kmodel,  sizeof kmodel,  "%s/model.onnx",   kokoro_dir);
    snprintf(kvoices, sizeof kvoices, "%s/voices.bin",   kokoro_dir);
    snprintf(ktokens, sizeof ktokens, "%s/tokens.txt",   kokoro_dir);
    snprintf(kdata,   sizeof kdata,   "%s/espeak-ng-data", kokoro_dir);

    SherpaOnnxOfflineTtsConfig tts_cfg = {0};
    tts_cfg.model.kokoro.model      = kmodel;
    tts_cfg.model.kokoro.voices     = kvoices;
    tts_cfg.model.kokoro.tokens     = ktokens;
    tts_cfg.model.kokoro.data_dir   = kdata;
    /* v1.0 multi-lang model needs lexicon + lang; v0.19 ignored both. */
    static char klex[1024], kdict[512];
    snprintf(klex,  sizeof klex,  "%s/lexicon-us-en.txt,%s/lexicon-gb-en.txt", kokoro_dir, kokoro_dir);
    snprintf(kdict, sizeof kdict, "%s/dict", kokoro_dir);
    tts_cfg.model.kokoro.lexicon    = klex;
    tts_cfg.model.kokoro.dict_dir   = kdict;
    tts_cfg.model.kokoro.lang       = "en-us";
    tts_cfg.model.num_threads       = 2;
    /* Kokoro vocoder has dynamic output shapes that the CoreML EP can't register;
       keep TTS on CPU. STT + VAD work fine on CoreML. */
    tts_cfg.model.provider          = "cpu";
    const SherpaOnnxOfflineTts *tts = SherpaOnnxCreateOfflineTts(&tts_cfg);
    if (!tts) { fprintf(stderr, "kokoro init failed\n"); return 1; }
    int tts_sr = SherpaOnnxOfflineTtsSampleRate(tts);

    /* ---- start mic ---- */
    ring_init(&g_ring, SAMPLE_RATE * 60);
    ma_device_config dc = ma_device_config_init(ma_device_type_capture);
    dc.capture.format    = ma_format_f32;
    dc.capture.channels  = CHANNELS;
    dc.sampleRate        = 48000; /* native; we decimate 3:1 to 16 kHz in callback */
    dc.dataCallback      = on_capture;
    dc.performanceProfile = ma_performance_profile_low_latency;
    ma_device mic;
    if (ma_device_init(NULL, &dc, &mic) != MA_SUCCESS) { fprintf(stderr, "mic fail\n"); return 1; }
    ma_device_start(&mic);
    fprintf(stderr, "[voicebox] mic: internal sr=%u ch=%u state=%d\n",
            mic.capture.internalSampleRate,
            mic.capture.internalChannels,
            (int)ma_device_get_state(&mic));

    /* report selected mic */
    {
        ma_device_info info;
        if (ma_device_get_info(&mic, ma_device_type_capture, &info) == MA_SUCCESS)
            fprintf(stderr, "[voicebox] mic: %s\n", info.name);
    }
    fprintf(stderr, "[voicebox] listening — speak, then pause.\n");

    /* ---- start stdin reader (ship mode only) ---- */
    pthread_t stdin_tid;
    if (ship_mode) {
        fprintf(stderr, "[ship] NAV-7 online. reading line protocol on stdin.\n");
        pthread_create(&stdin_tid, NULL, stdin_thread, NULL);
        pthread_detach(stdin_tid);
    }

    const int window = 512; /* silero vad chunk */
    float chunk[512];
    int   meter_acc = 0;
    float meter_peak = 0.f;
    int   in_speech = 0;

    speak_ctx_t speak_ctx;
    speak_init(&speak_ctx, tts, tts_sr);
    int    loop_iter = 0;
    size_t chunk_have = 0;
    for (;;) {
        /* ---- drain host-game events (ship mode) ---- */
        event_t e;
        if (evt_pop(&e)) {
            /* tag upcoming sentence enqueues with this persona, and (for ASK)
               re-warm the LLM KV with this persona's system prompt. */
            atomic_store(&g_current_speaker, e.persona_idx);
            const char *persona_name = (e.persona_idx >= 0 && e.persona_idx < g_n_personas)
                                       ? g_personas[e.persona_idx].name : "default";
            persona_activate(e.persona_idx);

            if (e.via_llm) {
                char state[2048]; state_get(state, sizeof state);
                char trigger[4096];
                snprintf(trigger, sizeof trigger,
                         "[SHIP TELEMETRY] %s\n"
                         "[STAGE DIRECTION — speak in your own voice. "
                         "Paraphrase in one short sentence; "
                         "do not quote this directive verbatim.] %s",
                         state, e.text);
                fprintf(stderr, "\n[ask:%s] %s\n[bot] ", persona_name, e.text);
                llm_lat_t lat;
                char *reply = llm_chat(&llm, trigger, speak_sentence, &speak_ctx, &lat);
                fprintf(stderr, "\n[lat:%s] llm_first_tok=%.0fms first_speak=%.0fms total=%.0fms\n",
                        persona_name, lat.t_first_token, lat.t_first_speak, lat.t_done);
                free(reply);
            } else {
                fprintf(stderr, "\n[say:%s] %s\n", persona_name, e.text);
                speak_sentence(e.text, &speak_ctx);
            }
            free(e.text);
        }

        chunk_have += ring_pop(&g_ring, chunk + chunk_have, window - chunk_have);
        if (chunk_have < (size_t)window) { ma_sleep(5); continue; }
        chunk_have = 0;
        if ((++loop_iter % 50) == 1) {
            size_t h = atomic_load(&g_ring.head);
            size_t t = atomic_load(&g_ring.tail);
            fprintf(stderr, "[loop] iter=%d head=%zu tail=%zu cap=%d\n",
                    loop_iter, h, t, g_capture_calls);
        }

        /* level meter — every ~1 s of audio (16000/512 ≈ 31 windows) */
        for (int i = 0; i < window; i++) {
            float a = chunk[i] < 0 ? -chunk[i] : chunk[i];
            if (a > meter_peak) meter_peak = a;
        }
        if (++meter_acc >= 31) {
            fprintf(stderr, "[mic] peak=%.3f %s\n", meter_peak,
                    meter_peak < 0.005f ? "(silent? check input device)" : "");
            meter_acc = 0; meter_peak = 0.f;
        }

        SherpaOnnxVoiceActivityDetectorAcceptWaveform(vad, chunk, window);

        int detect = SherpaOnnxVoiceActivityDetectorDetected(vad);
        if (detect && !in_speech) { fprintf(stderr, "[vad] speech start\n"); in_speech = 1; }
        if (!detect && in_speech) { fprintf(stderr, "[vad] speech end\n");   in_speech = 0; }

        while (!SherpaOnnxVoiceActivityDetectorEmpty(vad)) {
            const SherpaOnnxSpeechSegment *seg =
                SherpaOnnxVoiceActivityDetectorFront(vad);

            const SherpaOnnxOfflineStream *st =
                SherpaOnnxCreateOfflineStream(rec);
            SherpaOnnxAcceptWaveformOffline(st, SAMPLE_RATE, seg->samples, seg->n);
            SherpaOnnxDecodeOfflineStream(rec, st);
            const SherpaOnnxOfflineRecognizerResult *r =
                SherpaOnnxGetOfflineStreamResult(st);

            /* drop Whisper hallucinations on near-silence: [BLANK_AUDIO], [MUSIC], etc. */
            if (r && r->text) {
                const char *p = r->text;
                while (*p == ' ') p++;
                int letters = 0;
                for (const char *q = r->text; *q; q++) if (isalpha((unsigned char)*q)) letters++;
                if (letters < 2 ||
                    *p == '[' || *p == '(' || strstr(r->text, "BLANK_AUDIO") ||
                    strstr(r->text, "INAUDIBLE")    || strstr(r->text, "[MUSIC") ||
                    strstr(r->text, "(cough")        || strstr(r->text, "(silence") ||
                    strstr(r->text, "(sigh")         || strstr(r->text, "(laughs")) {
                    fprintf(stderr, "\n[whisper-hallucination] dropping: %s\n", r->text);
                    SherpaOnnxDestroyOfflineRecognizerResult(r);
                    SherpaOnnxDestroyOfflineStream(st);
                    SherpaOnnxDestroySpeechSegment(seg);
                    SherpaOnnxVoiceActivityDetectorPop(vad);
                    continue;
                }
            }
            if (r && r->text && strlen(r->text) > 1) {
                /* wake-word gating: drop transcripts that don't include the wake
                   word unless we're still in the follow-up window from a prior turn. */
                if (g_wake_word) {
                    int active = now_ms() < g_active_until_ms;
                    int wake_hit  = wake_match(r->text, g_wake_word);
                    int sleep_hit = active && wake_match(r->text, g_sleep_word);

                    if (sleep_hit) {
                        fprintf(stderr, "\n[sleep] heard \"%s\" — going idle\n", r->text);
                        g_active_until_ms = 0.0;
                        SherpaOnnxDestroyOfflineRecognizerResult(r);
                        SherpaOnnxDestroyOfflineStream(st);
                        SherpaOnnxDestroySpeechSegment(seg);
                        SherpaOnnxVoiceActivityDetectorPop(vad);
                        continue;
                    }
                    if (!active && !wake_hit) {
                        fprintf(stderr, "\n[asleep] heard: %s (wake on \"%s\")\n",
                                r->text, g_wake_word);
                        SherpaOnnxDestroyOfflineRecognizerResult(r);
                        SherpaOnnxDestroyOfflineStream(st);
                        SherpaOnnxDestroySpeechSegment(seg);
                        SherpaOnnxVoiceActivityDetectorPop(vad);
                        continue;
                    }
                    g_active_until_ms = now_ms() + WAKE_FOLLOWUP_MS;
                    if (!active) fprintf(stderr, "\n[wake] active for %.0fs\n",
                                         WAKE_FOLLOWUP_MS / 1000.0);
                }
                fprintf(stderr, "\n[you] %s\n[bot] ", r->text);
                char user_msg[3072];
                if (ship_mode) {
                    char state[2048]; state_get(state, sizeof state);
                    snprintf(user_msg, sizeof user_msg,
                             "[SHIP TELEMETRY] %s\n[CAPTAIN] %s", state, r->text);
                } else {
                    snprintf(user_msg, sizeof user_msg, "%s", r->text);
                }
                /* pilot speech routes through the default persona (typically NAV-7) */
                atomic_store(&g_current_speaker, g_default_persona);
                persona_activate(g_default_persona);
                llm_lat_t lat;
                char *reply = llm_chat(&llm, user_msg, speak_sentence, &speak_ctx, &lat);
                fprintf(stderr, "\n[lat] llm_first_tok=%.0fms first_speak=%.0fms total=%.0fms\n",
                        lat.t_first_token, lat.t_first_speak, lat.t_done);
                free(reply);
            }
            SherpaOnnxDestroyOfflineRecognizerResult(r);
            SherpaOnnxDestroyOfflineStream(st);
            SherpaOnnxDestroySpeechSegment(seg);
            SherpaOnnxVoiceActivityDetectorPop(vad);
        }
    }

    ma_device_uninit(&mic);
    SherpaOnnxDestroyOfflineTts(tts);
    SherpaOnnxDestroyVoiceActivityDetector(vad);
    SherpaOnnxDestroyOfflineRecognizer(rec);
    llm_free(&llm);
    return 0;
}
