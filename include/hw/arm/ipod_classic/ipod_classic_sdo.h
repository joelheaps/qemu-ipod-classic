/*
 * S5L8702 display/scaler output block, 0x39300000 (TVOUT_SDO_MEM_BASE in the
 * touch 2G map; the classic 6G drives 0x391/0x392/0x393 during video playback).
 *
 * The retailOS registers VIC line 30 with handler 0x0808b66c when the video
 * engine starts. That handler acks this block by writing 1 to +0x280 and then
 * posts the Granite engine's wait event, which is what makes the AVS present
 * path run. QEMU raised no line 30, so the present
 * drain parked on ring+0x6c and the frame pool exhausted.
 *
 * Only the frame-done edge is modelled: a periodic timer raises IRQ line 30 at
 * the panel frame rate, and the guest's +0x280 write-1-to-clear acks it. The
 * scaler's pixel path is not modelled (the compositor at 0x38900000 already
 * puts the picture on the panel). The frame rate is UNDETERMINED, so the whole
 * thing is behind SDO_FRAME_IRQ (default off) exactly like LCD_FRAME_IRQ; set
 * SDO_FPS to override 60.
 */
#ifndef HW_ARM_IPOD_CLASSIC_SDO_H
#define HW_ARM_IPOD_CLASSIC_SDO_H

#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "qom/object.h"

#define TYPE_IPOD_CLASSIC_SDO "s5l8702.sdo"
OBJECT_DECLARE_SIMPLE_TYPE(IPodClassicSdoState, IPOD_CLASSIC_SDO)

/* 4 KiB window; every access the guest makes is in the low page. */
#define S5L8702_SDO_MEM_SIZE   0x1000
#define S5L8702_SDO_NREG       (S5L8702_SDO_MEM_SIZE / 4)

/* Frame-done status/ack. Read: bit 0 set while a frame is pending. Write 1:
 * ack, clears the pending bit and drops the line. */
#define S5L8702_SDO_FRAME_DONE 0x280

/* VIC0 line the guest registers for the display frame-done (handler 0x0808b66c). */
#define S5L8702_SDO_IRQ        30

struct IPodClassicSdoState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;
    QEMUTimer *frame_timer;
    uint32_t reg[S5L8702_SDO_NREG];

    bool frame_pending;
    uint64_t frames;        /* frames raised */
    uint64_t acks;          /* +0x280 acks */
    uint64_t irq_ns;        /* period */
};

#endif
