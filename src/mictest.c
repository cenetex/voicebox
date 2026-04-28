/* Minimal mic test — counts callbacks over 5 s */
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
#include <stdio.h>

static volatile int g_calls = 0;
static volatile unsigned long g_frames = 0;
static volatile float g_peak = 0;

static void cb(ma_device *d, void *o, const void *i, ma_uint32 f) {
    (void)d; (void)o;
    g_calls++;
    g_frames += f;
    const float *s = i;
    for (ma_uint32 k = 0; k < f; k++) {
        float a = s[k] < 0 ? -s[k] : s[k];
        if (a > g_peak) g_peak = a;
    }
}

int main(void) {
    setvbuf(stderr, NULL, _IONBF, 0);
    ma_device_config c = ma_device_config_init(ma_device_type_capture);
    c.capture.format   = ma_format_f32;
    c.capture.channels = 1;
    c.sampleRate       = 48000;
    c.dataCallback     = cb;
    ma_device d;
    if (ma_device_init(NULL, &c, &d) != MA_SUCCESS) { fprintf(stderr, "init fail\n"); return 1; }
    ma_device_info info;
    ma_device_get_info(&d, ma_device_type_capture, &info);
    fprintf(stderr, "device: %s  sr=%u\n", info.name, d.capture.internalSampleRate);
    if (ma_device_start(&d) != MA_SUCCESS) { fprintf(stderr, "start fail\n"); return 1; }
    fprintf(stderr, "recording 5 s — speak now\n");
    for (int s = 1; s <= 5; s++) {
        ma_sleep(1000);
        fprintf(stderr, "[%ds] calls=%d frames=%lu peak=%.3f\n", s, g_calls, g_frames, g_peak);
        g_peak = 0;
    }
    ma_device_uninit(&d);
    return 0;
}
