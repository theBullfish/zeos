/*
 * Zeos -- USB Video Class (UVC) driver header
 *
 * Public surface:
 *   usb_uvc_init()         — walk xhci devices, attach class 0x0E.
 *   usb_uvc_count()        — number of attached cameras.
 *   usb_uvc_pump()         — drain iso events for every camera (called
 *                             from CHAIN_VIDEO_IN's iso_poll node).
 *   usb_uvc_start(idx)     — start streaming on camera idx.
 *   usb_uvc_stop(idx)      — stop streaming.
 *   usb_uvc_dequeue_frame  — pop one assembled JPEG frame, if available.
 *   usb_uvc_print_selftest_line
 *   usb_uvc_describe(idx,...) — vendor/product + format string for shell.
 *   usb_uvc_register_chains() — register CHAIN_VIDEO_IN, per-camera
 *                                CHAIN_CAMERA_<n>, CHAIN_VIDEO_PREVIEW.
 */

#ifndef ZEOS_USB_UVC_H
#define ZEOS_USB_UVC_H

#include <stdint.h>

/* Frame format codes — restricted to what UVC commonly serves. */
#define UVC_FORMAT_MJPEG        1
#define UVC_FORMAT_YUY2         2

/* Reassembled video frame handed to subscribers. */
typedef struct {
    int      camera_idx;
    int      format;        /* UVC_FORMAT_* */
    int      width;
    int      height;
    int      bytes_len;
    uint64_t ts;            /* TSC at frame complete */
    uint8_t *bytes;         /* Borrowed pointer; valid until next dequeue */
} video_frame_t;

int  usb_uvc_init(void);
int  usb_uvc_count(void);
int  usb_uvc_describe(int idx, char *out, int max);
int  usb_uvc_start(int idx);
int  usb_uvc_stop(int idx);
int  usb_uvc_streaming(int idx);
int  usb_uvc_dequeue_frame(int idx, video_frame_t *out);
void usb_uvc_pump(void);
void usb_uvc_print_selftest_line(void);
void usb_uvc_register_chains(void);

/* Single-shot capture: writes the next assembled JPEG to `path` via
 * fat32_write. Returns bytes written on success, negative on failure. */
int  usb_uvc_capture_jpeg(int idx, const char *path);

#endif /* ZEOS_USB_UVC_H */
