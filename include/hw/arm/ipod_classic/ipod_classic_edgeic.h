/*
 * S5L8702 edge interrupt controller, 0x38e02000.
 *
 * The block sits behind the two PL192 VICs (VIC0 0x38e00000, VIC1 0x38e01000)
 * and latches edge-triggered sources. The guest's video done handler
 * (0x0804b36c) acks it with `str 8, [+0xc]`; the decoder's own enable path
 * (0x0804b354) sets bit 3 of `+0x4`. Before this model it was plain RAM, so the
 * ack could not clear a source and a level line from a device re-entered the
 * handler for ever. Modelling the latch makes the ack meaningful.
 *
 * Only the bits the machine actually drives are modelled; the rest is an
 * honest register latch.
 */
#ifndef HW_ARM_IPOD_CLASSIC_EDGEIC_H
#define HW_ARM_IPOD_CLASSIC_EDGEIC_H

#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "qom/object.h"

#define TYPE_IPOD_CLASSIC_EDGEIC "s5l8702.edgeic"
OBJECT_DECLARE_SIMPLE_TYPE(IPodClassicEdgeICState, IPOD_CLASSIC_EDGEIC)

#define EDGEIC_MEM_SIZE 0x1000
#define EDGEIC_NREG     (EDGEIC_MEM_SIZE / 4)
#define EDGEIC_NIN      32

struct IPodClassicEdgeICState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;               /* OR of pending & enable -> a VIC line */
    uint32_t reg[EDGEIC_NREG];  /* honest readback for the rest */
    uint32_t pending;           /* latched rising edges */
    uint32_t enable;            /* +0x4 */
};

#endif
