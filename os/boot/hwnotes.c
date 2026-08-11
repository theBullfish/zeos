/*
 * Zeos — hardware annotation layer. See hwnotes.h for the design contract.
 *
 * Append-only log in the vault + an in-RAM index. Replay collapses supersedes
 * (same identity, higher seq wins) so the newest observation is what you read,
 * while every earlier observation stays on disk as provenance.
 */
#include "hwnotes.h"
#include "vault.h"
#include "kprint.h"

#define HWNOTE_MAGIC   0x314E485Au     /* "ZHN1" */
#define HWNOTE_PATH    "/hw/notes.db"
#define HWNOTE_MAX     192             /* in-RAM index cap (vault inode ~48 KiB) */
#define HWSHARE_PATH   "/hw/share_consent"   /* presence+value = explicit choice */

static struct hwnote g_idx[HWNOTE_MAX];
static int  g_count;
static int  g_ready;
static uint32_t g_seq;

int hwnote_count(void) { return g_count; }
int hwnote_ready(void) { return g_ready; }
const struct hwnote *hwnote_at(int i) {
    return (i >= 0 && i < g_count) ? &g_idx[i] : 0;
}

static void n_memset(void *d, int c, unsigned long n) {
    unsigned char *p = d; while (n--) *p++ = (unsigned char)c;
}
static void n_copy(char *dst, const char *src, int cap) {
    int i = 0;
    if (src) for (; src[i] && i < cap - 1; i++) dst[i] = src[i];
    dst[i] = 0;
}
static int same_id(const struct hwnote *n, uint8_t bus, uint16_t v, uint16_t d) {
    return n->bus == bus && n->vendor == v && n->device == d;
}

const struct hwnote *hwnote_find(uint8_t bus, uint16_t vendor, uint16_t device)
{
    for (int i = 0; i < g_count; i++)
        if (same_id(&g_idx[i], bus, vendor, device)) return &g_idx[i];
    return 0;
}

/* Insert-or-supersede in the RAM index. */
static void index_put(const struct hwnote *rec)
{
    for (int i = 0; i < g_count; i++) {
        if (same_id(&g_idx[i], rec->bus, rec->vendor, rec->device)) {
            if (rec->seq >= g_idx[i].seq) g_idx[i] = *rec;   /* newest wins */
            return;
        }
    }
    if (g_count < HWNOTE_MAX) g_idx[g_count++] = *rec;
}

void hwnote_init(void)
{
    g_count = 0; g_seq = 0;
    int sz = vault_size(HWNOTE_PATH);
    if (sz <= 0) {
        /* No log yet — that's fine; the first hwnote_add creates it. Notes still
         * work in RAM if the vault is unavailable, they just don't persist. */
        g_ready = (vault_exists("/hw") >= 0);
        return;
    }
    static struct hwnote scratch[HWNOTE_MAX];
    int want = sz;
    if (want > (int)sizeof scratch) want = (int)sizeof scratch;
    int got = vault_read(HWNOTE_PATH, scratch, (uint32_t)want);
    if (got <= 0) { g_ready = 1; return; }

    int recs = got / (int)sizeof(struct hwnote);
    for (int i = 0; i < recs; i++) {
        if (scratch[i].magic != HWNOTE_MAGIC) continue;   /* skip torn/garbage */
        index_put(&scratch[i]);
        if (scratch[i].seq > g_seq) g_seq = scratch[i].seq;
    }
    g_ready = 1;
    kputs("[hwnote] replayed "); kput_dec((uint64_t)recs);
    kputs(" record(s) -> "); kput_dec((uint64_t)g_count);
    kputs(" live annotation(s)\n");
}

/* ── sharing consent ────────────────────────────────────────────────────── */

static int g_share = -1;        /* -1 = not loaded yet, 0 = off, 1 = on */

int hwnote_share_enabled(void)
{
    if (g_share < 0) {
        /* Absent file == never asked == OFF. Only an explicit 'on' turns it on. */
        unsigned char v = 0;
        g_share = (vault_read(HWSHARE_PATH, &v, 1) == 1 && v == 1) ? 1 : 0;
    }
    return g_share;
}

int hwnote_share_set(int on)
{
    unsigned char v = on ? 1 : 0;
    g_share = v;
    (void)vault_mkdir("/hw", VAULT_TIER_INTERNAL);
    /* Persist the CHOICE so it is remembered across boots and never silently
     * re-defaults. A failed write leaves the in-RAM value for this session. */
    return vault_write(HWSHARE_PATH, &v, 1) >= 0;
}

const char *hwnote_share_endpoint(void)
{
    /* Contribution target. Returned only so the UI can tell the user exactly
     * where their data would go BEFORE they consent. */
    return "zeos-hwdb.codexlabs (fleet contribution)";
}

int hwnote_share_payload(char *buf, int cap)
{
    /* THE choke point. Everything that could leave the machine passes here. */
    if (!hwnote_share_enabled()) return -1;
    if (!buf || cap < 64) return -2;

    int n = 0;
    const char *hdr = "zeos-hwnotes/1\n";
    for (const char *q = hdr; *q && n < cap - 1; q++) buf[n++] = *q;

    for (int i = 0; i < g_count; i++) {
        const struct hwnote *r = &g_idx[i];
        /* One line per device: bus vid did class/sub/pif flags proto text.
         * Hardware facts only — nothing identifying the user or the machine. */
        char line[160]; int m = 0;
        static const char hx[] = "0123456789abcdef";
        line[m++] = (r->bus == HWNOTE_BUS_USB) ? 'u' : (r->bus == HWNOTE_BUS_DT ? 'd' : 'p');
        line[m++] = ' ';
        for (int s2 = 12; s2 >= 0; s2 -= 4) line[m++] = hx[(r->vendor >> s2) & 0xF];
        line[m++] = ':';
        for (int s2 = 12; s2 >= 0; s2 -= 4) line[m++] = hx[(r->device >> s2) & 0xF];
        line[m++] = ' ';
        line[m++] = hx[(r->cls >> 4) & 0xF]; line[m++] = hx[r->cls & 0xF]; line[m++] = '/';
        line[m++] = hx[(r->sub >> 4) & 0xF]; line[m++] = hx[r->sub & 0xF]; line[m++] = '/';
        line[m++] = hx[(r->progif >> 4) & 0xF]; line[m++] = hx[r->progif & 0xF];
        line[m++] = ' ';
        line[m++] = hx[(r->flags >> 4) & 0xF]; line[m++] = hx[r->flags & 0xF];
        line[m++] = ' ';
        for (int k = 0; r->protocol[k] && m < 140; k++) line[m++] = r->protocol[k];
        line[m++] = ' ';
        for (int k = 0; r->text[k] && m < 155; k++)
            line[m++] = (r->text[k] == '\n') ? ' ' : r->text[k];
        line[m++] = '\n';
        if (n + m >= cap - 1) return -2;
        for (int k = 0; k < m; k++) buf[n++] = line[k];
    }
    buf[n] = 0;
    return n;
}

int hwnote_add(uint8_t bus, uint16_t vendor, uint16_t device,
               uint8_t cls, uint8_t sub, uint8_t progif,
               uint32_t flags, const char *protocol, const char *text)
{
    struct hwnote rec;
    n_memset(&rec, 0, sizeof rec);
    rec.magic  = HWNOTE_MAGIC;
    rec.bus    = bus;
    rec.vendor = vendor;
    rec.device = device;
    rec.cls = cls; rec.sub = sub; rec.progif = progif;
    rec.flags  = flags | HWNOTE_F_SEEN;
    rec.seq    = ++g_seq;
    n_copy(rec.protocol, protocol, HWNOTE_PROTO_LEN);
    n_copy(rec.text, text, HWNOTE_TEXT_LEN);

    index_put(&rec);

    /* Append — never rewrite. vault_append creates the file on first call and
     * extends the inode without minting a new temporal version (log semantics). */
    (void)vault_mkdir("/hw", VAULT_TIER_INTERNAL);
    int rc = vault_append(HWNOTE_PATH, &rec, (uint32_t)sizeof rec);
    if (rc < 0) return 0;      /* RAM-only: the annotation is live but unsaved */
    return 1;
}
