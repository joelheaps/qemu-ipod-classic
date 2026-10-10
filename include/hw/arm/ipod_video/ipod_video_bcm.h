/*
 * Broadcom BCM2722 video companion chip of the iPod 5G.
 *
 * See hw/arm/ipod_video/ipod_video_bcm.c for the port protocol,
 * hw/arm/ipod_video/ipod_video_vc.c for the VideoCore host interface that
 * rides on top of it, and hw/arm/ipod_video/README.md for where it sits in the
 * memory map.
 */

#ifndef HW_ARM_IPOD_VIDEO_BCM_H
#define HW_ARM_IPOD_VIDEO_BCM_H

#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "ui/console.h"
#include "qom/object.h"

#define TYPE_IPOD_VIDEO_BCM "ipod-video.bcm2722"
OBJECT_DECLARE_SIMPLE_TYPE(IPodVideoBCMState, IPOD_VIDEO_BCM)

/*
 * Eight registers 64 KB apart, from BCM_DATA at +0 through BCM_ALT_CONTROL at
 * +0x70000 in Rockbox's lcd-video.c.
 */
#define BCM_MMIO_SIZE   0x80000

/*
 * The chip's own address space is sparse, and two windows of it are used.
 *
 * The low one starts at BCMA_SRAM_BASE, which is address 0: the mailbox at
 * 0x1F8/0x1FC lives there, both guests upload the ~200 KB VMCS firmware image
 * to it, and Rockbox writes frames at 0xE0000.
 *
 * Four megabytes rather than two, because the retailOS asks the chip to find
 * room for its display surfaces and we answer with addresses in here (see
 * VC_SURFACE_HEAP in ipod_video_vc.c). The real BCM2722 has its own SDRAM for
 * exactly that; how much, and where the firmware would have put a surface, is
 * UNDETERMINED, so the amount here is ours and only has to be enough.
 */
#define BCM_MEM_SIZE    0x00400000

/*
 * The high one is the VideoCore's control registers. Only two words of it are
 * used by either driver -- 0x10000C00, which releases the uploaded firmware
 * and reports back that it is executing, and 0x10000400, the doorbell -- but
 * Rockbox's bcm_powerdown() also touches 0x10001400 (lcd-video.c:515), so the
 * window runs to 0x10002000.
 */
#define BCM_CTRL_BASE   0x10000000
#define BCM_CTRL_SIZE   0x00002000

#define LCD_WIDTH       320
#define LCD_HEIGHT      240

/*
 * The chip has two identical host ports, the primary and the alternate, 64 KB
 * apart in the register window. They address the same chip memory but have
 * their own pointers: Rockbox only ever uses the primary, while the retailOS
 * uses both at once, pushing image data through the alternate one (0x164524
 * and 0x165024 pass port 1) while other tasks use the primary.
 */
typedef struct {
    uint32_t wr_addr;   /* auto-incrementing write pointer */
    uint32_t rd_addr;   /* auto-incrementing read pointer */
    uint32_t wr_start;  /* where the current run of writes was aimed */
} IPodVideoBCMPort;

#define BCM_NUM_PORTS   2

/*
 * A display surface the retailOS asked the VideoCore to create (channel type
 * 2, opcode 8 -- 0x286ca8). We pick where in chip memory it lives and hand the
 * address back in the reply; the guest then pushes pixels straight into it.
 */
#define VC_MAX_SURFACES 8

typedef struct {
    bool     valid;
    uint32_t addr;      /* chip address of the pixels, ours to choose */
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t size;
    uint8_t  format;    /* the code 0x286a1c assigns: 1 = 16bpp, 15 = 32bpp */
} IPodVideoVCSurface;

struct IPodVideoBCMState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;
    QemuConsole *con;

    uint8_t *mem;           /* the chip's low address space */
    uint8_t *ctrl;          /* its control registers at 0x10000000 */
    IPodVideoBCMPort port[BCM_NUM_PORTS];
    /* True while the uploaded VideoCore firmware is considered to be running. */
    bool vc_running;
    bool dirty;

    /* The host interface we publish once the VideoCore is released. */
    bool vc_iface_live;
    IPodVideoVCSurface surface[VC_MAX_SURFACES];
    uint32_t vc_heap;       /* bump allocator over the surface area */

    /* What the console scans out, and whether it needs rebuilding. */
    uint32_t scanout_addr;
    uint32_t scanout_width;
    uint32_t scanout_height;
    uint32_t scanout_stride;
    uint8_t  scanout_format;
    bool     scanout_changed;
};

/* ipod_video_vc.c -- the VideoCore's host interface over chip memory. */
void ipod_video_vc_reset(IPodVideoBCMState *s);
void ipod_video_vc_publish(IPodVideoBCMState *s);
void ipod_video_vc_wrote(IPodVideoBCMState *s, uint32_t addr, unsigned size);
void ipod_video_vc_kick(IPodVideoBCMState *s);

/* ipod_video_bcm.c -- what the VideoCore model asks the console to show. */
void ipod_video_bcm_scanout(IPodVideoBCMState *s, uint32_t addr,
                            uint32_t width, uint32_t height, uint32_t stride,
                            uint8_t format);

#endif
