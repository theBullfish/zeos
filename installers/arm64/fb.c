/* Zeos aarch64 — framebuffer via QEMU ramfb (fw_cfg configured).
 * First pixels on the ARM port. This is the foundation the compositor/WM
 * will draw on; brought up as part of the visual bring-up, not after it. */
#include <stdint.h>

extern int strcmp(const char *, const char *);

#define FWCFG_BASE   0x09020000UL
#define FWCFG_DMA    (FWCFG_BASE + 16)
#define CTL_ERROR    0x01
#define CTL_READ     0x02
#define CTL_SELECT   0x08
#define CTL_WRITE    0x10
#define FW_CFG_FILE_DIR 0x0019
#define DRM_XRGB8888 0x34325258u

#define FB_W 800
#define FB_H 600
uint32_t fb_backing[FB_W * FB_H] __attribute__((aligned(4096)));
static int fb_ready = 0;

static inline uint32_t be32(uint32_t v) { return __builtin_bswap32(v); }
static inline uint64_t be64(uint64_t v) { return __builtin_bswap64(v); }
static inline uint16_t be16(uint16_t v) { return __builtin_bswap16(v); }

struct DmaAccess { uint32_t control; uint32_t length; uint64_t address; } __attribute__((packed));
struct FwCfgFile { uint32_t size; uint16_t select; uint16_t reserved; char name[56]; } __attribute__((packed));
struct RamfbCfg  { uint64_t addr; uint32_t fourcc; uint32_t flags; uint32_t width; uint32_t height; uint32_t stride; } __attribute__((packed));

static void fwcfg_dma(uint32_t control, uint32_t length, void *buf)
{
    static volatile struct DmaAccess acc __attribute__((aligned(64)));
    acc.control = be32(control);
    acc.length  = be32(length);
    acc.address = be64((uint64_t)buf);
    __asm__ volatile("dsb sy" ::: "memory");
    *(volatile uint64_t *)FWCFG_DMA = be64((uint64_t)&acc);
    __asm__ volatile("dsb sy" ::: "memory");
    while (be32(acc.control) & ~CTL_ERROR) { }
}

static int fwcfg_find(const char *name)
{
    uint32_t cnt_be;
    fwcfg_dma((FW_CFG_FILE_DIR << 16) | CTL_SELECT | CTL_READ, 4, &cnt_be);
    uint32_t count = be32(cnt_be);
    for (uint32_t i = 0; i < count; i++) {
        struct FwCfgFile f;
        fwcfg_dma(CTL_READ, sizeof(f), &f);
        if (strcmp(f.name, name) == 0)
            return be16(f.select);
    }
    return -1;
}

void fb_init(void)
{
    int sel = fwcfg_find("etc/ramfb");
    if (sel < 0) return;
    struct RamfbCfg cfg;
    cfg.addr   = be64((uint64_t)fb_backing);
    cfg.fourcc = be32(DRM_XRGB8888);
    cfg.flags  = be32(0);
    cfg.width  = be32(FB_W);
    cfg.height = be32(FB_H);
    cfg.stride = be32(FB_W * 4);
    fwcfg_dma(((uint32_t)sel << 16) | CTL_SELECT | CTL_WRITE, sizeof(cfg), &cfg);
    fb_ready = 1;
}

int fb_is_ready(void) { return fb_ready; }

void fb_rect(int x, int y, int w, int h, uint32_t c)
{
    for (int j = 0; j < h; j++) {
        int py = y + j; if (py < 0 || py >= FB_H) continue;
        for (int i = 0; i < w; i++) {
            int px = x + i; if (px < 0 || px >= FB_W) continue;
            fb_backing[py * FB_W + px] = c;
        }
    }
}

void fb_gradient(uint32_t top, uint32_t bot)
{
    for (int y = 0; y < FB_H; y++) {
        int tr=(top>>16)&0xff, tg=(top>>8)&0xff, tb=top&0xff;
        int br=(bot>>16)&0xff, bg=(bot>>8)&0xff, bb=bot&0xff;
        int r=tr+(br-tr)*y/FB_H, g=tg+(bg-tg)*y/FB_H, b=tb+(bb-tb)*y/FB_H;
        uint32_t c=(r<<16)|(g<<8)|b;
        for (int x = 0; x < FB_W; x++) fb_backing[y*FB_W+x]=c;
    }
}

/* 5x7 font, ASCII 0x20..0x5A. Low 5 bits per row, bit4 = leftmost pixel.
 * This is the kernel's own glyph renderer -- what the WM/editor will draw with. */
static const uint8_t font5x7[][7] = {
/*20 sp*/{0,0,0,0,0,0,0},        /*21 !*/{0x04,0x04,0x04,0x04,0x04,0,0x04},
/*22*/{0,0,0,0,0,0,0},           /*23*/{0,0,0,0,0,0,0},
/*24*/{0,0,0,0,0,0,0},           /*25*/{0,0,0,0,0,0,0},
/*26*/{0,0,0,0,0,0,0},           /*27*/{0,0,0,0,0,0,0},
/*28 (*/{0x02,0x04,0x08,0x08,0x08,0x04,0x02}, /*29 )*/{0x08,0x04,0x02,0x02,0x02,0x04,0x08},
/*2a*/{0,0,0,0,0,0,0},           /*2b +*/{0,0x04,0x04,0x1F,0x04,0x04,0},
/*2c ,*/{0,0,0,0,0,0x04,0x08},   /*2d -*/{0,0,0,0x1F,0,0,0},
/*2e .*/{0,0,0,0,0,0x0C,0x0C},   /*2f /*/{0x01,0x02,0x02,0x04,0x08,0x08,0x10},
/*30 0*/{0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}, /*31 1*/{0x04,0x0C,0x04,0x04,0x04,0x04,0x0E},
/*32 2*/{0x0E,0x11,0x01,0x06,0x08,0x10,0x1F}, /*33 3*/{0x1F,0x02,0x04,0x02,0x01,0x11,0x0E},
/*34 4*/{0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}, /*35 5*/{0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E},
/*36 6*/{0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}, /*37 7*/{0x1F,0x01,0x02,0x04,0x08,0x08,0x08},
/*38 8*/{0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}, /*39 9*/{0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C},
/*3a :*/{0,0x0C,0x0C,0,0x0C,0x0C,0}, /*3b*/{0,0,0,0,0,0,0},
/*3c*/{0,0,0,0,0,0,0},           /*3d =*/{0,0,0x1F,0,0x1F,0,0},
/*3e*/{0,0,0,0,0,0,0},           /*3f*/{0,0,0,0,0,0,0},
/*40*/{0,0,0,0,0,0,0},
/*41 A*/{0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}, /*42 B*/{0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E},
/*43 C*/{0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}, /*44 D*/{0x1E,0x11,0x11,0x11,0x11,0x11,0x1E},
/*45 E*/{0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}, /*46 F*/{0x1F,0x10,0x10,0x1E,0x10,0x10,0x10},
/*47 G*/{0x0E,0x11,0x10,0x17,0x11,0x11,0x0F}, /*48 H*/{0x11,0x11,0x11,0x1F,0x11,0x11,0x11},
/*49 I*/{0x1F,0x04,0x04,0x04,0x04,0x04,0x1F}, /*4a J*/{0x07,0x02,0x02,0x02,0x12,0x12,0x0C},
/*4b K*/{0x11,0x12,0x14,0x18,0x14,0x12,0x11}, /*4c L*/{0x10,0x10,0x10,0x10,0x10,0x10,0x1F},
/*4d M*/{0x11,0x1B,0x15,0x15,0x11,0x11,0x11}, /*4e N*/{0x11,0x11,0x19,0x15,0x13,0x11,0x11},
/*4f O*/{0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}, /*50 P*/{0x1E,0x11,0x11,0x1E,0x10,0x10,0x10},
/*51 Q*/{0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}, /*52 R*/{0x1E,0x11,0x11,0x1E,0x14,0x12,0x11},
/*53 S*/{0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}, /*54 T*/{0x1F,0x04,0x04,0x04,0x04,0x04,0x04},
/*55 U*/{0x11,0x11,0x11,0x11,0x11,0x11,0x0E}, /*56 V*/{0x11,0x11,0x11,0x11,0x11,0x0A,0x04},
/*57 W*/{0x11,0x11,0x11,0x15,0x15,0x1B,0x11}, /*58 X*/{0x11,0x11,0x0A,0x04,0x0A,0x11,0x11},
/*59 Y*/{0x11,0x11,0x0A,0x04,0x04,0x04,0x04}, /*5a Z*/{0x1F,0x01,0x02,0x04,0x08,0x10,0x1F},
};

static void fb_char(int x, int y, char ch, int s, uint32_t color)
{
    if (ch >= 'a' && ch <= 'z') ch -= 32;           /* fold to uppercase */
    if (ch < 0x20 || ch > 0x5A) ch = 0x20;
    const uint8_t *g = font5x7[(int)ch - 0x20];
    for (int row = 0; row < 7; row++)
        for (int col = 0; col < 5; col++)
            if (g[row] & (1 << (4 - col)))
                fb_rect(x + col*s, y + row*s, s, s, color);
}

/* returns x advance */
int fb_text(int x, int y, const char *str, int s, uint32_t color)
{
    int cx = x;
    for (const char *p = str; *p; p++) {
        if (*p == '\n') { y += 8*s; cx = x; continue; }
        fb_char(cx, y, *p, s, color);
        cx += 6*s;
    }
    return cx;
}
