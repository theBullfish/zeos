/*
 * Zeos — virtio-net PCI driver (legacy interface)
 *
 * virtio legacy uses I/O port-based PCI BAR0 for device registers
 * and shared memory (virtqueues) for data transfer.
 *
 * Virtqueue layout (per virtio 0.9 spec):
 *   Descriptor table: array of {addr, len, flags, next}
 *   Available ring: guest writes here to offer buffers
 *   Used ring: device writes here when done with a buffer
 *
 * We use two queues: RX (queue 0) and TX (queue 1).
 */

#include "net_virtio.h"
#include "pci.h"
#include "hal.h"
#include "pmm.h"
#include "kprint.h"
#include "spinlock.h"

/* ── virtio legacy register offsets (from BAR0) ── */
#define VIRTIO_DEV_FEATURES     0x00    /* 32-bit, device features */
#define VIRTIO_DRV_FEATURES     0x04    /* 32-bit, driver features */
#define VIRTIO_QUEUE_ADDR       0x08    /* 32-bit, queue PFN */
#define VIRTIO_QUEUE_SIZE       0x0C    /* 16-bit, queue size */
#define VIRTIO_QUEUE_SELECT     0x0E    /* 16-bit, queue index */
#define VIRTIO_QUEUE_NOTIFY     0x10    /* 16-bit, queue notify */
#define VIRTIO_DEV_STATUS       0x12    /* 8-bit, device status */
#define VIRTIO_ISR_STATUS       0x13    /* 8-bit, ISR status */
#define VIRTIO_NET_MAC          0x14    /* 6 bytes, MAC address */

/* virtio status bits */
#define VIRTIO_STATUS_ACK       1
#define VIRTIO_STATUS_DRIVER    2
#define VIRTIO_STATUS_FEATURES  8
#define VIRTIO_STATUS_DRIVER_OK 4
#define VIRTIO_STATUS_FAILED    128

/* virtio-net feature bits */
#define VIRTIO_NET_F_MAC        (1 << 5)

/* virtio descriptor flags */
#define VRING_DESC_F_NEXT       1
#define VRING_DESC_F_WRITE      2   /* Buffer is device-writable */

/* ── Virtqueue structures ─────────────────────── */

struct vring_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed));

struct vring_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[];
} __attribute__((packed));

struct vring_used_elem {
    uint32_t id;
    uint32_t len;
} __attribute__((packed));

struct vring_used {
    uint16_t flags;
    uint16_t idx;
    struct vring_used_elem ring[];
} __attribute__((packed));

struct virtqueue {
    uint16_t             size;       /* Number of descriptors */
    uint16_t             free_head;  /* Next free descriptor */
    uint16_t             last_used;  /* Last processed used index */
    struct vring_desc   *desc;
    struct vring_avail  *avail;
    struct vring_used   *used;
    uint8_t            **buffers;    /* Buffer addresses per descriptor */
};

/* ── Device state ─────────────────────────────── */

static uint16_t io_base;            /* BAR0 I/O port base */
static struct mac_addr dev_mac;
static struct virtqueue rxq;        /* Queue 0: receive */
static struct virtqueue txq;        /* Queue 1: transmit */
static int virtio_ready = 0;

/* SMP — virtqueue ring writes (avail->idx, ring slots, descriptor
 * table) must be serialized. Concurrent virtio_net_send from two cores
 * would corrupt avail->idx. Same for rx requeue path. */
static zeos_spinlock_t s_rxq_lock = ZEOS_SPINLOCK_INIT;
static zeos_spinlock_t s_txq_lock = ZEOS_SPINLOCK_INIT;

/* RX/TX buffer pool */
#define VQ_SIZE     256             /* Max queue size */
#define RX_BUFS     16
#define TX_BUFS     16
static uint8_t rx_buffers[RX_BUFS][NET_BUF_SIZE] __attribute__((aligned(16)));
static uint8_t tx_buffers[TX_BUFS][NET_BUF_SIZE] __attribute__((aligned(16)));

/* ── I/O helpers ──────────────────────────────── */

static inline void vio_write8(uint16_t offset, uint8_t val)  { hal_out8(io_base + offset, val); }
static inline void vio_write16(uint16_t offset, uint16_t val) { hal_out16(io_base + offset, val); }
static inline void vio_write32(uint16_t offset, uint32_t val) { hal_out32(io_base + offset, val); }
static inline uint8_t vio_read8(uint16_t offset) { return hal_in8(io_base + offset); }
static inline uint16_t vio_read16(uint16_t offset) {
    uint16_t val;
    val = hal_in16(io_base + offset);
    return val;
}
static inline uint32_t vio_read32(uint16_t offset) {
    uint32_t val;
    val = hal_in32(io_base + offset);
    return val;
}

/* ── Virtqueue setup ──────────────────────────── */

/*
 * Calculate virtqueue memory layout size.
 * Descriptor table: 16 bytes * size
 * Available ring: 6 + 2 * size bytes (aligned to 2)
 * Used ring: 6 + 8 * size bytes (aligned to 4096)
 */
static uint64_t vq_mem_size(uint16_t qsz)
{
    uint64_t desc_size = (uint64_t)qsz * 16;
    uint64_t avail_size = 6 + (uint64_t)qsz * 2;
    uint64_t used_offset = (desc_size + avail_size + 4095) & ~4095ULL;
    uint64_t used_size = 6 + (uint64_t)qsz * 8;
    return used_offset + used_size;
}

static int setup_virtqueue(struct virtqueue *vq, uint16_t queue_idx)
{
    /* Select queue */
    vio_write16(VIRTIO_QUEUE_SELECT, queue_idx);

    /* Read queue size */
    uint16_t qsz = vio_read16(VIRTIO_QUEUE_SIZE);
    if (qsz == 0) return -1;
    if (qsz > VQ_SIZE) qsz = VQ_SIZE;

    vq->size = qsz;
    vq->free_head = 0;
    vq->last_used = 0;

    /* Allocate contiguous memory (page-aligned) */
    uint64_t total = vq_mem_size(qsz);
    uint64_t pages = (total + 4095) / 4096;
    uint64_t phys = pmm_alloc_contiguous(pages);
    if (!phys) return -1;

    uint8_t *mem = (uint8_t *)phys;

    /* Zero it */
    for (uint64_t i = 0; i < pages * 4096; i++)
        mem[i] = 0;

    /* Set up pointers */
    vq->desc = (struct vring_desc *)mem;
    uint64_t avail_off = (uint64_t)qsz * 16;
    vq->avail = (struct vring_avail *)(mem + avail_off);
    uint64_t used_off = (avail_off + 6 + (uint64_t)qsz * 2 + 4095) & ~4095ULL;
    vq->used = (struct vring_used *)(mem + used_off);

    /* Chain free descriptors */
    for (uint16_t i = 0; i < qsz - 1; i++)
        vq->desc[i].next = i + 1;
    vq->desc[qsz - 1].next = 0xFFFF;

    /* Tell device where the queue is (page frame number) */
    vio_write32(VIRTIO_QUEUE_ADDR, (uint32_t)(phys / 4096));

    return 0;
}

/* ── RX buffer management ─────────────────────── */

/*
 * virtio-net RX requires a 10-byte header before the packet data.
 * We include the header in the buffer.
 */
#define VIRTIO_NET_HDR_SIZE 10

static void rx_populate(void)
{
    for (int i = 0; i < RX_BUFS && i < rxq.size; i++) {
        uint16_t idx = rxq.free_head;
        if (idx >= rxq.size) break;

        rxq.free_head = rxq.desc[idx].next;

        rxq.desc[idx].addr = (uint64_t)(unsigned long)rx_buffers[i];
        rxq.desc[idx].len = NET_BUF_SIZE;
        rxq.desc[idx].flags = VRING_DESC_F_WRITE;  /* Device writes into this */
        rxq.desc[idx].next = 0;

        /* Add to available ring */
        uint16_t avail_idx = rxq.avail->idx % rxq.size;
        rxq.avail->ring[avail_idx] = idx;
        __asm__ volatile("" ::: "memory");  /* Memory barrier */
        rxq.avail->idx++;
    }

    /* Notify device */
    vio_write16(VIRTIO_QUEUE_NOTIFY, 0);
}

/* ── Public API ───────────────────────────────── */

int virtio_net_init(void)
{
    /* Find virtio-net PCI device: vendor 0x1AF4, device 0x1000 (legacy net) */
    int count = pci_device_count();
    struct pci_device *dev = 0;

    for (int i = 0; i < count; i++) {
        struct pci_device *d = pci_get_device(i);
        if (d && d->vendor_id == 0x1AF4 &&
            (d->device_id == 0x1000 || d->device_id == 0x1041)) {
            dev = d;
            break;
        }
    }

    if (!dev) {
        kputs("virtio-net: device not found\n");
        return -1;
    }

    /* Get BAR0 (I/O port base) */
    io_base = (uint16_t)(dev->bar[0] & ~3);

    /* Enable PCI bus mastering */
    uint32_t cmd = pci_config_read32(dev->bus, dev->dev, dev->func, 0x04);
    cmd |= (1 << 2);  /* Bus master enable */
    cmd |= (1 << 0);  /* I/O space enable */
    pci_config_write32(dev->bus, dev->dev, dev->func, 0x04, cmd);

    /* Reset device */
    vio_write8(VIRTIO_DEV_STATUS, 0);

    /* Acknowledge + driver */
    vio_write8(VIRTIO_DEV_STATUS, VIRTIO_STATUS_ACK);
    vio_write8(VIRTIO_DEV_STATUS, VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER);

    /* Read features, negotiate */
    uint32_t features = vio_read32(VIRTIO_DEV_FEATURES);
    uint32_t accept = 0;
    if (features & VIRTIO_NET_F_MAC)
        accept |= VIRTIO_NET_F_MAC;
    vio_write32(VIRTIO_DRV_FEATURES, accept);

    /* Read MAC address */
    for (int i = 0; i < 6; i++)
        dev_mac.b[i] = vio_read8(VIRTIO_NET_MAC + i);

    /* Set up queues */
    if (setup_virtqueue(&rxq, 0) < 0) {
        kputs("virtio-net: failed to setup RX queue\n");
        vio_write8(VIRTIO_DEV_STATUS, VIRTIO_STATUS_FAILED);
        return -1;
    }
    if (setup_virtqueue(&txq, 1) < 0) {
        kputs("virtio-net: failed to setup TX queue\n");
        vio_write8(VIRTIO_DEV_STATUS, VIRTIO_STATUS_FAILED);
        return -1;
    }

    /* Driver OK */
    vio_write8(VIRTIO_DEV_STATUS, VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER |
                                   VIRTIO_STATUS_DRIVER_OK);

    /* Pre-populate RX queue with buffers */
    rx_populate();

    virtio_ready = 1;
    return 0;
}

int virtio_net_send(const void *data, uint16_t len)
{
    if (!virtio_ready || len > NET_FRAME_MAX)
        return -1;

    uint64_t f = spin_lock_irqsave(&s_txq_lock);

    /* Get a free TX descriptor */
    static int tx_idx = 0;
    int buf_idx = tx_idx % TX_BUFS;
    tx_idx++;

    /* Prepend virtio-net header (10 bytes of zeros) */
    uint8_t *buf = tx_buffers[buf_idx];
    for (int i = 0; i < VIRTIO_NET_HDR_SIZE; i++)
        buf[i] = 0;
    for (int i = 0; i < len; i++)
        buf[VIRTIO_NET_HDR_SIZE + i] = ((const uint8_t *)data)[i];

    /* Set up descriptor */
    uint16_t desc_idx = buf_idx % txq.size;
    txq.desc[desc_idx].addr = (uint64_t)(unsigned long)buf;
    txq.desc[desc_idx].len = VIRTIO_NET_HDR_SIZE + len;
    txq.desc[desc_idx].flags = 0;  /* Device reads from this */
    txq.desc[desc_idx].next = 0;

    /* Add to available ring */
    uint16_t avail_idx = txq.avail->idx % txq.size;
    txq.avail->ring[avail_idx] = desc_idx;
    __asm__ volatile("" ::: "memory");
    txq.avail->idx++;

    /* Notify device */
    vio_write16(VIRTIO_QUEUE_NOTIFY, 1);

    spin_unlock_irqrestore(&s_txq_lock, f);
    return 0;
}

int virtio_net_recv(void *buf, uint16_t max_len)
{
    if (!virtio_ready)
        return 0;

    uint64_t f = spin_lock_irqsave(&s_rxq_lock);

    /* Check if device has returned any buffers */
    if (rxq.last_used == rxq.used->idx) {
        spin_unlock_irqrestore(&s_rxq_lock, f);
        return 0;  /* No new packets */
    }

    /* Get the used element */
    uint16_t used_idx = rxq.last_used % rxq.size;
    uint32_t desc_idx = rxq.used->ring[used_idx].id;
    uint32_t total_len = rxq.used->ring[used_idx].len;
    rxq.last_used++;

    /* Skip virtio-net header */
    if (total_len <= VIRTIO_NET_HDR_SIZE)
        goto requeue;

    uint16_t frame_len = (uint16_t)(total_len - VIRTIO_NET_HDR_SIZE);
    if (frame_len > max_len)
        frame_len = max_len;

    uint8_t *src = (uint8_t *)(unsigned long)rxq.desc[desc_idx].addr;
    uint8_t *dst = (uint8_t *)buf;
    for (uint16_t i = 0; i < frame_len; i++)
        dst[i] = src[VIRTIO_NET_HDR_SIZE + i];

requeue:
    /* Re-add buffer to available ring */
    {
        uint16_t avail_idx = rxq.avail->idx % rxq.size;
        rxq.avail->ring[avail_idx] = (uint16_t)desc_idx;
        __asm__ volatile("" ::: "memory");
        rxq.avail->idx++;
        vio_write16(VIRTIO_QUEUE_NOTIFY, 0);
    }

    spin_unlock_irqrestore(&s_rxq_lock, f);

    if (total_len <= VIRTIO_NET_HDR_SIZE)
        return 0;

    return (int)((total_len - VIRTIO_NET_HDR_SIZE > max_len) ? max_len : total_len - VIRTIO_NET_HDR_SIZE);
}

void virtio_net_get_mac(struct mac_addr *mac)
{
    *mac = dev_mac;
}
