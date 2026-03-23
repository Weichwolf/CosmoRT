/* CosmoUI API — Display, Audio, Input
 *
 * Three subsystems CosmoUI talks to directly with minimal latency.
 * Everything else (network, storage, power, USB) goes through
 * cosmo_rt.h (drivers) or POSIX syscalls (libc).
 */
#ifndef COSMO_UI_H
#define COSMO_UI_H

#include <stdint.h>

/* ── Display ── */
typedef struct {
    uint32_t width, height, refresh_hz;
    uint32_t format;
} cosmo_display_info_t;

int cosmo_display_count(void);
int cosmo_display_info(int display, cosmo_display_info_t *info);
int cosmo_surface_create(int display, uint32_t w, uint32_t h, uint32_t fmt);
int cosmo_surface_present(int surface_id); /* VSync, blocks */
void cosmo_surface_destroy(int surface_id);

/* ── Audio ── */
typedef struct {
    uint32_t sample_rate, channels, format;
    char name[64];
} cosmo_audio_info_t;

int cosmo_audio_device_count(void);
int cosmo_audio_device_info(int dev, cosmo_audio_info_t *info);
int cosmo_audio_open(int dev, uint32_t rate, uint32_t ch, uint32_t fmt);
int cosmo_audio_submit(int stream, const void *buf, uint32_t frames);
int cosmo_audio_capture(int stream, void *buf, uint32_t frames);
void cosmo_audio_close(int stream);

/* ── Input ── */
typedef struct {
    uint16_t type;  /* EV_KEY, EV_REL, EV_ABS */
    uint16_t code;  /* KEY_A, REL_X, ABS_X */
    int32_t  value;
} cosmo_input_event_t;

int cosmo_input_device_count(void);
int cosmo_input_read(int dev, cosmo_input_event_t *ev); /* blocks */

#endif
