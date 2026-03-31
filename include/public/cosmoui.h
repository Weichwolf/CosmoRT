/* CosmoUI API — Display, Audio, Input
 *
 * Three kernel subsystems for latency-critical UI.
 * Inspired by BeOS BMediaKit/BScreen and SDL3.
 *
 * Display: Surfaces. A surface is a pixel buffer the kernel presents
 * to a display at VSync. No windows, no compositor — each VT slot
 * owns one surface. The kernel flips, the app draws.
 *
 * Audio: Streams. An app opens a stream (playback or capture), submits
 * PCM buffers. The kernel mixes all active streams on the RT core.
 * 12 streams (one per VT slot), each with volume/pan/fx-send.
 * Callback-free: the app pushes buffers, the kernel pulls at IRQ rate.
 *
 * Input: Events. Keyboard, mouse, touch, gamepad — unified event queue.
 * The kernel delivers events to the focused VT slot. Apps block on read.
 * RT core polls hardware, zero missed events.
 */

#ifndef COSMO_UI_H
#define COSMO_UI_H

#include <stdint.h>

/* ════════════════════════════════════════════════════════════════════
 * Display
 *
 * One surface per VT slot. Surface = pixel buffer + metadata.
 * Present blocks until VSync — no tearing, guaranteed.
 * Multi-monitor: each display has an ID. Surface binds to display.
 * ════════════════════════════════════════════════════════════════════ */

/* Pixel formats */
#define COSMO_FMT_ARGB8888  0  /* 32-bit, alpha + RGB */
#define COSMO_FMT_XRGB8888  1  /* 32-bit, no alpha */
#define COSMO_FMT_RGB565    2  /* 16-bit */

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t refresh_hz;    /* 60, 120, 144, ... */
    uint32_t format;        /* native pixel format */
    char     name[32];      /* "HDMI-1", "DP-2", ... */
} cosmo_display_info_t;

/* Query connected displays. Returns count. */
int cosmo_display_count(void);

/* Get display info. display = 0..count-1. */
int cosmo_display_info(int display, cosmo_display_info_t *info);

/* Create a surface on a display.
 * Returns surface_id >= 0, or negative errno.
 * The surface is backed by a kernel-allocated framebuffer.
 * Use cosmo_surface_map to get a writable pointer. */
int cosmo_surface_create(int display, uint32_t w, uint32_t h, uint32_t fmt);

/* Map surface pixels into app address space.
 * Returns pointer to pixel data, or NULL on error.
 * Stride = bytes per row (may be > width * bpp for alignment). */
void *cosmo_surface_map(int surface_id, uint32_t *stride);

/* Present surface to display. Blocks until VSync.
 * After return, the app may draw the next frame.
 * Double-buffering is implicit — kernel swaps on present. */
int cosmo_surface_present(int surface_id);

/* Destroy surface and release framebuffer. */
void cosmo_surface_destroy(int surface_id);

/* ════════════════════════════════════════════════════════════════════
 * Audio
 *
 * Push model: app writes PCM buffers, kernel pulls on RT core.
 * Each stream has a ring buffer. The kernel drains it at the
 * hardware sample rate. If the ring runs empty → silence.
 *
 * 12 mixer channels (one per VT slot):
 *   volume (0..255), pan (L128..R128), fx_send (0..255)
 *
 * Master bus: one FX slot (reverb/delay) + compressor.
 * Active slot = audible. Background slots = mute unless pinned.
 * ════════════════════════════════════════════════════════════════════ */

/* Audio sample formats */
#define COSMO_AUDIO_S16    0  /* signed 16-bit PCM (native) */
#define COSMO_AUDIO_F32    1  /* 32-bit float [-1.0, 1.0] */

typedef struct {
    uint32_t sample_rate;   /* 44100, 48000, 96000 */
    uint32_t channels;      /* 1 = mono, 2 = stereo, 6 = 5.1, 8 = 7.1 */
    uint32_t format;        /* COSMO_AUDIO_S16, COSMO_AUDIO_F32 */
    uint32_t buffer_frames; /* ring buffer size in frames */
    char     name[64];      /* "HDA Intel PCH", "USB Headset", ... */
} cosmo_audio_info_t;

/* Query audio devices. Returns count. */
int cosmo_audio_device_count(void);

/* Get device info. dev = 0..count-1. */
int cosmo_audio_device_info(int dev, cosmo_audio_info_t *info);

/* Open a playback stream. Returns stream_id >= 0, or negative errno.
 * The stream is initially silent (no data in ring). */
int cosmo_audio_open(int dev, uint32_t rate, uint32_t channels, uint32_t fmt);

/* Open a capture stream (microphone). Same semantics. */
int cosmo_audio_capture_open(int dev, uint32_t rate, uint32_t channels, uint32_t fmt);

/* Submit PCM frames to playback ring buffer.
 * Returns frames accepted (may be less than requested if ring full).
 * Non-blocking. App should poll or sleep and retry. */
int cosmo_audio_write(int stream, const void *buf, uint32_t frames);

/* Read captured PCM frames from capture ring.
 * Returns frames read (may be 0 if ring empty). Non-blocking. */
int cosmo_audio_read(int stream, void *buf, uint32_t frames);

/* Per-stream mixer controls. Applied on RT core during mix. */
int cosmo_audio_set_volume(int stream, uint8_t volume);  /* 0..255 */
int cosmo_audio_set_pan(int stream, int8_t pan);          /* -128=L, 0=C, 127=R */
int cosmo_audio_set_fx_send(int stream, uint8_t send);    /* 0..255 */

/* Pin: stream plays even when its VT slot is not focused. */
int cosmo_audio_pin(int stream, int pinned);  /* 1=pinned, 0=normal */

/* Close stream. Ring buffer freed. */
void cosmo_audio_close(int stream);

/* ════════════════════════════════════════════════════════════════════
 * Input
 *
 * Unified event queue. All input devices (keyboard, mouse, touch,
 * gamepad) deliver events in the same format. The kernel routes
 * events to the focused VT slot.
 *
 * Event types match Linux input subsystem (EV_KEY, EV_REL, EV_ABS).
 * Key codes match Linux KEY_* / BTN_* constants.
 * Apps that work with /dev/input on Linux work here unchanged.
 *
 * F1-F12 are intercepted by the kernel for VT switching.
 * The app never sees them (unless in exclusive/fullscreen mode).
 * ════════════════════════════════════════════════════════════════════ */

/* Event types */
#define COSMO_EV_KEY   0x01  /* keyboard/button press/release */
#define COSMO_EV_REL   0x02  /* relative movement (mouse) */
#define COSMO_EV_ABS   0x03  /* absolute position (touch, tablet) */

typedef struct {
    uint64_t timestamp_us;  /* microseconds since boot */
    uint16_t type;          /* COSMO_EV_KEY, EV_REL, EV_ABS */
    uint16_t code;          /* KEY_A, REL_X, ABS_X, BTN_LEFT, ... */
    int32_t  value;         /* key: 1=press 0=release, rel: delta, abs: position */
} cosmo_input_event_t;

typedef struct {
    char     name[64];      /* "AT Keyboard", "USB Mouse", ... */
    uint16_t type;          /* primary type: EV_KEY, EV_REL, EV_ABS */
    uint16_t vendor_id;
    uint16_t product_id;
} cosmo_input_info_t;

/* Query input devices. Returns count. */
int cosmo_input_device_count(void);

/* Get device info. dev = 0..count-1. */
int cosmo_input_device_info(int dev, cosmo_input_info_t *info);

/* Read next input event for this VT slot.
 * Blocks until an event is available.
 * Returns 0 on success, negative errno on error. */
int cosmo_input_read(cosmo_input_event_t *ev);

/* Read from a specific device (bypass VT routing).
 * For raw device access (gamepad, MIDI controller). */
int cosmo_input_read_device(int dev, cosmo_input_event_t *ev);

/* Request exclusive input (fullscreen mode).
 * F-keys no longer switch VTs. App sees all keys.
 * System cursor hidden. Returns 0 or negative errno. */
int cosmo_input_grab(void);

/* Release exclusive input. VT switching restored. */
int cosmo_input_ungrab(void);

#endif
