/* CosmoUI API — Display, Audio, Input */

#ifndef COSMO_UI_H
#define COSMO_UI_H

#include <stdint.h>

#define COSMO_FMT_ARGB8888  0
#define COSMO_FMT_XRGB8888  1
#define COSMO_FMT_RGB565    2

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t refresh_hz;
    uint32_t format;
    char     name[32];
} cosmo_display_info_t;

int cosmo_display_count(void);

int cosmo_display_info(int display, cosmo_display_info_t *info);

int cosmo_surface_create(int display, uint32_t w, uint32_t h, uint32_t fmt);

void *cosmo_surface_map(int surface_id, uint32_t *stride);

int cosmo_surface_present(int surface_id);

void cosmo_surface_destroy(int surface_id);

#define COSMO_AUDIO_S16    0
#define COSMO_AUDIO_F32    1

typedef struct {
    uint32_t sample_rate;
    uint32_t channels;
    uint32_t format;
    uint32_t buffer_frames;
    char     name[64];
} cosmo_audio_info_t;

int cosmo_audio_device_count(void);

int cosmo_audio_device_info(int dev, cosmo_audio_info_t *info);

int cosmo_audio_open(int dev, uint32_t rate, uint32_t channels, uint32_t fmt);

int cosmo_audio_capture_open(int dev, uint32_t rate, uint32_t channels, uint32_t fmt);

int cosmo_audio_write(int stream, const void *buf, uint32_t frames);

int cosmo_audio_read(int stream, void *buf, uint32_t frames);

int cosmo_audio_set_volume(int stream, uint8_t volume);
int cosmo_audio_set_pan(int stream, int8_t pan);
int cosmo_audio_set_fx_send(int stream, uint8_t send);

int cosmo_audio_pin(int stream, int pinned);

void cosmo_audio_close(int stream);

#define COSMO_EV_KEY   0x01
#define COSMO_EV_REL   0x02
#define COSMO_EV_ABS   0x03

typedef struct {
    uint64_t timestamp_us;
    uint16_t type;
    uint16_t code;
    int32_t  value;
} cosmo_input_event_t;

typedef struct {
    char     name[64];
    uint16_t type;
    uint16_t vendor_id;
    uint16_t product_id;
} cosmo_input_info_t;

int cosmo_input_device_count(void);

int cosmo_input_device_info(int dev, cosmo_input_info_t *info);

int cosmo_input_read(cosmo_input_event_t *ev);

int cosmo_input_read_device(int dev, cosmo_input_event_t *ev);

int cosmo_input_grab(void);

int cosmo_input_ungrab(void);

#endif
