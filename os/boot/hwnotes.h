/*
 * Zeos — hardware ANNOTATION layer (our own observations).
 *
 * Deliberately SEPARATE from the vendor database (hwdb.c, generated from
 * pci.ids/usb.ids). We never edit or overwrite vendor data:
 *
 *   hwdb   = what the world publishes  (regenerate any time, ours survives)
 *   hwnote = what WE measured on real hardware, appended alongside
 *
 * The two JOIN on the same identity (bus + vendor + device), so a lookup returns
 * the vendor's facts plus a flag saying "there is local data to be read here."
 * When the vendor list gains an entry we already annotated, nothing collides —
 * our record simply becomes a sub-note under the now-official name.
 *
 * Storage is an APPEND-ONLY log in the vault, so history is never rewritten:
 * a later observation of the same device supersedes an earlier one on replay,
 * and both remain on disk as provenance.
 */
#ifndef ZEOS_HWNOTES_H
#define ZEOS_HWNOTES_H

#include <stdint.h>

#define HWNOTE_BUS_PCI  1
#define HWNOTE_BUS_USB  2
#define HWNOTE_BUS_DT   3      /* device-tree node (ARM): key by compat hash */

/* flags — what we learned by actually touching it */
#define HWNOTE_F_SEEN     (1u << 0)   /* enumerated on real hardware */
#define HWNOTE_F_PROBED   (1u << 1)   /* a driver attached */
#define HWNOTE_F_WORKS    (1u << 2)   /* verified functional */
#define HWNOTE_F_FAILED   (1u << 3)   /* attached but did not work */
#define HWNOTE_F_QUIRK    (1u << 4)   /* needs special handling (see text) */

#define HWNOTE_PROTO_LEN  16
#define HWNOTE_TEXT_LEN   72

/* On-disk record. Fixed size so the log is trivially replayable. */
struct hwnote {
    uint32_t magic;
    uint16_t vendor;
    uint16_t device;
    uint8_t  bus;                        /* HWNOTE_BUS_* */
    uint8_t  cls, sub, progif;           /* class triple as WE observed it */
    uint32_t flags;
    uint32_t seq;                        /* monotonic; higher supersedes */
    char     protocol[HWNOTE_PROTO_LEN]; /* what we actually drove it as */
    char     text[HWNOTE_TEXT_LEN];      /* what we learned, free-form */
};

void hwnote_init(void);      /* replay the vault log into the index */
int  hwnote_count(void);     /* live (superseded-collapsed) entry count */
int  hwnote_ready(void);     /* 1 = vault-backed, 0 = RAM only */

/* Join point: annotation for this identity, or NULL. Never consults hwdb —
 * callers show vendor data AND this, so the layers stay visibly distinct. */
const struct hwnote *hwnote_find(uint8_t bus, uint16_t vendor, uint16_t device);

/* Record an observation. Appends; never mutates an existing record. */
int hwnote_add(uint8_t bus, uint16_t vendor, uint16_t device,
               uint8_t cls, uint8_t sub, uint8_t progif,
               uint32_t flags, const char *protocol, const char *text);

/* Iterate the live index (0..hwnote_count()-1), for listing. */
const struct hwnote *hwnote_at(int index);

/* ── SHARING (opt-in, off by default) ──────────────────────────────────────
 * Annotations are LOCAL and fully functional offline. They may optionally be
 * contributed to a shared fleet database so hardware one machine has already
 * characterised is known to the others.
 *
 * Consent rules, enforced here rather than by convention:
 *   - Default is OFF. A fresh install shares nothing.
 *   - Only an explicit user action flips it (hwnote share on), and that choice
 *     is persisted so it is remembered, never re-defaulted to on.
 *   - hwnote_share_payload() is the ONE function that can hand data out, and it
 *     refuses unless consent is set. No caller can bypass it.
 *   - The payload is hardware identity + what we measured driving it. No user
 *     content, no filenames, no identifiers of the person or machine.
 */
int  hwnote_share_enabled(void);          /* 0 = do not transmit anything */
int  hwnote_share_set(int on);            /* explicit user choice; persisted */
const char *hwnote_share_endpoint(void);  /* where it would go, or NULL */

/* Serialize annotations for contribution. Returns bytes written, or:
 *   -1 = consent not granted (nothing serialized)
 *   -2 = buffer too small
 * Callers MUST treat any negative return as "transmit nothing". */
int  hwnote_share_payload(char *buf, int cap);

#endif
