/*
 * ACPI Embedded Controller. RD_EC / WR_EC protocol on command/data
 * ports. Backs OperationRegion(EmbeddedControl) accesses from AML
 * methods — required for _BST/_BIF on every laptop with battery
 * data behind the EC.
 *
 * Reference: ACPI 6.5 §12 (ACPI Embedded Controller Interface).
 *
 *   Status register (read from cmd port, bit 0 = OBF, bit 1 = IBF,
 *                    bit 5 = SMI_EVT, bit 6 = SCI_EVT, bit 7 = unused)
 *   Commands (write to cmd port):
 *     0x80 RD_EC, 0x81 WR_EC, 0x82 BE_EC, 0x83 BD_EC, 0x84 QR_EC
 *
 *   Read at offset N:
 *     wait IBF=0 ; write 0x80 to cmd ; wait IBF=0 ; write N to data ;
 *     wait OBF=1 ; read byte from data.
 *
 *   Write at offset N:
 *     wait IBF=0 ; write 0x81 to cmd ; wait IBF=0 ; write N to data ;
 *     wait IBF=0 ; write byte to data.
 */

#include "ec.h"
#include "hal.h"
#include "kprint.h"
#include "timer.h"
#include "spinlock.h"
#include "aml.h"

/* Status bits */
#define EC_SR_OBF      0x01u
#define EC_SR_IBF      0x02u
#define EC_SR_SMI_EVT  0x20u
#define EC_SR_SCI_EVT  0x40u

/* Commands */
#define EC_CMD_RD_EC   0x80u
#define EC_CMD_WR_EC   0x81u
#define EC_CMD_BE_EC   0x82u
#define EC_CMD_BD_EC   0x83u
#define EC_CMD_QR_EC   0x84u

/* 100ms timeout per ACPI hand-off step. */
#define EC_TIMEOUT_MS  100u

static struct {
    int      present;
    uint16_t cmd;
    uint16_t data;
    zeos_spinlock_t lock;
    uint64_t ops_read;
    uint64_t ops_write;
    uint64_t ops_timeout;
    int      crs_parsed;   /* 1 if ports came from _CRS, 0 if defaults */
} g_ec = { 0, 0, 0, ZEOS_SPINLOCK_INIT, 0, 0, 0, 0 };

/* Wait for status bit to clear / be set. Returns 0 on success, -1 on timeout. */
static int ec_wait_bit(uint8_t mask, uint8_t want)
{
    /* Use TSC for fine-grained timeout. timer_tsc_freq() is set at boot. */
    uint64_t freq = timer_tsc_freq();
    if (freq == 0) freq = 3000000000ULL;  /* fallback ~3GHz */
    uint64_t deadline = timer_read_tsc() + (freq / 1000ULL) * EC_TIMEOUT_MS;

    while (timer_read_tsc() < deadline) {
        uint8_t s = hal_in8(g_ec.cmd);
        if ((s & mask) == want) return 0;
        __asm__ volatile("pause");
    }
    g_ec.ops_timeout++;
    return -1;
}

static inline int ec_wait_ibf_clear(void) { return ec_wait_bit(EC_SR_IBF, 0); }
static inline int ec_wait_obf_set(void)   { return ec_wait_bit(EC_SR_OBF, EC_SR_OBF); }

void ec_init(uint16_t cmd_port, uint16_t data_port)
{
    g_ec.cmd = cmd_port;
    g_ec.data = data_port;
    g_ec.present = 1;
}

int ec_present(void)        { return g_ec.present; }
uint16_t ec_cmd_port(void)  { return g_ec.cmd; }
uint16_t ec_data_port(void) { return g_ec.data; }
uint64_t ec_ops_read(void)    { return g_ec.ops_read; }
uint64_t ec_ops_write(void)   { return g_ec.ops_write; }
uint64_t ec_ops_timeout(void) { return g_ec.ops_timeout; }

int ec_read(uint8_t offset, uint8_t *out)
{
    if (!g_ec.present || !out) return -1;
    uint64_t flags = spin_lock_irqsave(&g_ec.lock);
    int rc = -1;

    /* Drain any stale OBF before issuing a new command. */
    if (hal_in8(g_ec.cmd) & EC_SR_OBF) (void)hal_in8(g_ec.data);

    if (ec_wait_ibf_clear() != 0) goto done;
    hal_out8(g_ec.cmd, EC_CMD_RD_EC);
    if (ec_wait_ibf_clear() != 0) goto done;
    hal_out8(g_ec.data, offset);
    if (ec_wait_obf_set() != 0) goto done;
    *out = hal_in8(g_ec.data);
    g_ec.ops_read++;
    rc = 0;

done:
    spin_unlock_irqrestore(&g_ec.lock, flags);
    return rc;
}

int ec_write(uint8_t offset, uint8_t value)
{
    if (!g_ec.present) return -1;
    uint64_t flags = spin_lock_irqsave(&g_ec.lock);
    int rc = -1;

    if (ec_wait_ibf_clear() != 0) goto done;
    hal_out8(g_ec.cmd, EC_CMD_WR_EC);
    if (ec_wait_ibf_clear() != 0) goto done;
    hal_out8(g_ec.data, offset);
    if (ec_wait_ibf_clear() != 0) goto done;
    hal_out8(g_ec.data, value);
    if (ec_wait_ibf_clear() != 0) goto done;
    g_ec.ops_write++;
    rc = 0;

done:
    spin_unlock_irqrestore(&g_ec.lock, flags);
    return rc;
}

int ec_read_block(uint8_t offset, uint8_t *buf, int len)
{
    if (!g_ec.present || !buf || len <= 0) return -1;
    for (int i = 0; i < len; i++) {
        if (ec_read((uint8_t)(offset + i), &buf[i]) != 0) return -1;
    }
    return 0;
}

/* ── AML interpreter callbacks ──────────────────────────────────── */

int aml_ec_region_read(uint64_t base, uint32_t byte_off, int access_bytes,
                       uint64_t *out)
{
    (void)access_bytes;
    if (!out) return -1;
    uint8_t v = 0;
    uint32_t off = (uint32_t)(base + byte_off);
    if (off > 0xFF) return -1;  /* EC space is 256 bytes */
    int rc = ec_read((uint8_t)off, &v);
    if (rc == 0) {
        *out = v;
        /* Trace: [aml/ec] read offset=0xNN -> 0xNN
         * Only emitted when serial debug is on; kputs goes to serial. */
#ifdef ZEOS_AML_EC_TRACE
        kputs("[aml/ec] read offset=0x"); kput_hex(off);
        kputs(" -> 0x"); kput_hex(v); kputs("\n");
#endif
    }
    return rc;
}

int aml_ec_region_write(uint64_t base, uint32_t byte_off, int access_bytes,
                        uint64_t val)
{
    (void)access_bytes;
    uint32_t off = (uint32_t)(base + byte_off);
    if (off > 0xFF) return -1;
    int rc = ec_write((uint8_t)off, (uint8_t)(val & 0xFF));
#ifdef ZEOS_AML_EC_TRACE
    if (rc == 0) {
        kputs("[aml/ec] write offset=0x"); kput_hex(off);
        kputs(" <- 0x"); kput_hex((uint8_t)val); kputs("\n");
    }
#endif
    return rc;
}

/* ── Discovery ──────────────────────────────────────────────────── */

/* Encode "PNPxxxx" as the EISA-style packed integer used by some
 * firmware in `Name(_HID, EisaId(...))`. The packed encoding for
 * "PNP0C09" is little-endian-stored as 0x0C094150. */
static int matches_pnp0c09(const aml_object_t *o)
{
    if (!o) return 0;
    if (o->type == AML_T_STRING && o->u.str.s) {
        const char *s = o->u.str.s;
        return (s[0]=='P' && s[1]=='N' && s[2]=='P' &&
                s[3]=='0' && s[4]=='C' && s[5]=='0' && s[6]=='9' && s[7]==0);
    }
    if (o->type == AML_T_INTEGER) {
        /* EisaId("PNP0C09") = 0x0C094150 */
        return (o->u.integer & 0xFFFFFFFFu) == 0x0C094150u;
    }
    return 0;
}

/* Try to parse `_CRS` for the given device and extract the IO ranges
 * that map to the EC command and data ports. The _CRS resource buffer
 * uses ACPI Resource Descriptors (small + large item formats). The EC
 * historically uses two adjacent IO descriptors:
 *
 *   IO Port Descriptor (Small, type 0x47): _MIN, _MAX, _ALN, _LEN
 *
 * We pull two of them — first = data port, second = cmd port — per
 * ACPI 6.x §6.4.2.5 / §12.11. If parse fails, returns -1. */
static int parse_crs_io_ports(const aml_object_t *crs,
                              uint16_t *out_data, uint16_t *out_cmd)
{
    if (!crs || crs->type != AML_T_BUFFER) return -1;
    const uint8_t *p = crs->u.buf.bytes;
    uint32_t n = crs->u.buf.len;
    if (!p || n == 0) return -1;

    uint16_t ports[4];
    int port_count = 0;

    uint32_t i = 0;
    while (i < n && port_count < 4) {
        uint8_t tag = p[i];
        if (tag == 0x79) {
            break;  /* End-tag */
        }
        if (tag & 0x80) {
            /* Large item: tag(1) + len_lo(1) + len_hi(1) + data... */
            if (i + 3 > n) break;
            uint16_t llen = p[i+1] | ((uint16_t)p[i+2] << 8);
            i += 3 + llen;
        } else {
            /* Small item: tag has length in low 3 bits. */
            uint8_t name = (tag >> 3) & 0x0F;
            uint8_t slen = tag & 0x07;
            if (i + 1 + slen > n) break;
            if (name == 0x08) {
                /* IO Port Descriptor: _DEC(1) _MIN(2) _MAX(2) _ALN(1) _LEN(1) */
                if (slen >= 7) {
                    uint16_t min = p[i+2] | ((uint16_t)p[i+3] << 8);
                    if (port_count < 4) ports[port_count++] = min;
                }
            } else if (name == 0x09) {
                /* Fixed Location IO Port: _BAS(2) _LEN(1) */
                if (slen >= 3) {
                    uint16_t bas = p[i+1] | ((uint16_t)p[i+2] << 8);
                    if (port_count < 4) ports[port_count++] = bas;
                }
            }
            i += 1 + slen;
        }
    }

    if (port_count >= 2) {
        /* Per ACPI §12.11: the first IO resource is the data port,
         * the second is the command/status port. */
        *out_data = ports[0];
        *out_cmd  = ports[1];
        return 0;
    }
    return -1;
}

int ec_discover_and_init(void)
{
    int total = aml_sym_total();
    char path[64];
    char hid_path[80];
    char crs_path[80];

    int found = 0;
    uint16_t cmd = 0x66, data = 0x62;

    for (int i = 0; i < total; i++) {
        aml_sym_kind_t k;
        if (aml_sym_info(i, &k, path, sizeof(path)) != 0) continue;
        if (k != AML_SYM_DEVICE) continue;

        /* Build "<path>._HID" and try to evaluate. */
        int j = 0;
        while (path[j] && j < (int)sizeof(hid_path) - 6) { hid_path[j] = path[j]; j++; }
        if (j >= (int)sizeof(hid_path) - 6) continue;
        hid_path[j++] = '.';
        hid_path[j++] = '_';
        hid_path[j++] = 'H';
        hid_path[j++] = 'I';
        hid_path[j++] = 'D';
        hid_path[j] = 0;

        aml_object_t hid = { .type = AML_T_UNINIT };
        int rc = aml_evaluate(hid_path, 0, 0, &hid);
        int hit = (rc == 0) && matches_pnp0c09(&hid);
        aml_object_release(&hid);
        if (!hit) continue;

        /* Found PNP0C09. Try _CRS for ports. */
        found = 1;
        int cj = 0;
        while (path[cj] && cj < (int)sizeof(crs_path) - 6) { crs_path[cj] = path[cj]; cj++; }
        crs_path[cj++] = '.';
        crs_path[cj++] = '_';
        crs_path[cj++] = 'C';
        crs_path[cj++] = 'R';
        crs_path[cj++] = 'S';
        crs_path[cj] = 0;

        aml_object_t crs = { .type = AML_T_UNINIT };
        if (aml_evaluate(crs_path, 0, 0, &crs) == 0) {
            uint16_t pd = 0, pc = 0;
            if (parse_crs_io_ports(&crs, &pd, &pc) == 0) {
                data = pd;
                cmd = pc;
                g_ec.crs_parsed = 1;
            } else {
                kputs("[ec] _CRS parse failed; using defaults 0x66/0x62\n");
            }
        } else {
            kputs("[ec] _CRS not evaluable; using defaults 0x66/0x62\n");
        }
        aml_object_release(&crs);
        break;
    }

    if (found) {
        ec_init(cmd, data);
    }
    return found;
}

void ec_print_selftest_line(void)
{
    if (!g_ec.present) {
        kputs("EC .................... not present (no _HID PNP0C09)\n");
        return;
    }
    kputs("EC .................... ports=0x");
    kput_hex(g_ec.cmd);
    kputs("/0x");
    kput_hex(g_ec.data);
    kputs(", AML EC region=ready");
    if (!g_ec.crs_parsed) {
        kputs(" (defaults — _CRS not parsed)");
    }
    kputs("\n");
}
