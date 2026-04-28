/*
 * fx.c — tiny DSP chain for "robot voice" coloration of Kokoro's PCM.
 * In-place on float samples, mono, sample rate from the caller.
 *
 * Filters:
 *   intercom — bandpass 300–3300 Hz + soft saturation. Ship-comms feel.
 *   vintage  — intercom + light ring modulation (~28 Hz). 70s-AI shimmer.
 *   robot    — intercom + heavier ring mod (~80 Hz) + 6-bit crush. Hard sci-fi.
 *   none     — no-op.
 */
#include <math.h>
#include <string.h>
#include "fx.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static void apply_intercom(float *s, int n, int sr) {
    /* 1-pole high-pass at 300 Hz */
    float a_hp = expf(-2.0f * (float)M_PI * 300.0f  / (float)sr);
    /* 1-pole low-pass at 3300 Hz */
    float a_lp = expf(-2.0f * (float)M_PI * 3300.0f / (float)sr);
    float hp_x = 0, hp_y = 0, lp_y = 0;
    for (int i = 0; i < n; i++) {
        float x = s[i];
        float y = a_hp * (hp_y + x - hp_x);
        hp_x = x; hp_y = y;
        lp_y = (1.0f - a_lp) * y + a_lp * lp_y;
        /* mild tanh drive for grit */
        s[i] = tanhf(lp_y * 1.6f) * 0.9f;
    }
}

static void apply_ringmod(float *s, int n, int sr, float hz, float mix) {
    float phase = 0;
    float dphi  = 2.0f * (float)M_PI * hz / (float)sr;
    for (int i = 0; i < n; i++) {
        float c = cosf(phase);
        s[i]    = s[i] * ((1.0f - mix) + mix * c);
        phase  += dphi;
        if (phase > 2.0f * (float)M_PI) phase -= 2.0f * (float)M_PI;
    }
}

static void apply_bitcrush(float *s, int n, int bits) {
    int   levels = 1 << (bits - 1);
    float step   = 1.0f / (float)levels;
    for (int i = 0; i < n; i++) {
        float v = s[i] / step;
        v = (v >= 0 ? floorf(v + 0.5f) : -floorf(-v + 0.5f));
        s[i] = v * step;
    }
}

void fx_apply(const char *name, float *s, int n, int sr) {
    if (!name || !*name || !strcmp(name, "none")) return;
    if (!strcmp(name, "intercom")) {
        apply_intercom(s, n, sr);
    } else if (!strcmp(name, "vintage")) {
        apply_intercom(s, n, sr);
        apply_ringmod(s, n, sr, 28.0f, 0.55f);
    } else if (!strcmp(name, "robot")) {
        apply_intercom(s, n, sr);
        apply_ringmod(s, n, sr, 80.0f, 0.9f);
        apply_bitcrush(s, n, 6);
    }
}
