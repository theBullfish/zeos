/*
 * habanalabs_uapi.h — minimal vendored subset of the in-tree Linux UAPI
 * for the `habanalabs` accelerator driver.
 *
 * Source: linux/include/uapi/misc/habanalabs.h (stable kernel ABI;
 * kernel rule is "do not break userspace" — these structs do not
 * change in incompatible ways). We vendor a subset so this proof
 * binary builds on any Linux box without requiring the
 * linux-libc-dev or kernel-headers package versions to match.
 *
 * We define ONLY what habana_proof.c uses: HL_INFO subcommands,
 * HW_IP_INFO struct, command-buffer / command-submit / wait
 * structures, memory ioctl arguments. Everything else in the upstream
 * header (counter ioctls, debug, profiler) is omitted on purpose.
 *
 * License note: kernel UAPI headers are dual-licensed (GPL + BSD) and
 * the structure layout is hardware-defined, same precedent as the
 * BAR/register offsets used elsewhere in this project.
 */

#ifndef HABANA_PROOF_HABANALABS_UAPI_H
#define HABANA_PROOF_HABANALABS_UAPI_H

#include <stdint.h>
#include <linux/ioctl.h>

/* ── device family enum, returned in hl_info_hw_ip_info.device_id ── */
#define HL_DEVICE_GOYA            1
#define HL_DEVICE_PLACEHOLDER     2
#define HL_DEVICE_GAUDI           3
#define HL_DEVICE_GAUDI_SEC       4
#define HL_DEVICE_GAUDI2          5
#define HL_DEVICE_GAUDI2B         6
#define HL_DEVICE_GAUDI2C         7
#define HL_DEVICE_GAUDI3          8
/* Numbers above 8 are reserved for future generations; the discovery
 * code in habana_proof.c falls through to "Habana-unknown" rather than
 * misidentify silicon it has never seen. */

/* ── HL_INFO subcommands ──────────────────────────────────────── */
#define HL_INFO_HW_IP_INFO        0
#define HL_INFO_HW_EVENTS         1
#define HL_INFO_DRAM_USAGE        2
#define HL_INFO_HW_IDLE           3
#define HL_INFO_DEVICE_STATUS     4
#define HL_INFO_DEVICE_UTILIZATION 6

struct hl_info_args {
    uint64_t return_pointer;
    uint32_t return_size;
    uint32_t op;
    union {
        uint32_t dcore_id;
        uint32_t ctx_id;
        uint32_t period_ms;
        uint32_t pll_index;
        uint32_t eventQ_index;
        uint32_t user_buffer_actual_size;
        uint32_t sec_attest_nonce;
    };
    uint32_t pad[2];
};

struct hl_info_hw_ip_info {
    uint64_t sram_base_address;
    uint64_t dram_base_address;
    uint64_t dram_size;
    uint32_t sram_size;
    uint32_t num_of_events;
    uint32_t device_id;            /* HL_DEVICE_* enum above. */
    uint32_t module_id;
    uint32_t decoder_enabled_mask;
    uint16_t first_available_interrupt_id;
    uint16_t server_type;
    uint32_t cpld_version;
    uint32_t psoc_pci_pll_nr;
    uint32_t psoc_pci_pll_nf;
    uint32_t psoc_pci_pll_od;
    uint32_t psoc_pci_pll_div_factor;
    uint8_t  tpc_enabled_mask;
    uint8_t  dram_enabled;
    uint8_t  security_enabled;
    uint8_t  mme_master_slave_mode;
    uint8_t  cpucp_version[64];
    uint8_t  card_name[16];
    uint64_t tpc_enabled_mask_ext;
    uint64_t dram_page_size;
    uint32_t edma_enabled_mask;
    uint16_t number_of_user_interrupts;
    uint8_t  reserved_topology[2];
    uint64_t reserved1;
    uint64_t reserved2;
    uint8_t  reserved3;
    uint8_t  revision_id;
    uint16_t tpc_interrupt_id;
    uint32_t rotator_enabled_mask;
    uint32_t reserved_engines[8];
};

/* ── HL_CB / command buffer ──────────────────────────────────── */
#define HL_CB_OP_CREATE           0
#define HL_CB_OP_DESTROY          1
#define HL_CB_OP_INFO             2

struct hl_cb_in {
    uint64_t cb_handle;
    uint32_t op;
    uint32_t cb_size;
    uint32_t ctx_id;
    uint32_t flags;
};
struct hl_cb_out {
    union {
        uint64_t cb_handle;
        struct {
            uint32_t usage_cnt;
            uint32_t pad;
        };
    };
};
union hl_cb_args {
    struct hl_cb_in  in;
    struct hl_cb_out out;
};

/* ── HL_CS / command submission ──────────────────────────────── */
#define HL_CS_FLAGS_FORCE_RESTORE       0x1
#define HL_CS_FLAGS_SIGNAL              0x2
#define HL_CS_FLAGS_WAIT                0x4
#define HL_CS_FLAGS_COLLECTIVE_WAIT     0x8
#define HL_CS_FLAGS_TIMESTAMP           0x20

struct hl_cs_chunk {
    union {
        uint64_t cb_handle;
        uint64_t signal_seq_arr;
    };
    uint32_t queue_index;
    uint32_t cb_size;
    uint32_t cs_chunk_flags;
    uint32_t num_signal_seq_arr;
    uint32_t signal_seq_nr;
    uint32_t encaps_signal_offset;
};

struct hl_cs_in {
    uint64_t chunks_restore;
    uint64_t chunks_execute;
    union {
        uint64_t chunks_store;
        uint64_t signal_seq_arr;
    };
    uint32_t num_chunks_restore;
    uint32_t num_chunks_execute;
    union {
        uint32_t num_chunks_store;
        uint32_t num_signal_seq_arr;
    };
    uint32_t cs_flags;
    uint32_t ctx_id;
    uint32_t encaps_sig_handle_id;
    uint32_t pad[2];
};

struct hl_cs_out {
    union {
        uint64_t seq;
        struct {
            uint32_t handle_id;
            uint32_t count;
        };
    };
    uint32_t status;
    uint32_t sob_base_addr_offset;
    uint16_t sob_count_before_submission;
    uint16_t pad[3];
};
union hl_cs_args {
    struct hl_cs_in  in;
    struct hl_cs_out out;
};

/* ── HL_WAIT_CS ───────────────────────────────────────────────── */
#define HL_WAIT_CS_STATUS_COMPLETED       0
#define HL_WAIT_CS_STATUS_BUSY            1
#define HL_WAIT_CS_STATUS_TIMEDOUT        2
#define HL_WAIT_CS_STATUS_ABORTED         3

struct hl_wait_cs_in {
    union {
        struct {
            uint64_t seq;
            uint64_t timeout_us;
        };
        struct {
            union {
                uint64_t addr;
                uint64_t cq_counters_handle;
            };
            uint64_t target;
        };
    };
    uint32_t ctx_id;
    uint32_t flags;
    union {
        struct {
            uint8_t  reset_on_timeout;
            uint8_t  pad[7];
        };
        struct {
            uint64_t interrupt_timeout_us;
        };
    };
    uint32_t cq_counters_offset;
    uint32_t timestamp_handle;
    uint32_t timestamp_offset;
};

struct hl_wait_cs_out {
    uint32_t status;
    uint32_t flags;
    int64_t  timestamp_nsec;
};
union hl_wait_cs_args {
    struct hl_wait_cs_in  in;
    struct hl_wait_cs_out out;
};

/* ── HL_MEM ──────────────────────────────────────────────────── */
#define HL_MEM_OP_ALLOC               0
#define HL_MEM_OP_FREE                1
#define HL_MEM_OP_MAP                 2
#define HL_MEM_OP_UNMAP               3
#define HL_MEM_OP_MAP_BLOCK           4
#define HL_MEM_OP_EXPORT_DMABUF_FD    5
#define HL_MEM_OP_TS_ALLOC            6

#define HL_MEM_CONTIGUOUS             0x1
#define HL_MEM_SHARED                 0x2
#define HL_MEM_USERPTR                0x4

struct hl_mem_in {
    union {
        struct {
            uint64_t mem_size;
            uint64_t page_size;
        } alloc;
        struct {
            uint64_t handle;
        } free;
        struct {
            uint64_t hint_addr;
            uint64_t handle;
        } map_device;
        struct {
            uint64_t hint_addr;
            uint64_t host_virt_addr;
            uint64_t mem_size;
        } map_host;
        struct {
            uint64_t device_virt_addr;
        } unmap;
        struct {
            uint64_t block_addr;
        } map_block;
        struct {
            uint64_t handle;
            uint64_t mem_size;
            uint64_t offset;
            uint64_t flags;
        } export_dmabuf_fd;
    };
    uint32_t op;
    uint32_t flags;
    uint32_t ctx_id;
    uint32_t num_of_elements;
};

struct hl_mem_out {
    union {
        uint64_t device_virt_addr;
        uint64_t handle;
        struct {
            uint64_t block_handle;
            uint32_t block_size;
            uint32_t pad;
        };
        int32_t fd;
    };
};
union hl_mem_args {
    struct hl_mem_in  in;
    struct hl_mem_out out;
};

/* ── ioctl numbers ───────────────────────────────────────────── */
#define HL_IOCTL_BASE        'H'
#define HL_IOCTL_INFO        _IOWR(HL_IOCTL_BASE, 0x01, struct hl_info_args)
#define HL_IOCTL_CB          _IOWR(HL_IOCTL_BASE, 0x02, union hl_cb_args)
#define HL_IOCTL_CS          _IOWR(HL_IOCTL_BASE, 0x03, union hl_cs_args)
#define HL_IOCTL_WAIT_CS     _IOWR(HL_IOCTL_BASE, 0x04, union hl_wait_cs_args)
#define HL_IOCTL_MEMORY      _IOWR(HL_IOCTL_BASE, 0x05, union hl_mem_args)

#endif /* HABANA_PROOF_HABANALABS_UAPI_H */
