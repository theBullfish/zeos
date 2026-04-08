/*
 * Zeos -- OTA Updater
 *
 * The entire OS is one file: BOOTZ.EFI.
 * Download new binary from update server, write to ESP, reboot.
 *
 * Flow:
 *   1. updater_check() -- GET /zeos/version.txt, compare versions
 *   2. updater_download() -- GET /zeos/BOOTZ.EFI into heap buffer
 *   3. updater_apply() -- find ESP via GPT, overwrite BOOTX64.EFI
 *   4. Reboot.
 *
 * Bare-metal C. No libc. No package manager.
 */

#include "updater.h"
#include "net_http.h"
#include "net_tls.h"
#include "nvme.h"
#include "fb.h"
#include "font.h"
#include "theme.h"
#include "heap.h"
#include "kprint.h"

/* ── Constants ─────────────────────────────────────────────────── */

#define UPDATE_BUF_MAX    (4 * 1024 * 1024)  /* 4MB max for Alpha */
#define VERSION_MAX       32
#define BLOCK_SIZE        512                 /* Sector size for GPT parsing */

/* PE/COFF magic bytes (EFI binary) */
#define PE_MAGIC_0        0x4D  /* 'M' */
#define PE_MAGIC_1        0x5A  /* 'Z' */

/* ── Static state ──────────────────────────────────────────────── */

static updater_state_t state;

/* Downloaded binary buffer */
static uint8_t *dl_buf = 0;
static int      dl_len = 0;

/* ── String helpers (no libc) ──────────────────────────────────── */

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

static int str_eq(const char *a, const char *b)
{
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return *a == *b;
}

static int str_starts(const char *s, const char *prefix)
{
    while (*prefix) {
        if (*s != *prefix) return 0;
        s++; prefix++;
    }
    return 1;
}

static void str_append(char *dst, const char *src, int max)
{
    int dlen = str_len(dst);
    int i = 0;
    while (dlen + i < max - 1 && src[i]) {
        dst[dlen + i] = src[i];
        i++;
    }
    dst[dlen + i] = '\0';
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

static int mem_eq(const void *a, const void *b, uint64_t n)
{
    const uint8_t *pa = (const uint8_t *)a;
    const uint8_t *pb = (const uint8_t *)b;
    for (uint64_t i = 0; i < n; i++)
        if (pa[i] != pb[i]) return 0;
    return 1;
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

static void set_error(const char *msg)
{
    state.state = UPDATE_ERROR;
    str_copy(state.error_msg, msg, sizeof(state.error_msg));
    kputs("UPDATER ERROR: ");
    kputs(msg);
    kputs("\n");
}

/* ── URL parsing ───────────────────────────────────────────────── */

/*
 * Parse a URL into hostname, path, and TLS flag.
 * Supports http:// and https:// prefixes.
 */
static void parse_url(const char *url, char *hostname, int hmax,
                      char *path, int pmax, int *use_tls)
{
    *use_tls = 0;
    const char *p = url;

    if (str_starts(p, "https://")) { *use_tls = 1; p += 8; }
    else if (str_starts(p, "http://"))  { p += 7; }

    /* Hostname */
    int hi = 0;
    while (*p && *p != '/' && *p != ':' && hi < hmax - 1)
        hostname[hi++] = *p++;
    hostname[hi] = '\0';

    /* Skip port if present */
    if (*p == ':') { while (*p && *p != '/') p++; }

    /* Path */
    if (*p == '/') {
        int pi = 0;
        while (*p && pi < pmax - 1) path[pi++] = *p++;
        path[pi] = '\0';
    } else {
        path[0] = '/'; path[1] = '\0';
    }
}

/* Strip leading/trailing whitespace and newlines from version string */
static void trim_version(char *dst, const char *src, int max)
{
    /* Skip leading whitespace */
    while (*src == ' ' || *src == '\t' || *src == '\n' || *src == '\r')
        src++;

    int i = 0;
    while (i < max - 1 && src[i] && src[i] != '\n' && src[i] != '\r')
        i++;

    /* Copy trimmed */
    int j = 0;
    for (j = 0; j < i && j < max - 1; j++)
        dst[j] = src[j];

    /* Trim trailing whitespace */
    while (j > 0 && (dst[j - 1] == ' ' || dst[j - 1] == '\t'))
        j--;

    dst[j] = '\0';
}

/* ── GPT structures (matches installer.c) ─────────────────────── */

typedef struct __attribute__((packed)) {
    uint8_t  signature[8];    /* "EFI PART" */
    uint32_t revision;
    uint32_t header_size;     /* 92 */
    uint32_t header_crc32;
    uint32_t reserved;
    uint64_t my_lba;
    uint64_t alt_lba;
    uint64_t first_usable_lba;
    uint64_t last_usable_lba;
    uint8_t  disk_guid[16];
    uint64_t part_entry_lba;
    uint32_t num_parts;
    uint32_t part_entry_size; /* 128 */
    uint32_t part_crc32;
} gpt_header_t;

typedef struct __attribute__((packed)) {
    uint8_t  type_guid[16];
    uint8_t  unique_guid[16];
    uint64_t first_lba;
    uint64_t last_lba;
    uint64_t attributes;
    uint16_t name[36];        /* UTF-16LE */
} gpt_entry_t;

/* EFI System Partition type GUID: C12A7328-F81F-11D2-BA4B-00A0C93EC93B */
static const uint8_t ESP_TYPE_GUID[16] = {
    0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11,
    0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B
};

/* ── FAT32 helpers ─────────────────────────────────────────────── */

/*
 * Locate BOOTX64.EFI in a minimal FAT32 ESP.
 *
 * Alpha approach: matches the layout written by installer.c --
 *   - Cluster 2 = root dir
 *   - Cluster 3 = /EFI
 *   - Cluster 4 = /EFI/BOOT
 *   - Cluster 5+ = BOOTX64.EFI data
 *
 * Returns the starting LBA of BOOTX64.EFI data, or 0 on failure.
 * Also returns the directory entry LBA and offset for size update.
 */
typedef struct {
    uint64_t data_start_lba;    /* LBA where file data begins */
    uint64_t dir_entry_lba;     /* LBA of the directory block with the entry */
    int      dir_entry_offset;  /* Byte offset within that block */
    uint32_t start_cluster;     /* Starting FAT cluster */
    uint64_t fat1_lba;          /* FAT1 starting LBA */
    uint64_t fat2_lba;          /* FAT2 starting LBA */
    uint64_t data_region_lba;   /* Start of data region */
    uint32_t sectors_per_cluster;
    uint32_t blk_size;          /* NVMe block size */
} esp_file_info_t;

static int find_bootx64(uint64_t esp_start_lba, esp_file_info_t *info)
{
    const nvme_dev_t *dev = nvme_get_dev();
    if (!dev || !dev->ready) return -1;

    uint32_t blk_size = dev->block_size;
    if (blk_size == 0) blk_size = 512;
    info->blk_size = blk_size;

    /* Read BPB (first sector of ESP) */
    uint8_t *buf = (uint8_t *)kmalloc(blk_size);
    if (!buf) return -1;

    if (nvme_read(esp_start_lba, 1, buf) != 0) {
        kfree(buf);
        return -1;
    }

    /* Parse BPB fields */
    uint16_t bytes_per_sector = buf[11] | (buf[12] << 8);
    uint8_t  spc              = buf[13]; /* sectors per cluster */
    uint16_t reserved_sectors = buf[14] | (buf[15] << 8);
    uint8_t  num_fats         = buf[16];
    uint32_t fat_size_32      = buf[36] | (buf[37] << 8) |
                                (buf[38] << 16) | (buf[39] << 24);
    uint32_t root_cluster     = buf[44] | (buf[45] << 8) |
                                (buf[46] << 16) | (buf[47] << 24);

    if (bytes_per_sector == 0 || spc == 0 || num_fats == 0) {
        kfree(buf);
        return -1;
    }

    info->sectors_per_cluster = spc;

    /* Calculate FAT and data region positions in LBA */
    uint32_t sectors_per_block = blk_size / bytes_per_sector;
    if (sectors_per_block == 0) sectors_per_block = 1;

    uint64_t fat1_sector = reserved_sectors;
    uint64_t fat2_sector = fat1_sector + fat_size_32;
    uint64_t data_sector = fat2_sector + fat_size_32;

    info->fat1_lba = esp_start_lba + fat1_sector / sectors_per_block;
    info->fat2_lba = esp_start_lba + fat2_sector / sectors_per_block;
    info->data_region_lba = esp_start_lba + data_sector / sectors_per_block;

    /*
     * Navigate: root -> /EFI -> /BOOT -> BOOTX64.EFI
     *
     * cluster_to_lba(c) = data_region_lba + (c - 2) * spc / sectors_per_block
     */
    #define CLUSTER_TO_LBA(c) \
        (info->data_region_lba + \
         (uint64_t)((c) - 2) * spc / sectors_per_block)

    /* Read root directory (cluster 2 typically) */
    uint64_t dir_lba = CLUSTER_TO_LBA(root_cluster);
    if (nvme_read(dir_lba, 1, buf) != 0) {
        kfree(buf);
        return -1;
    }

    /* Find "EFI     " directory entry in root */
    uint32_t efi_cluster = 0;
    for (int i = 0; i < (int)blk_size; i += 32) {
        if (buf[i] == 0x00) break;    /* End of entries */
        if (buf[i] == 0xE5) continue; /* Deleted */
        if ((buf[i + 11] & 0x10) == 0) continue; /* Not a directory */
        /* Check 8.3 name: "EFI     " (8 chars, space-padded) */
        if (buf[i] == 'E' && buf[i+1] == 'F' && buf[i+2] == 'I' &&
            buf[i+3] == ' ' && buf[i+4] == ' ') {
            efi_cluster = (uint32_t)(buf[i+26] | (buf[i+27] << 8)) |
                          ((uint32_t)(buf[i+20] | (buf[i+21] << 8)) << 16);
            break;
        }
    }

    if (efi_cluster == 0) {
        kfree(buf);
        return -1;
    }

    /* Read /EFI directory */
    dir_lba = CLUSTER_TO_LBA(efi_cluster);
    if (nvme_read(dir_lba, 1, buf) != 0) {
        kfree(buf);
        return -1;
    }

    /* Find "BOOT    " directory entry */
    uint32_t boot_cluster = 0;
    for (int i = 0; i < (int)blk_size; i += 32) {
        if (buf[i] == 0x00) break;
        if (buf[i] == 0xE5) continue;
        if ((buf[i + 11] & 0x10) == 0) continue;
        if (buf[i] == 'B' && buf[i+1] == 'O' && buf[i+2] == 'O' &&
            buf[i+3] == 'T' && buf[i+4] == ' ') {
            boot_cluster = (uint32_t)(buf[i+26] | (buf[i+27] << 8)) |
                           ((uint32_t)(buf[i+20] | (buf[i+21] << 8)) << 16);
            break;
        }
    }

    if (boot_cluster == 0) {
        kfree(buf);
        return -1;
    }

    /* Read /EFI/BOOT directory */
    dir_lba = CLUSTER_TO_LBA(boot_cluster);
    if (nvme_read(dir_lba, 1, buf) != 0) {
        kfree(buf);
        return -1;
    }

    /* Find "BOOTX64 EFI" file entry */
    info->start_cluster = 0;
    for (int i = 0; i < (int)blk_size; i += 32) {
        if (buf[i] == 0x00) break;
        if (buf[i] == 0xE5) continue;
        if (buf[i + 11] & 0x10) continue; /* Skip directories */
        /* Check 8.3 name: "BOOTX64 EFI" */
        if (buf[i] == 'B' && buf[i+1] == 'O' && buf[i+2] == 'O' &&
            buf[i+3] == 'T' && buf[i+4] == 'X' && buf[i+5] == '6' &&
            buf[i+6] == '4' && buf[i+7] == ' ' &&
            buf[i+8] == 'E' && buf[i+9] == 'F' && buf[i+10] == 'I') {
            info->start_cluster =
                (uint32_t)(buf[i+26] | (buf[i+27] << 8)) |
                ((uint32_t)(buf[i+20] | (buf[i+21] << 8)) << 16);
            info->dir_entry_lba = dir_lba;
            info->dir_entry_offset = i;
            break;
        }
    }

    kfree(buf);

    if (info->start_cluster == 0) return -1;

    info->data_start_lba = CLUSTER_TO_LBA(info->start_cluster);

    #undef CLUSTER_TO_LBA
    return 0;
}

/* ── Public API ────────────────────────────────────────────────── */

void updater_init(const char *current_version)
{
    mem_set(&state, 0, sizeof(state));
    state.state = UPDATE_IDLE;
    str_copy(state.current_version, current_version,
             sizeof(state.current_version));
    dl_buf = 0;
    dl_len = 0;

    kputs("UPDATER: init, running version ");
    kputs(state.current_version);
    kputs("\n");
}

int updater_check(const char *update_server)
{
    state.state = UPDATE_CHECKING;
    state.progress_pct = 0;
    state.error_msg[0] = '\0';
    state.available_version[0] = '\0';
    state.update_url[0] = '\0';

    kputs("UPDATER: checking ");
    kputs(update_server);
    kputs("\n");

    /* Build version check URL: [server]/zeos/version.txt */
    char hostname[128];
    char path[256];
    int  use_tls = 0;

    /* Parse the server base URL */
    parse_url(update_server, hostname, sizeof(hostname),
              path, sizeof(path), &use_tls);

    /* Build version path: append /zeos/version.txt to base path */
    char version_path[256];
    str_copy(version_path, path, sizeof(version_path));
    /* Remove trailing slash if present */
    int plen = str_len(version_path);
    if (plen > 1 && version_path[plen - 1] == '/')
        version_path[plen - 1] = '\0';
    str_append(version_path, "/zeos/version.txt", sizeof(version_path));

    /* Fetch version.txt */
    char version_raw[128];
    mem_set(version_raw, 0, sizeof(version_raw));

    if (use_tls) {
        int body_len = 0;
        int status = https_get(hostname, version_path,
                               version_raw, sizeof(version_raw) - 1,
                               &body_len);
        if (status < 200 || status >= 300) {
            set_error("Version check failed (HTTPS)");
            return -1;
        }
        version_raw[body_len] = '\0';
    } else {
        struct http_response resp;
        if (http_get(hostname, version_path, &resp) != 0) {
            set_error("Version check failed (HTTP)");
            return -1;
        }
        if (resp.status_code < 200 || resp.status_code >= 300) {
            set_error("Version check: server returned error");
            return -1;
        }
        mem_copy(version_raw, resp.body, resp.body_len);
        version_raw[resp.body_len] = '\0';
    }

    /* Parse version (trim whitespace/newlines) */
    char remote_version[VERSION_MAX];
    trim_version(remote_version, version_raw, sizeof(remote_version));

    kputs("UPDATER: remote version = '");
    kputs(remote_version);
    kputs("', local = '");
    kputs(state.current_version);
    kputs("'\n");

    str_copy(state.available_version, remote_version,
             sizeof(state.available_version));

    /* Compare: if different, update is available */
    if (str_eq(remote_version, state.current_version)) {
        state.state = UPDATE_IDLE;
        kputs("UPDATER: up to date\n");
        return 0;
    }

    /* Build download URL: [server]/zeos/BOOTZ.EFI */
    str_copy(state.update_url, update_server, sizeof(state.update_url));
    /* Ensure no trailing slash before appending */
    int ulen = str_len(state.update_url);
    if (ulen > 0 && state.update_url[ulen - 1] == '/')
        state.update_url[ulen - 1] = '\0';
    str_append(state.update_url, "/zeos/BOOTZ.EFI", sizeof(state.update_url));

    state.state = UPDATE_AVAILABLE;

    kputs("UPDATER: update available: ");
    kputs(remote_version);
    kputs(" -> ");
    kputs(state.update_url);
    kputs("\n");

    return 0;
}

int updater_download(void)
{
    if (state.state != UPDATE_AVAILABLE) {
        set_error("No update available");
        return -1;
    }

    state.state = UPDATE_DOWNLOADING;
    state.progress_pct = 0;

    kputs("UPDATER: downloading ");
    kputs(state.update_url);
    kputs("\n");

    /* Free any previous download buffer */
    if (dl_buf) {
        kfree(dl_buf);
        dl_buf = 0;
        dl_len = 0;
    }

    /* Allocate download buffer */
    dl_buf = (uint8_t *)kmalloc(UPDATE_BUF_MAX);
    if (!dl_buf) {
        set_error("Out of memory for download");
        return -1;
    }

    /* Parse download URL */
    char hostname[128];
    char path[256];
    int  use_tls = 0;
    parse_url(state.update_url, hostname, sizeof(hostname),
              path, sizeof(path), &use_tls);

    /*
     * Download the binary.
     *
     * For HTTPS: use tls_connect + raw send/recv for large file support
     * (https_get is limited to its buffer size).
     * For HTTP: http_get is limited to HTTP_MAX_BODY (16KB), so we also
     * do a raw TCP download for the real binary.
     *
     * Alpha: use https_get/http_get for simplicity. The 4MB buffer and
     * 16KB HTTP body limit means real large downloads need the raw path.
     * For now, TLS raw download is the primary path.
     */

    if (use_tls) {
        /* TLS raw download for large binary */
        tls_conn_t *conn = tls_connect(hostname, 443);
        if (!conn) {
            set_error("TLS connect failed");
            kfree(dl_buf);
            dl_buf = 0;
            return -1;
        }

        /* Send HTTP GET request */
        char req[512];
        str_copy(req, "GET ", sizeof(req));
        str_append(req, path, sizeof(req));
        str_append(req, " HTTP/1.1\r\nHost: ", sizeof(req));
        str_append(req, hostname, sizeof(req));
        str_append(req, "\r\nConnection: close\r\n\r\n", sizeof(req));

        if (tls_send(conn, req, str_len(req)) < 0) {
            set_error("TLS send failed");
            tls_close(conn);
            kfree(dl_buf);
            dl_buf = 0;
            return -1;
        }

        /* Receive response (skip HTTP headers) */
        uint8_t tmp[4096];
        int total = 0;
        int headers_done = 0;
        int content_length = 0;

        while (total < UPDATE_BUF_MAX) {
            int n = tls_recv(conn, tmp, sizeof(tmp));
            if (n <= 0) break;

            int start = 0;
            if (!headers_done) {
                /* Scan for end of headers (\r\n\r\n) */
                for (int i = 0; i < n - 3; i++) {
                    if (tmp[i] == '\r' && tmp[i+1] == '\n' &&
                        tmp[i+2] == '\r' && tmp[i+3] == '\n') {
                        /* Parse Content-Length from headers for progress */
                        for (int h = 0; h < i; h++) {
                            if ((tmp[h] == 'C' || tmp[h] == 'c') &&
                                str_starts((const char *)tmp + h,
                                          "Content-Length: ")) {
                                int cl = 0;
                                int k = h + 16;
                                while (k < i && tmp[k] >= '0' && tmp[k] <= '9')
                                    cl = cl * 10 + (tmp[k++] - '0');
                                content_length = cl;
                                break;
                            }
                        }
                        headers_done = 1;
                        start = i + 4;
                        break;
                    }
                }
                if (!headers_done) continue;
            }

            /* Copy body data */
            int copy = n - start;
            if (total + copy > UPDATE_BUF_MAX)
                copy = UPDATE_BUF_MAX - total;
            if (copy > 0) {
                mem_copy(dl_buf + total, tmp + start, copy);
                total += copy;
            }

            /* Update progress */
            if (content_length > 0)
                state.progress_pct = (total * 100) / content_length;
            else if (total > 0)
                state.progress_pct = 50; /* Unknown size, show midpoint */
        }

        tls_close(conn);
        dl_len = total;

    } else {
        /* HTTP raw download via TCP (http_get is too small for binaries) */
        /* For Alpha: use http_get and accept the 16KB limit.
         * Real deployments use HTTPS anyway. */
        struct http_response resp;
        if (http_get(hostname, path, &resp) != 0) {
            set_error("HTTP download failed");
            kfree(dl_buf);
            dl_buf = 0;
            return -1;
        }
        if (resp.status_code < 200 || resp.status_code >= 300) {
            set_error("HTTP download: server error");
            kfree(dl_buf);
            dl_buf = 0;
            return -1;
        }
        mem_copy(dl_buf, resp.body, resp.body_len);
        dl_len = resp.body_len;
        state.progress_pct = 100;
    }

    kputs("UPDATER: downloaded ");
    kput_dec((uint64_t)dl_len);
    kputs(" bytes\n");

    if (dl_len < 2) {
        set_error("Download too small");
        kfree(dl_buf);
        dl_buf = 0;
        dl_len = 0;
        return -1;
    }

    /* Verify PE/COFF magic (MZ header) */
    if (dl_buf[0] != PE_MAGIC_0 || dl_buf[1] != PE_MAGIC_1) {
        set_error("Bad EFI binary (no MZ header)");
        kputs("UPDATER: first bytes: ");
        kput_hex(dl_buf[0]);
        kputs(" ");
        kput_hex(dl_buf[1]);
        kputs(" (expected 4D 5A)\n");
        kfree(dl_buf);
        dl_buf = 0;
        dl_len = 0;
        return -1;
    }

    state.progress_pct = 100;
    kputs("UPDATER: MZ header verified\n");
    return 0;
}

int updater_apply(void)
{
    if (dl_buf == 0 || dl_len == 0) {
        set_error("No downloaded binary to apply");
        return -1;
    }

    state.state = UPDATE_WRITING;
    state.progress_pct = 0;

    kputs("UPDATER: applying update (");
    kput_dec((uint64_t)dl_len);
    kputs(" bytes)\n");

    const nvme_dev_t *dev = nvme_get_dev();
    if (!dev || !dev->ready) {
        set_error("NVMe not available");
        return -1;
    }

    uint32_t blk_size = dev->block_size;
    if (blk_size == 0) blk_size = 512;

    /* ── Step 1: Read GPT header (LBA 1) ── */

    uint8_t *blk = (uint8_t *)kmalloc(blk_size);
    if (!blk) {
        set_error("Out of memory");
        return -1;
    }

    if (nvme_read(1, 1, blk) != 0) {
        set_error("Failed to read GPT header");
        kfree(blk);
        return -1;
    }

    gpt_header_t *gpt = (gpt_header_t *)blk;

    /* Verify GPT signature: "EFI PART" */
    const uint8_t gpt_sig[8] = {'E','F','I',' ','P','A','R','T'};
    if (!mem_eq(gpt->signature, gpt_sig, 8)) {
        set_error("Invalid GPT signature");
        kfree(blk);
        return -1;
    }

    uint64_t part_entry_lba = gpt->part_entry_lba;
    uint32_t num_parts      = gpt->num_parts;
    uint32_t entry_size     = gpt->part_entry_size;

    kputs("UPDATER: GPT found, ");
    kput_dec(num_parts);
    kputs(" partitions, entries at LBA ");
    kput_dec(part_entry_lba);
    kputs("\n");

    /* ── Step 2: Find EFI System Partition ── */

    uint64_t esp_start_lba = 0;
    uint64_t esp_end_lba   = 0;
    int entries_per_block = blk_size / entry_size;
    if (entries_per_block == 0) entries_per_block = 1;

    int found = 0;
    for (uint32_t i = 0; i < num_parts && !found; i++) {
        uint32_t block_idx = i / entries_per_block;
        uint32_t entry_idx = i % entries_per_block;

        if (nvme_read(part_entry_lba + block_idx, 1, blk) != 0) {
            set_error("Failed to read partition entries");
            kfree(blk);
            return -1;
        }

        gpt_entry_t *ent = (gpt_entry_t *)(blk + entry_idx * entry_size);

        if (mem_eq(ent->type_guid, ESP_TYPE_GUID, 16)) {
            esp_start_lba = ent->first_lba;
            esp_end_lba   = ent->last_lba;
            found = 1;
            kputs("UPDATER: ESP at LBA ");
            kput_dec(esp_start_lba);
            kputs(" - ");
            kput_dec(esp_end_lba);
            kputs("\n");
        }
    }

    if (!found) {
        set_error("EFI System Partition not found");
        kfree(blk);
        return -1;
    }

    /* ── Step 3: Find BOOTX64.EFI in FAT32 ── */

    esp_file_info_t finfo;
    mem_set(&finfo, 0, sizeof(finfo));

    if (find_bootx64(esp_start_lba, &finfo) != 0) {
        set_error("BOOTX64.EFI not found in ESP");
        kfree(blk);
        return -1;
    }

    kputs("UPDATER: BOOTX64.EFI at cluster ");
    kput_dec(finfo.start_cluster);
    kputs(", data LBA ");
    kput_dec(finfo.data_start_lba);
    kputs("\n");

    /* ── Step 4: Write new binary data ── */

    /*
     * Write the downloaded binary starting at the file's data cluster.
     * For Alpha: we write sequentially from the start cluster and
     * update the FAT chain to allocate consecutive clusters.
     */

    uint32_t bytes_per_cluster = finfo.sectors_per_cluster * BLOCK_SIZE;
    if (bytes_per_cluster == 0) bytes_per_cluster = 4096;

    uint32_t clusters_needed = ((uint32_t)dl_len + bytes_per_cluster - 1) /
                               bytes_per_cluster;

    uint32_t sectors_per_block = blk_size / BLOCK_SIZE;
    if (sectors_per_block == 0) sectors_per_block = 1;

    kputs("UPDATER: writing ");
    kput_dec(dl_len);
    kputs(" bytes (");
    kput_dec(clusters_needed);
    kputs(" clusters)\n");

    /* Write file data block by block */
    uint64_t write_lba = finfo.data_start_lba;
    int bytes_written = 0;

    while (bytes_written < dl_len) {
        int chunk = blk_size;
        if (bytes_written + chunk > dl_len)
            chunk = dl_len - bytes_written;

        /* Pad last block with zeros */
        if (chunk < (int)blk_size) {
            mem_set(blk, 0, blk_size);
            mem_copy(blk, dl_buf + bytes_written, chunk);
            if (nvme_write(write_lba, 1, blk) != 0) {
                set_error("Write failed");
                kfree(blk);
                return -1;
            }
        } else {
            if (nvme_write(write_lba, 1, dl_buf + bytes_written) != 0) {
                set_error("Write failed");
                kfree(blk);
                return -1;
            }
        }

        bytes_written += chunk;
        write_lba++;

        /* Update progress (0-80% for write phase) */
        state.progress_pct = (bytes_written * 80) / dl_len;
    }

    kputs("UPDATER: ");
    kput_dec(bytes_written);
    kputs(" bytes written\n");

    /* ── Step 5: Update FAT chain ── */

    /* Read FAT block containing our starting cluster */
    uint32_t fat_entries_per_block = blk_size / 4;
    uint32_t fat_block_idx = finfo.start_cluster / fat_entries_per_block;

    if (nvme_read(finfo.fat1_lba + fat_block_idx, 1, blk) != 0) {
        set_error("Failed to read FAT");
        kfree(blk);
        return -1;
    }

    /* Build chain: cluster N -> N+1 -> ... -> EOC */
    uint32_t *fat = (uint32_t *)blk;
    uint32_t first_entry = finfo.start_cluster % fat_entries_per_block;

    for (uint32_t c = 0; c < clusters_needed; c++) {
        uint32_t idx = first_entry + c;
        if (idx >= fat_entries_per_block) break; /* FAT spans multiple blocks */
        if (c == clusters_needed - 1)
            fat[idx] = 0x0FFFFFFF; /* EOC */
        else
            fat[idx] = finfo.start_cluster + c + 1;
    }

    /* Write updated FAT (both copies) */
    if (nvme_write(finfo.fat1_lba + fat_block_idx, 1, blk) != 0 ||
        nvme_write(finfo.fat2_lba + fat_block_idx, 1, blk) != 0) {
        set_error("FAT write failed");
        kfree(blk);
        return -1;
    }

    state.progress_pct = 85;

    /* ── Step 6: Update directory entry file size ── */

    if (nvme_read(finfo.dir_entry_lba, 1, blk) != 0) {
        set_error("Failed to read directory");
        kfree(blk);
        return -1;
    }

    uint8_t *entry = blk + finfo.dir_entry_offset;
    uint32_t fsize = (uint32_t)dl_len;
    entry[28] = fsize & 0xFF;
    entry[29] = (fsize >> 8) & 0xFF;
    entry[30] = (fsize >> 16) & 0xFF;
    entry[31] = (fsize >> 24) & 0xFF;

    if (nvme_write(finfo.dir_entry_lba, 1, blk) != 0) {
        set_error("Directory update failed");
        kfree(blk);
        return -1;
    }

    state.progress_pct = 90;

    /* ── Step 7: Flush and verify ── */

    nvme_flush();

    /* Read back first block and verify MZ header + data match */
    if (nvme_read(finfo.data_start_lba, 1, blk) != 0) {
        set_error("Verify read failed");
        kfree(blk);
        return -1;
    }

    if (blk[0] != PE_MAGIC_0 || blk[1] != PE_MAGIC_1) {
        set_error("Verify failed: MZ header mismatch");
        kfree(blk);
        return -1;
    }

    /* Verify sample of bytes against download buffer */
    int verify_len = (int)blk_size;
    if (verify_len > dl_len) verify_len = dl_len;
    if (!mem_eq(blk, dl_buf, verify_len)) {
        set_error("Verify failed: data mismatch");
        kfree(blk);
        return -1;
    }

    state.progress_pct = 100;
    state.state = UPDATE_DONE;

    kputs("UPDATER: update applied and verified\n");
    kputs("UPDATER: reboot to run version ");
    kputs(state.available_version);
    kputs("\n");

    /* Free download buffer -- no longer needed */
    kfree(dl_buf);
    dl_buf = 0;
    dl_len = 0;
    kfree(blk);

    return 0;
}

/* ── UI Drawing ────────────────────────────────────────────────── */

void updater_draw(int x, int y, int w, int h)
{
    /* Background panel */
    fb_rect(x, y, w, h, COLOR_SURFACE_HIGH);
    fb_rect_outline(x, y, w, h, COLOR_SEPARATOR, 1);

    int pad = Z4;
    int cx = x + pad;
    int cy = y + pad;
    int cw = w - pad * 2;

    /* Title */
    font_draw(cx, cy, "System Update", FONT_UI_BOLD, TYPE_HEADING,
              COLOR_ON_SURFACE);
    cy += font_line_height(FONT_UI_BOLD, TYPE_HEADING) + Z2;

    /* Current version */
    char ver_line[96];
    str_copy(ver_line, "Running: v", sizeof(ver_line));
    str_append(ver_line, state.current_version, sizeof(ver_line));
    font_draw(cx, cy, ver_line, FONT_UI, TYPE_BODY, COLOR_ON_SURFACE_2);
    cy += font_line_height(FONT_UI, TYPE_BODY) + Z1;

    /* State-specific content */
    switch (state.state) {

    case UPDATE_IDLE:
        font_draw(cx, cy, "Up to date.", FONT_UI, TYPE_BODY,
                  COLOR_SUCCESS);
        break;

    case UPDATE_CHECKING:
        font_draw(cx, cy, "Checking for updates...", FONT_UI, TYPE_BODY,
                  COLOR_ON_SURFACE_2);
        break;

    case UPDATE_AVAILABLE: {
        char avail[96];
        str_copy(avail, "Available: v", sizeof(avail));
        str_append(avail, state.available_version, sizeof(avail));
        font_draw(cx, cy, avail, FONT_UI, TYPE_BODY, COLOR_PRIMARY);
        cy += font_line_height(FONT_UI, TYPE_BODY) + Z2;
        font_draw(cx, cy, "Ready to download.", FONT_UI, TYPE_LABEL,
                  COLOR_ON_SURFACE_2);
        break;
    }

    case UPDATE_DOWNLOADING: {
        char dl_msg[96];
        str_copy(dl_msg, "Downloading v", sizeof(dl_msg));
        str_append(dl_msg, state.available_version, sizeof(dl_msg));
        str_append(dl_msg, "...", sizeof(dl_msg));
        font_draw(cx, cy, dl_msg, FONT_UI, TYPE_BODY, COLOR_ON_SURFACE);
        cy += font_line_height(FONT_UI, TYPE_BODY) + Z2;

        /* Progress bar */
        int bar_h = Z3;
        int bar_y = cy;
        fb_rect(cx, bar_y, cw, bar_h, COLOR_SURFACE);
        int fill_w = (cw * state.progress_pct) / 100;
        if (fill_w > 0)
            fb_rect(cx, bar_y, fill_w, bar_h, COLOR_PRIMARY);
        cy += bar_h + Z1;

        /* Percentage text */
        char pct_str[16];
        int_to_str(state.progress_pct, pct_str, sizeof(pct_str));
        str_append(pct_str, "%", sizeof(pct_str));
        font_draw(cx, cy, pct_str, FONT_CODE, TYPE_LABEL,
                  COLOR_ON_SURFACE_2);
        break;
    }

    case UPDATE_WRITING: {
        font_draw(cx, cy, "Writing to disk...", FONT_UI, TYPE_BODY,
                  COLOR_WARNING);
        cy += font_line_height(FONT_UI, TYPE_BODY) + Z2;

        /* Progress bar */
        int bar_h = Z3;
        int bar_y = cy;
        fb_rect(cx, bar_y, cw, bar_h, COLOR_SURFACE);
        int fill_w = (cw * state.progress_pct) / 100;
        if (fill_w > 0)
            fb_rect(cx, bar_y, fill_w, bar_h, COLOR_WARNING);
        cy += bar_h + Z1;

        char pct_str[16];
        int_to_str(state.progress_pct, pct_str, sizeof(pct_str));
        str_append(pct_str, "%", sizeof(pct_str));
        font_draw(cx, cy, pct_str, FONT_CODE, TYPE_LABEL,
                  COLOR_ON_SURFACE_2);

        cy += font_line_height(FONT_CODE, TYPE_LABEL) + Z1;
        font_draw(cx, cy, "Do not power off.", FONT_UI, TYPE_CAPTION,
                  COLOR_DANGER);
        break;
    }

    case UPDATE_DONE:
        font_draw(cx, cy, "Update complete.", FONT_UI, TYPE_BODY,
                  COLOR_SUCCESS);
        cy += font_line_height(FONT_UI, TYPE_BODY) + Z2;

        font_draw(cx, cy, "Reboot to complete.", FONT_UI_BOLD, TYPE_BODY,
                  COLOR_PRIMARY);
        cy += font_line_height(FONT_UI_BOLD, TYPE_BODY) + Z1;

        if (state.available_version[0]) {
            char done_msg[96];
            str_copy(done_msg, "v", sizeof(done_msg));
            str_append(done_msg, state.current_version, sizeof(done_msg));
            str_append(done_msg, " -> v", sizeof(done_msg));
            str_append(done_msg, state.available_version, sizeof(done_msg));
            font_draw(cx, cy, done_msg, FONT_CODE, TYPE_LABEL,
                      COLOR_ON_SURFACE_3);
        }
        break;

    case UPDATE_ERROR:
        font_draw(cx, cy, "Update failed.", FONT_UI_BOLD, TYPE_BODY,
                  COLOR_DANGER);
        cy += font_line_height(FONT_UI_BOLD, TYPE_BODY) + Z2;

        if (state.error_msg[0]) {
            font_draw(cx, cy, state.error_msg, FONT_CODE, TYPE_LABEL,
                      COLOR_DANGER);
        }
        break;
    }

    (void)h; /* Height available for future scroll/layout */
}

updater_state_t *updater_get_state(void)
{
    return &state;
}
