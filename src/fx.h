#pragma once
/* In-place DSP coloration. name: "none" | "intercom" | "vintage" | "robot". */
void fx_apply(const char *name, float *samples, int n, int sample_rate);
