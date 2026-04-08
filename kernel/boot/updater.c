/*
 * Zeos -- OTA Updater
 *
 * The entire OS is one file: BOOTZ.EFI.
 * Updates = download new binary, write to ESP, reboot.
 *
 * Flow:
 *   1. updater_check()    — GET version.txt from update server
 *   2. updater_download() — GET BOOTZ.EFI into heap buffer
 *   3. updater_apply()    — find ESP on NVMe, overwrite BOOTX64.EFI
 *   4. Reboot.
 *
 * Alpha: placeholder update server URL. Mechanism is real.
 */

#include "updater.h"
#include "nvme.h"
#include "net_http.h"
#include "net_tls.h"
#include "vault_disk.h"
#include "fb.h"
#include "font.h"
#include "theme.h"
#include "kprint.h"
#include "heap.h"
#include "timer.h"

/* ── Static state ─────────────────────────────────────────────── */

static updater_state_t ustate;

/* Downloaded binary buffer */
static uint8_t *download_buf = NULL;
static uint32_t download_size = 0;

/* Alpha: max binary size = 16 MB (BOOTZ.EFI is a few MB) */
#define MAX_BINARY_SIZE  (16 * 1024 * 1024)

/* Alpha: default update server */
#define DEFAULT_UPDATE_HOST  "updates.zeos-os.com"
#define DEFAULT_UPDATE_PATH  "/alpha/BOOTZ.EFI"

/* ── String helpers (no libc) ─────────────────────────────────── */

static int str_len(const char *s)
{
    int n = 0;
    while (s[n]) n++;
    return n;
}

static void str_copy(char *dst, const char *src, int max)
{
    int i = 0;
    while (i < max - 1 && src[i]) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static void str_append(char *a, const char *b, int max)
{
    int alen = str_len(a);
    int i = 0;
    while (alen + i < max - 1 && b[i]) {
        a[alen + i] = b[i];
        i++;
    }
    a[alen + i] = '\0';
}

static void mem_set(void *dst, uint8_t val, uint64_t n)
{
    uint8_t *d = (uint8_t *)dst;
    for (uint64_t i = 0; i < n; i++)
        d[i] = val;
}

static void mem_copy(void *dst, const void *src, uint64_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (uint64_t i = 0; i < n; i++)
        d[i] = s[i];
}

static int mem_cmp(const void *a, const void *b, uint64_t n)
{
    const uint8_t *pa = (const uint8_t *)a;
    const uint8_t *pb = (const uint8_t *)b;
    for (uint64_t i = 0; i < n; i++) {
        if (pa[i] != pb[i])
            return (int)pa[i] - (int)pb[i];
    }
    return 0;
}

/* Integer to decimal string */
static void int_to_str(uint64_t val, char *buf, int bufsz)
{
    char tmp[24];
    int i = 0;
    if (val == 0) {
        tmp[i++] = '0';
    } else {
        while (val > 0 && i < 22) {
            tmp[i++] = '0' + (val % 10);
            val /= 10;
        }
    }
    int j = 0;
    while (i > 0 && j < bufsz - 1)
        buf[j++] = tmp[--i];
    buf[j] = '\0';
}

/*
 * Compare version strings: "major.minor.patch"
 * Returns >0 if a > b, <0 if a < b, 0 if equal.
 * Simple left-to-right numeric compare. Fine for Alpha.
 */
static int version_compare(const char *a, const char *b)
{
    int av[3] = {0, 0, 0};
    int bv[3] = {0, 0, 0};

    /* Parse a */
    int idx = 0;
    for (int i = 0; a[i] && idx < 3; i++) {
        if (a[i] == '.')
            idx++;
        else if (a[i] >= '0' && a[i] <= '9')
            av[idx] = av[idx] * 10 + (a[i] - '0');
    }

    /* Parse b */
    idx = 0;
    for (int i = 0; b[i] && idx < 3; i++) {
        if (b[i] == '.')
            idx++;
        else if (b[i] >= '0' && b[i] <= '9')
            bv[idx] = bv[idx] * 10 + (b[i] - '0');
    }

    for (int i = 0; i < 3; i++) {
        if (av[i] != bv[i])
            return av[i] - bv[i];
    }
    return 0;
}

/* Strip leading/trailing whitespace and newlines from a version string */
static void strip_version(char *dst, const char *src, int max)
{
    /* Skip leading whitespace */
    while (*src == ' ' || *src == '\t' || *src == '\n' || *src == '\r')
        src++;
    int i = 0;
    while (i < max - 1 && src[i] && src[i] != '\n' && src[i] != '\r') {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
    /* Trim trailing spaces */
    while (i > 0 && (dst[i - 1] == ' ' || dst[i - 1] == '\t')) {
        i--;
        dst[i] = '\0';
    }
}

/* ── GPT structures (same as installer.c) ─────────────────────── */

typedef struct __attribute__((packed)) {
    uint8_t  signature[8];
    uint32_t revision;
    uint32_t header_size;
    uint32_t header_crc32;
    uint32_t reserved;
    uint64_t my_lba;
    uint64_t alt_lba;
    uint64_t first_usable_lba;
    uint64_t last_usable_lba;
    uint8_t  disk_guid[16];
    uint64_t part_entry_lba;
    uint32_t num_parts;
    uint32_t part_entry_size;
    uint32_t part_crc32;
} gpt_header_t;

typedef struct __attribute__((packed)) {
    uint8_t  type_guid[16];
    uint8_t  unique_guid[16];
    uint64_t first_lba;
    uint64_t last_lba;
    uint64_t attributes;
    uint16_t name[36];
} gpt_entry_t;

/* EFI System Partition type GUID: C12A7328-F81F-11D2-BA4B-00A0C93EC93B */
static const uint8_t ESP_TYPE_GUID[16] = {
    0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11,
    0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B
};

/* ── Init ─────────────────────────────────────────────────────── */

void updater_init(const char *current_version)
{
    mem_set(&ustate, 0, sizeof(ustate));
    ustate.state = UPDATE_IDLE;
    ustate.progress_pct = 0;
    if (current_version)
        str_copy(ustate.current_version, current_version,
                 sizeof(ustate.current_version));
    else
        str_copy(ustate.current_version, "0.0.0",
                 sizeof(ustate.current_version));

    download_buf = NULL;
    download_size = 0;

    kputs("[updater] Initialized, current version: ");
    kputs(ustate.current_version);
    kputc('\n');
}

/* ── Check for update ─────────────────────────────────────────── */

int updater_check(const char *update_server)
{
    const char *host = update_server ? update_server : DEFAULT_UPDATE_HOST;

    ustate.state = UPDATE_CHECKING;
    ustate.progress_pct = 0;
    mem_set(ustate.error_msg, 0, sizeof(ustate.error_msg));
    mem_set(ustate.available_version, 0, sizeof(ustate.available_version));

    kputs("[updater] Checking for updates at ");
    kputs(host);
    kputs("/version.txt\n");

    /*
     * Try HTTPS first, fall back to HTTP.
     * version.txt format: single line, e.g. "0.1.1\n"
     */
    char resp_buf[512];
    int body_len = 0;
    int status = -1;

    /* HTTPS attempt */
    status = https_get(host, "/version.txt", resp_buf, sizeof(resp_buf),
                       &body_len);

    if (status < 0) {
        /* Fall back to plain HTTP */
        kputs("[updater] HTTPS failed, trying HTTP\n");
        struct http_response http_resp;
        if (http_get(host, "/version.txt", &http_resp) == 0) {
            status = http_resp.status_code;
            body_len = http_resp.body_len;
            if (body_len > 0 && body_len < (int)sizeof(resp_buf))
                mem_copy(resp_buf, http_resp.body, (uint64_t)body_len);
            resp_buf[body_len < (int)sizeof(resp_buf) ? body_len : (int)sizeof(resp_buf) - 1] = '\0';
        }
    } else {
        resp_buf[body_len < (int)sizeof(resp_buf) ? body_len : (int)sizeof(resp_buf) - 1] = '\0';
    }

    if (status != 200) {
        ustate.state = UPDATE_ERROR;
        str_copy(ustate.error_msg, "Failed to fetch version.txt (HTTP ",
                 sizeof(ustate.error_msg));
        if (status > 0) {
            char code_str[8];
            int_to_str((uint64_t)status, code_str, sizeof(code_str));
            str_append(ustate.error_msg, code_str, sizeof(ustate.error_msg));
        } else {
            str_append(ustate.error_msg, "connect failed",
                       sizeof(ustate.error_msg));
        }
        str_append(ustate.error_msg, ")", sizeof(ustate.error_msg));
        kputs("[updater] ");
        kputs(ustate.error_msg);
        kputc('\n');
        return -1;
    }

    /* Parse version from response body */
    char remote_ver[32];
    strip_version(remote_ver, resp_buf, sizeof(remote_ver));
    str_copy(ustate.available_version, remote_ver,
             sizeof(ustate.available_version));

    kputs("[updater] Remote version: ");
    kputs(remote_ver);
    kputs("  Local: ");
    kputs(ustate.current_version);
    kputc('\n');

    /* Compare versions */
    int cmp = version_compare(remote_ver, ustate.current_version);
    if (cmp > 0) {
        ustate.state = UPDATE_AVAILABLE;

        /* Build download URL: host + /alpha/BOOTZ.EFI */
        str_copy(ustate.update_url, host, sizeof(ustate.update_url));
        str_append(ustate.update_url, DEFAULT_UPDATE_PATH,
                   sizeof(ustate.update_url));

        kputs("[updater] Update available: ");
        kputs(remote_ver);
        kputc('\n');
    } else {
        ustate.state = UPDATE_IDLE;
        kputs("[updater] Up to date.\n");
    }

    return 0;
}

/* ── Download update ──────────────────────────────────────────── */

int updater_download(void)
{
    if (ustate.state != UPDATE_AVAILABLE) {
        str_copy(ustate.error_msg, "No update available to download",
                 sizeof(ustate.error_msg));
        ustate.state = UPDATE_ERROR;
        return -1;
    }

    ustate.state = UPDATE_DOWNLOADING;
    ustate.progress_pct = 0;
    mem_set(ustate.error_msg, 0, sizeof(ustate.error_msg));

    kputs("[updater] Downloading BOOTZ.EFI from ");
    kputs(ustate.update_url);
    kputc('\n');

    /* Parse host from update_url (stored as "host/path") */
    char host[128];
    char path[128];
    mem_set(host, 0, sizeof(host));
    mem_set(path, 0, sizeof(path));

    int slash_pos = -1;
    for (int i = 0; ustate.update_url[i]; i++) {
        if (ustate.update_url[i] == '/') {
            slash_pos = i;
            break;
        }
    }

    if (slash_pos > 0) {
        str_copy(host, ustate.update_url, slash_pos + 1 < (int)sizeof(host) ?
                 slash_pos + 1 : (int)sizeof(host));
        str_copy(path, ustate.update_url + slash_pos, sizeof(path));
    } else {
        str_copy(host, ustate.update_url, sizeof(host));
        str_copy(path, DEFAULT_UPDATE_PATH, sizeof(path));
    }

    /* Allocate download buffer */
    if (download_buf) {
        kfree(download_buf);
        download_buf = NULL;
    }
    download_buf = (uint8_t *)kmalloc(MAX_BINARY_SIZE);
    if (!download_buf) {
        ustate.state = UPDATE_ERROR;
        str_copy(ustate.error_msg, "Out of memory for download buffer",
                 sizeof(ustate.error_msg));
        kputs("[updater] ");
        kputs(ustate.error_msg);
        kputc('\n');
        return -1;
    }
    download_size = 0;

    /*
     * Download via TLS (streaming).
     * We use the raw TLS API for large downloads since https_get()
     * has a limited response buffer. Send HTTP/1.1 GET manually,
     * read the body in chunks.
     */
    tls_conn_t *conn = tls_connect(host, 443);
    if (!conn) {
        /* Fall back: try plain HTTP on port 80 */
        kputs("[updater] TLS connect failed, trying HTTP fallback\n");

        struct http_response resp;
        if (http_get(host, path, &resp) == 0 && resp.status_code == 200) {
            /* HTTP body is small (16KB max). Won't fit a real binary.
             * This path is for testing only. */
            if (resp.body_len > 0) {
                mem_copy(download_buf, resp.body, (uint64_t)resp.body_len);
                download_size = (uint32_t)resp.body_len;
                ustate.progress_pct = 100;
            }
        } else {
            ustate.state = UPDATE_ERROR;
            str_copy(ustate.error_msg, "Download failed: connection error",
                     sizeof(ustate.error_msg));
            kputs("[updater] ");
            kputs(ustate.error_msg);
            kputc('\n');
            kfree(download_buf);
            download_buf = NULL;
            return -1;
        }

        goto verify;
    }

    /* Send HTTP/1.1 GET request over TLS */
    {
        char request[384];
        str_copy(request, "GET ", sizeof(request));
        str_append(request, path, sizeof(request));
        str_append(request, " HTTP/1.1\r\nHost: ", sizeof(request));
        str_append(request, host, sizeof(request));
        str_append(request, "\r\nConnection: close\r\n\r\n", sizeof(request));

        int req_len = str_len(request);
        if (tls_send(conn, request, req_len) < 0) {
            ustate.state = UPDATE_ERROR;
            str_copy(ustate.error_msg, "TLS send failed",
                     sizeof(ustate.error_msg));
            tls_close(conn);
            kfree(download_buf);
            download_buf = NULL;
            return -1;
        }
    }

    /*
     * Read response. Parse headers to get Content-Length,
     * then stream body into download_buf.
     */
    {
        /* Read headers first (up to 4KB) */
        char hdr_buf[4096];
        int hdr_len = 0;
        int hdr_done = 0;
        uint32_t content_length = 0;
        int body_start = 0;

        while (!hdr_done && hdr_len < (int)sizeof(hdr_buf) - 1) {
            int n = tls_recv(conn, hdr_buf + hdr_len,
                             (int)sizeof(hdr_buf) - 1 - hdr_len);
            if (n <= 0) break;
            hdr_len += n;
            hdr_buf[hdr_len] = '\0';

            /* Look for end of headers: \r\n\r\n */
            for (int i = 0; i < hdr_len - 3; i++) {
                if (hdr_buf[i] == '\r' && hdr_buf[i+1] == '\n' &&
                    hdr_buf[i+2] == '\r' && hdr_buf[i+3] == '\n') {
                    hdr_done = 1;
                    body_start = i + 4;
                    break;
                }
            }
        }

        /* Parse Content-Length from headers */
        for (int i = 0; i < hdr_len - 16; i++) {
            if ((hdr_buf[i]   == 'C' || hdr_buf[i]   == 'c') &&
                (hdr_buf[i+1] == 'o' || hdr_buf[i+1] == 'O') &&
                (hdr_buf[i+8] == 'L' || hdr_buf[i+8] == 'l') &&
                hdr_buf[i+14] == ':') {
                /* "Content-Length: NNN" */
                int k = i + 15;
                while (k < hdr_len && hdr_buf[k] == ' ') k++;
                while (k < hdr_len && hdr_buf[k] >= '0' && hdr_buf[k] <= '9') {
                    content_length = content_length * 10 +
                                     (uint32_t)(hdr_buf[k] - '0');
                    k++;
                }
                break;
            }
        }

        if (content_length == 0)
            content_length = MAX_BINARY_SIZE; /* Unknown size, read until EOF */

        if (content_length > MAX_BINARY_SIZE) {
            ustate.state = UPDATE_ERROR;
            str_copy(ustate.error_msg, "Binary too large (>16MB)",
                     sizeof(ustate.error_msg));
            tls_close(conn);
            kfree(download_buf);
            download_buf = NULL;
            return -1;
        }

        /* Copy any body bytes already read with the headers */
        int initial_body = hdr_len - body_start;
        if (initial_body > 0) {
            mem_copy(download_buf, hdr_buf + body_start, (uint64_t)initial_body);
            download_size = (uint32_t)initial_body;
        }

        /* Stream remaining body */
        while (download_size < content_length) {
            int chunk = (int)(content_length - download_size);
            if (chunk > 8192) chunk = 8192;

            int n = tls_recv(conn, download_buf + download_size, chunk);
            if (n <= 0) break; /* EOF or error */
            download_size += (uint32_t)n;

            /* Update progress */
            if (content_length > 0)
                ustate.progress_pct = (int)((uint64_t)download_size * 100 /
                                            content_length);
        }

        tls_close(conn);
    }

verify:
    kputs("[updater] Downloaded ");
    kput_dec(download_size);
    kputs(" bytes\n");

    /* Verify EFI magic: MZ header (0x4D, 0x5A) */
    if (download_size < 64 || download_buf[0] != 0x4D ||
        download_buf[1] != 0x5A) {
        ustate.state = UPDATE_ERROR;
        str_copy(ustate.error_msg, "Invalid binary (bad MZ header)",
                 sizeof(ustate.error_msg));
        kputs("[updater] ");
        kputs(ustate.error_msg);
        kputc('\n');
        kfree(download_buf);
        download_buf = NULL;
        download_size = 0;
        return -1;
    }

    ustate.progress_pct = 100;
    kputs("[updater] EFI binary verified (MZ header OK)\n");
    return 0;
}

/* ── Find ESP on NVMe via GPT ─────────────────────────────────── */

/*
 * Scan GPT for the EFI System Partition.
 * Returns the starting LBA and sets *out_blocks to partition size.
 * Returns 0 on failure (LBA 0 is never a valid ESP start).
 */
static uint64_t find_esp(uint64_t *out_blocks)
{
    const nvme_dev_t *dev = nvme_get_dev();
    if (!dev || !dev->ready) {
        kputs("[updater] NVMe not ready\n");
        return 0;
    }

    uint32_t blk_size = dev->block_size;
    if (blk_size == 0) blk_size = 512;

    /* Read GPT header at LBA 1 */
    uint8_t *buf = (uint8_t *)kmalloc(blk_size);
    if (!buf) return 0;

    if (nvme_read(1, 1, buf) != 0) {
        kputs("[updater] Failed to read GPT header\n");
        kfree(buf);
        return 0;
    }

    gpt_header_t *gpt = (gpt_header_t *)buf;

    /* Verify GPT signature "EFI PART" */
    const uint8_t efi_sig[8] = {'E','F','I',' ','P','A','R','T'};
    if (mem_cmp(gpt->signature, efi_sig, 8) != 0) {
        kputs("[updater] No GPT signature found\n");
        kfree(buf);
        return 0;
    }

    uint64_t part_lba = gpt->part_entry_lba;
    uint32_t num_parts = gpt->num_parts;
    if (num_parts > 128) num_parts = 128;

    kfree(buf);

    /* Read partition entries (128 bytes each, typically at LBA 2) */
    uint32_t entries_size = num_parts * 128;
    uint32_t entries_blocks = (entries_size + blk_size - 1) / blk_size;

    uint8_t *entries = (uint8_t *)kmalloc((uint64_t)entries_blocks * blk_size);
    if (!entries) return 0;

    if (nvme_read(part_lba, entries_blocks, entries) != 0) {
        kputs("[updater] Failed to read GPT entries\n");
        kfree(entries);
        return 0;
    }

    /* Scan for ESP type GUID */
    uint64_t esp_lba = 0;
    uint64_t esp_blocks_out = 0;

    for (uint32_t i = 0; i < num_parts; i++) {
        gpt_entry_t *e = (gpt_entry_t *)(entries + i * 128);

        /* Skip empty entries (all-zero type GUID) */
        int empty = 1;
        for (int j = 0; j < 16; j++) {
            if (e->type_guid[j] != 0) { empty = 0; break; }
        }
        if (empty) continue;

        if (mem_cmp(e->type_guid, ESP_TYPE_GUID, 16) == 0) {
            esp_lba = e->first_lba;
            esp_blocks_out = e->last_lba - e->first_lba + 1;
            kputs("[updater] Found ESP at LBA ");
            kput_dec(esp_lba);
            kputs(", size ");
            kput_dec(esp_blocks_out);
            kputs(" blocks\n");
            break;
        }
    }

    kfree(entries);
    if (out_blocks) *out_blocks = esp_blocks_out;
    return esp_lba;
}

/* ── FAT32 file locator ───────────────────────────────────────── */

/*
 * Find BOOTX64.EFI in the FAT32 ESP and return its:
 *   - Directory entry LBA and offset (for size update)
 *   - Starting data cluster
 *   - FAT32 layout info for cluster chain manipulation
 *
 * This mirrors the FAT32 layout created by installer.c:
 *   Reserved = 32 sectors, cluster size = 8 sectors, 2 FATs
 *   Root dir = cluster 2, /EFI = cluster 3, /EFI/BOOT = cluster 4
 *   BOOTX64.EFI starts at cluster 5
 */
typedef struct {
    uint64_t esp_start_lba;
    uint32_t blk_size;
    uint32_t sectors_per_block;
    uint32_t sectors_per_cluster;
    uint32_t reserved_sectors;
    uint64_t fat1_lba;
    uint64_t fat2_lba;
    uint64_t data_lba;
    uint64_t boot_dir_lba;     /* LBA of /EFI/BOOT directory */
    int      dir_entry_offset; /* Byte offset of BOOTX64.EFI entry within block */
    uint32_t start_cluster;    /* First data cluster of BOOTX64.EFI */
} fat32_info_t;

static int find_bootx64(uint64_t esp_start_lba, fat32_info_t *info)
{
    const nvme_dev_t *dev = nvme_get_dev();
    if (!dev) return -1;

    uint32_t blk_size = dev->block_size;
    if (blk_size == 0) blk_size = 512;

    info->esp_start_lba = esp_start_lba;
    info->blk_size = blk_size;
    info->sectors_per_block = blk_size / 512;
    if (info->sectors_per_block == 0) info->sectors_per_block = 1;
    info->sectors_per_cluster = 8;
    info->reserved_sectors = 32;

    /* Calculate FAT layout (matches installer.c) */
    uint64_t esp_sectors = (100ULL * 1024 * 1024) / 512; /* 100MB ESP */
    uint32_t total_clusters = (uint32_t)(esp_sectors - info->reserved_sectors) /
                              info->sectors_per_cluster;
    uint32_t fat_bytes = (total_clusters + 2) * 4;
    uint32_t fat_sectors = (fat_bytes + 511) / 512;

    info->fat1_lba = esp_start_lba +
                     (info->reserved_sectors / info->sectors_per_block);
    if (blk_size == 512)
        info->fat1_lba = esp_start_lba + info->reserved_sectors;

    info->fat2_lba = info->fat1_lba + (fat_sectors / info->sectors_per_block);
    if (blk_size == 512)
        info->fat2_lba = info->fat1_lba + fat_sectors;

    info->data_lba = info->fat2_lba + (fat_sectors / info->sectors_per_block);
    if (blk_size == 512)
        info->data_lba = info->fat2_lba + fat_sectors;

    /* /EFI dir = cluster 3 = data_lba + 1 cluster */
    uint64_t efi_dir_lba = info->data_lba +
                           (info->sectors_per_cluster / info->sectors_per_block);
    if (efi_dir_lba == info->data_lba) efi_dir_lba = info->data_lba + 1;

    /* /EFI/BOOT dir = cluster 4 */
    info->boot_dir_lba = efi_dir_lba +
                         (info->sectors_per_cluster / info->sectors_per_block);
    if (info->boot_dir_lba == efi_dir_lba)
        info->boot_dir_lba = efi_dir_lba + 1;

    /* Read /EFI/BOOT directory, find BOOTX64.EFI entry */
    uint8_t *buf = (uint8_t *)kmalloc(blk_size);
    if (!buf) return -1;

    if (nvme_read(info->boot_dir_lba, 1, buf) != 0) {
        kputs("[updater] Failed to read BOOT dir\n");
        kfree(buf);
        return -1;
    }

    /* Scan 8.3 directory entries (32 bytes each) */
    int found = 0;
    int entries_per_block = (int)(blk_size / 32);
    for (int i = 0; i < entries_per_block; i++) {
        uint8_t *e = buf + i * 32;
        if (e[0] == 0x00) break;    /* End of directory */
        if (e[0] == 0xE5) continue; /* Deleted */
        if (e[11] == 0x0F) continue; /* LFN entry */

        /* Compare 8.3 name: "BOOTX64 EFI" */
        if (mem_cmp(e, "BOOTX64 EFI", 11) == 0) {
            info->dir_entry_offset = i * 32;
            info->start_cluster = (uint32_t)e[26] | ((uint32_t)e[27] << 8) |
                                  ((uint32_t)e[20] << 16) | ((uint32_t)e[21] << 24);
            found = 1;
            kputs("[updater] Found BOOTX64.EFI at cluster ");
            kput_dec(info->start_cluster);
            kputc('\n');
            break;
        }
    }

    kfree(buf);

    if (!found) {
        kputs("[updater] BOOTX64.EFI not found in /EFI/BOOT\n");
        return -1;
    }

    return 0;
}

/* ── Apply update ─────────────────────────────────────────────── */

int updater_apply(void)
{
    if (!download_buf || download_size == 0) {
        ustate.state = UPDATE_ERROR;
        str_copy(ustate.error_msg, "No downloaded binary to apply",
                 sizeof(ustate.error_msg));
        return -1;
    }

    ustate.state = UPDATE_WRITING;
    ustate.progress_pct = 0;
    mem_set(ustate.error_msg, 0, sizeof(ustate.error_msg));

    kputs("[updater] Applying update...\n");

    /* Step 1: Find ESP */
    uint64_t esp_blocks = 0;
    uint64_t esp_lba = find_esp(&esp_blocks);
    if (esp_lba == 0) {
        ustate.state = UPDATE_ERROR;
        str_copy(ustate.error_msg, "ESP partition not found",
                 sizeof(ustate.error_msg));
        return -1;
    }

    /* Step 2: Find BOOTX64.EFI in FAT32 */
    fat32_info_t fi;
    mem_set(&fi, 0, sizeof(fi));
    if (find_bootx64(esp_lba, &fi) != 0) {
        ustate.state = UPDATE_ERROR;
        str_copy(ustate.error_msg, "BOOTX64.EFI not found on ESP",
                 sizeof(ustate.error_msg));
        return -1;
    }

    const nvme_dev_t *dev = nvme_get_dev();
    uint32_t blk_size = dev->block_size;
    if (blk_size == 0) blk_size = 512;

    /*
     * Step 3: Write binary data to clusters starting at start_cluster.
     *
     * Cluster N maps to LBA:
     *   data_lba + (N - 2) * sectors_per_cluster / sectors_per_block
     *
     * We write sequentially and build a FAT chain.
     */
    uint32_t bytes_per_cluster = fi.sectors_per_cluster * 512;
    uint32_t clusters_needed = (download_size + bytes_per_cluster - 1) /
                               bytes_per_cluster;
    uint32_t first_cluster = fi.start_cluster;
    if (first_cluster < 2) first_cluster = 5; /* Default from installer */

    kputs("[updater] Writing ");
    kput_dec(download_size);
    kputs(" bytes (");
    kput_dec(clusters_needed);
    kputs(" clusters) starting at cluster ");
    kput_dec(first_cluster);
    kputc('\n');

    /* Write data cluster by cluster */
    uint32_t written = 0;
    for (uint32_t c = 0; c < clusters_needed; c++) {
        uint32_t cluster = first_cluster + c;
        uint64_t cluster_lba = fi.data_lba +
            (uint64_t)(cluster - 2) * fi.sectors_per_cluster /
            fi.sectors_per_block;

        uint32_t chunk = download_size - written;
        if (chunk > bytes_per_cluster) chunk = bytes_per_cluster;

        /* Write full blocks covering this cluster */
        uint32_t blocks_per_cluster = (bytes_per_cluster + blk_size - 1) /
                                      blk_size;

        /* If this is the last partial cluster, zero-pad */
        if (chunk < bytes_per_cluster) {
            uint8_t *pad = (uint8_t *)kmalloc(bytes_per_cluster);
            if (!pad) {
                ustate.state = UPDATE_ERROR;
                str_copy(ustate.error_msg, "Out of memory during write",
                         sizeof(ustate.error_msg));
                return -1;
            }
            mem_set(pad, 0, bytes_per_cluster);
            mem_copy(pad, download_buf + written, chunk);
            if (nvme_write(cluster_lba, blocks_per_cluster, pad) != 0) {
                kfree(pad);
                ustate.state = UPDATE_ERROR;
                str_copy(ustate.error_msg, "NVMe write failed",
                         sizeof(ustate.error_msg));
                return -1;
            }
            kfree(pad);
        } else {
            if (nvme_write(cluster_lba, blocks_per_cluster,
                           download_buf + written) != 0) {
                ustate.state = UPDATE_ERROR;
                str_copy(ustate.error_msg, "NVMe write failed",
                         sizeof(ustate.error_msg));
                return -1;
            }
        }

        written += chunk;
        ustate.progress_pct = (int)((uint64_t)written * 80 / download_size);
    }

    /*
     * Step 4: Update FAT chain.
     * Read FAT1, set chain: cluster N -> N+1 -> ... -> EOC
     */
    {
        uint8_t *fat = (uint8_t *)kmalloc(blk_size);
        if (!fat) {
            ustate.state = UPDATE_ERROR;
            str_copy(ustate.error_msg, "Out of memory for FAT",
                     sizeof(ustate.error_msg));
            return -1;
        }

        /* Read the FAT block containing our cluster entries */
        uint32_t fat_offset = first_cluster * 4;
        uint32_t fat_block_offset = fat_offset / blk_size;
        uint64_t fat_lba = fi.fat1_lba + fat_block_offset;

        if (nvme_read(fat_lba, 1, fat) != 0) {
            kfree(fat);
            ustate.state = UPDATE_ERROR;
            str_copy(ustate.error_msg, "FAT read failed",
                     sizeof(ustate.error_msg));
            return -1;
        }

        uint32_t local_offset = fat_offset % blk_size;

        for (uint32_t c = 0; c < clusters_needed; c++) {
            uint32_t entry_off = local_offset + c * 4;

            /* If we'd overflow this block, flush and read next */
            if (entry_off + 4 > blk_size) {
                nvme_write(fat_lba, 1, fat);
                nvme_write(fi.fat2_lba + fat_block_offset, 1, fat);
                fat_block_offset++;
                fat_lba = fi.fat1_lba + fat_block_offset;
                nvme_read(fat_lba, 1, fat);
                local_offset = 0;
                entry_off = c * 4 - (fat_block_offset * blk_size -
                             first_cluster * 4);
                /* Recalculate offset within new block */
                entry_off = ((first_cluster + c) * 4) % blk_size;
            }

            uint32_t val;
            if (c == clusters_needed - 1)
                val = 0x0FFFFFFF; /* End of chain */
            else
                val = first_cluster + c + 1; /* Next cluster */

            fat[entry_off + 0] = (uint8_t)(val & 0xFF);
            fat[entry_off + 1] = (uint8_t)((val >> 8) & 0xFF);
            fat[entry_off + 2] = (uint8_t)((val >> 16) & 0xFF);
            fat[entry_off + 3] = (uint8_t)((val >> 24) & 0xFF);
        }

        /* Write updated FAT to both copies */
        nvme_write(fat_lba, 1, fat);
        nvme_write(fi.fat2_lba + fat_block_offset, 1, fat);
        kfree(fat);
    }

    ustate.progress_pct = 85;

    /*
     * Step 5: Update directory entry with new file size.
     */
    {
        uint8_t *dir = (uint8_t *)kmalloc(blk_size);
        if (!dir) {
            ustate.state = UPDATE_ERROR;
            str_copy(ustate.error_msg, "Out of memory for dir update",
                     sizeof(ustate.error_msg));
            return -1;
        }

        nvme_read(fi.boot_dir_lba, 1, dir);

        uint8_t *entry = dir + fi.dir_entry_offset;
        /* Update file size (bytes 28-31) */
        entry[28] = (uint8_t)(download_size & 0xFF);
        entry[29] = (uint8_t)((download_size >> 8) & 0xFF);
        entry[30] = (uint8_t)((download_size >> 16) & 0xFF);
        entry[31] = (uint8_t)((download_size >> 24) & 0xFF);

        /* Update start cluster (bytes 26-27 low, 20-21 high) */
        entry[26] = (uint8_t)(first_cluster & 0xFF);
        entry[27] = (uint8_t)((first_cluster >> 8) & 0xFF);
        entry[20] = (uint8_t)((first_cluster >> 16) & 0xFF);
        entry[21] = (uint8_t)((first_cluster >> 24) & 0xFF);

        nvme_write(fi.boot_dir_lba, 1, dir);
        kfree(dir);
    }

    ustate.progress_pct = 90;

    /*
     * Step 6: Verify by reading back the first block.
     */
    {
        uint8_t *verify = (uint8_t *)kmalloc(blk_size);
        if (verify) {
            uint64_t first_data_lba = fi.data_lba +
                (uint64_t)(first_cluster - 2) * fi.sectors_per_cluster /
                fi.sectors_per_block;

            nvme_read(first_data_lba, 1, verify);

            if (verify[0] != 0x4D || verify[1] != 0x5A) {
                kputs("[updater] WARNING: Read-back verification failed\n");
                ustate.state = UPDATE_ERROR;
                str_copy(ustate.error_msg,
                         "Verification failed (read-back mismatch)",
                         sizeof(ustate.error_msg));
                kfree(verify);
                return -1;
            }

            /* Compare first block */
            uint32_t cmp_size = blk_size < download_size ? blk_size :
                                download_size;
            if (mem_cmp(verify, download_buf, cmp_size) != 0) {
                kputs("[updater] WARNING: Read-back data mismatch\n");
                ustate.state = UPDATE_ERROR;
                str_copy(ustate.error_msg,
                         "Verification failed (data mismatch)",
                         sizeof(ustate.error_msg));
                kfree(verify);
                return -1;
            }

            kfree(verify);
            kputs("[updater] Read-back verification OK\n");
        }
    }

    /* Flush NVMe write cache */
    nvme_flush();

    ustate.progress_pct = 100;
    ustate.state = UPDATE_DONE;

    /* Free download buffer */
    kfree(download_buf);
    download_buf = NULL;
    download_size = 0;

    kputs("[updater] Update applied successfully. Reboot to activate.\n");
    return 0;
}

/* ── UI Drawing ───────────────────────────────────────────────── */

static void draw_progress_bar(int x, int y, int w, int h, int pct)
{
    /* Background track */
    fb_rect(x, y, w, h, COLOR_SURFACE_TOP);

    /* Filled portion */
    if (pct > 0) {
        int fill_w = (w * pct) / 100;
        if (fill_w > w) fill_w = w;
        fb_rect(x, y, fill_w, h, COLOR_PRIMARY);
    }

    /* Border */
    fb_rect_outline(x, y, w, h, COLOR_SEPARATOR, 1);
}

void updater_draw(int x, int y, int w, int h)
{
    int pad = CONTENT_PADDING;
    int line_h = font_line_height(FONT_UI, TYPE_BODY);
    int cy = y + pad;

    /* Title */
    font_draw(x + pad, cy, "System Update", FONT_UI_BOLD, TYPE_HEADING,
              COLOR_ON_SURFACE);
    cy += font_line_height(FONT_UI_BOLD, TYPE_HEADING) + Z4;

    /* Separator */
    fb_hline(x + pad, cy, w - pad * 2, COLOR_SEPARATOR);
    cy += Z2;

    /* Current version */
    {
        char line[64];
        str_copy(line, "Current:  v", sizeof(line));
        str_append(line, ustate.current_version, sizeof(line));
        font_draw(x + pad, cy, line, FONT_UI, TYPE_BODY,
                  COLOR_ON_SURFACE);
        cy += line_h + Z1;
    }

    /* Available version or status */
    switch (ustate.state) {
    case UPDATE_IDLE:
        font_draw(x + pad, cy, "Up to date", FONT_UI, TYPE_BODY,
                  COLOR_SUCCESS);
        cy += line_h + Z1;
        break;

    case UPDATE_CHECKING:
        font_draw(x + pad, cy, "Checking for updates...", FONT_UI,
                  TYPE_BODY, COLOR_ON_SURFACE_2);
        cy += line_h + Z1;
        break;

    case UPDATE_AVAILABLE:
    {
        char line[64];
        str_copy(line, "Available: v", sizeof(line));
        str_append(line, ustate.available_version, sizeof(line));
        font_draw(x + pad, cy, line, FONT_UI, TYPE_BODY, COLOR_PRIMARY);
        cy += line_h + Z1;
        break;
    }

    case UPDATE_DOWNLOADING:
    {
        char line[64];
        str_copy(line, "Downloading v", sizeof(line));
        str_append(line, ustate.available_version, sizeof(line));
        str_append(line, "...", sizeof(line));
        font_draw(x + pad, cy, line, FONT_UI, TYPE_BODY,
                  COLOR_ON_SURFACE_2);
        cy += line_h + Z2;

        /* Progress bar */
        draw_progress_bar(x + pad, cy, w - pad * 2, Z6, ustate.progress_pct);
        cy += Z6 + Z1;

        /* Percentage */
        char pct_str[8];
        int_to_str((uint64_t)ustate.progress_pct, pct_str, sizeof(pct_str));
        char pct_line[16];
        str_copy(pct_line, pct_str, sizeof(pct_line));
        str_append(pct_line, "%", sizeof(pct_line));
        font_draw(x + pad, cy, pct_line, FONT_CODE, TYPE_LABEL,
                  COLOR_ON_SURFACE_2);
        cy += line_h + Z1;
        break;
    }

    case UPDATE_WRITING:
    {
        font_draw(x + pad, cy, "Writing to ESP...", FONT_UI, TYPE_BODY,
                  COLOR_WARNING);
        cy += line_h + Z2;

        draw_progress_bar(x + pad, cy, w - pad * 2, Z6, ustate.progress_pct);
        cy += Z6 + Z1;

        char pct_str[8];
        int_to_str((uint64_t)ustate.progress_pct, pct_str, sizeof(pct_str));
        char pct_line[16];
        str_copy(pct_line, pct_str, sizeof(pct_line));
        str_append(pct_line, "%", sizeof(pct_line));
        font_draw(x + pad, cy, pct_line, FONT_CODE, TYPE_LABEL,
                  COLOR_ON_SURFACE_2);
        cy += line_h + Z1;
        break;
    }

    case UPDATE_DONE:
        font_draw(x + pad, cy, "Update installed", FONT_UI, TYPE_BODY,
                  COLOR_SUCCESS);
        cy += line_h + Z2;
        font_draw(x + pad, cy, "Reboot to complete update", FONT_UI,
                  TYPE_BODY, COLOR_ON_SURFACE_2);
        cy += line_h + Z1;
        break;

    case UPDATE_ERROR:
    {
        font_draw(x + pad, cy, "Update error:", FONT_UI, TYPE_BODY,
                  COLOR_DANGER);
        cy += line_h + Z1;
        font_draw(x + pad, cy, ustate.error_msg, FONT_UI, TYPE_LABEL,
                  COLOR_DANGER);
        cy += line_h + Z1;
        break;
    }
    }

    (void)h; /* Height available for scrolling if needed later */
}

/* ── State accessor ───────────────────────────────────────────── */

updater_state_t *updater_get_state(void)
{
    return &ustate;
}
