/*
 * PP502x DMA engine — one instance per block.
 *
 * See hw/arm/ipod_video/ipod_video_dma.c for the register map and where each
 * bit comes from, and hw/arm/ipod_video/README.md for the two blocks and their
 * interrupt lines.
 */

#ifndef HW_ARM_IPOD_VIDEO_DMA_H
#define HW_ARM_IPOD_VIDEO_DMA_H

#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "qom/object.h"

#define TYPE_PP502X_DMA "pp502x-dma"
OBJECT_DECLARE_SIMPLE_TYPE(PP502xDMAState, PP502X_DMA)

/*
 * A block is two pages: the master registers at +0x000 and the channels at
 * +0x1000, 0x20 bytes apart. Apple's channel constructor spells the
 * relationship out -- 0x1da2b4 does "base + channel*0x20 + 0x1000" -- and
 * pp5020.h agrees for the documented block, with DMA_MASTER_CONTROL at
 * 0x6000a000 and DMA0_BASE_ADDR at 0x6000b000.
 */
#define PP502X_DMA_REGION_SIZE      0x2000
#define PP502X_DMA_CHANNEL_BASE     0x1000
#define PP502X_DMA_CHANNEL_STRIDE   0x20

/*
 * How many channels are backed. Apple's driver instantiates two in block A and
 * four in block B (0x1da1c8 "cmp r5,#2" and 0x1da308 "cmp r5,#4"), but its boot
 * shutdown sweep clears the start bit on four and eight respectively, so the
 * larger figure is what has to answer without complaining. Eight is that
 * figure; nothing says the hardware has more.
 */
#define PP502X_DMA_NUM_CHANNELS     8

typedef struct {
    uint32_t cmd;       /* +0x00 control and byte count */
    uint32_t status;    /* +0x04 remaining count, INTR, BUSY */
    uint32_t ram_addr;  /* +0x10 memory side address */
    uint32_t ram_desc;  /* +0x14 memory side burst/width descriptor */
    uint32_t per_addr;  /* +0x18 peripheral port address */
    uint32_t per_desc;  /* +0x1c peripheral side burst/width descriptor */
} PP502xDMAChannel;

struct PP502xDMAState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;

    uint32_t master_control;
    uint32_t req_status;
    PP502xDMAChannel chan[PP502X_DMA_NUM_CHANNELS];
};

#endif
