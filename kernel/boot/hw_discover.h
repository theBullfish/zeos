/*
 * Zeos -- Hardware Discovery
 *
 * At boot, scan hardware and create a chain for each device.
 * No traditional "device driver" model -- each device IS a chain
 * with typed inputs and outputs that MDE can route.
 */

#ifndef ZEOS_HW_DISCOVER_H
#define ZEOS_HW_DISCOVER_H

/* ── Hardware chain types ───────────────────────────────────────── */

#define HW_TYPE_CPU       "hw.cpu"
#define HW_TYPE_MEMORY    "hw.memory"
#define HW_TYPE_GPU       "hw.gpu"
#define HW_TYPE_NIC       "hw.nic"
#define HW_TYPE_STORAGE   "hw.storage"
#define HW_TYPE_INPUT     "hw.input"
#define HW_TYPE_USB       "hw.usb"
#define HW_TYPE_AUDIO     "hw.audio"
#define HW_TYPE_BRIDGE    "hw.bridge"
#define HW_TYPE_WIRELESS  "hw.wireless"
#define HW_TYPE_ACCEL     "hw.accelerator"
#define HW_TYPE_SERIAL    "hw.serial"
#define HW_TYPE_CRYPTO    "hw.crypto"
#define HW_TYPE_UNKNOWN   "hw.unknown"

/* ── API ────────────────────────────────────────────────────────── */

/* Discover all hardware and create chains. Returns number of chains created. */
int hw_discover_all(void);

/* Get chain ID for a hardware type (first match). Returns -1 if not found. */
int hw_find_chain(const char *hw_type);

/* Get count of hardware chains created by discovery */
int hw_chain_count(void);

#endif /* ZEOS_HW_DISCOVER_H */
