/*
 * ARM PrimeCell PL080/PL081 DMA controller
 *
 * Copyright (c) 2006 CodeSourcery.
 * Copyright (c) 2018 Linaro Limited
 * Written by Paul Brook, Peter Maydell
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 or
 * (at your option) any later version.
 */

/*
 * This is a model of the Arm PrimeCell PL080/PL081 DMA controller:
 * The PL080 TRM is:
 * https://developer.arm.com/documentation/ddi0196/latest
 * and the PL081 TRM is:
 * https://developer.arm.com/documentation/ddi0218/latest
 *
 * QEMU interface:
 * + sysbus IRQ 0: DMACINTR combined interrupt line
 * + sysbus IRQ 1: DMACINTERR error interrupt request
 * + sysbus IRQ 2: DMACINTTC count interrupt request
 * + sysbus MMIO region 0: MemoryRegion for the device's registers
 * + QOM property "downstream": MemoryRegion defining where DMA
 *   bus master transactions are made
 */

#ifndef HW_DMA_PL080_H
#define HW_DMA_PL080_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define PL080_MAX_CHANNELS 8

typedef struct {
    uint32_t src;
    uint32_t dest;
    uint32_t lli;
    uint32_t ctrl;
    uint32_t conf;
} pl080_channel;

#define TYPE_PL080 "pl080"
#define TYPE_PL081 "pl081"
OBJECT_DECLARE_SIMPLE_TYPE(PL080State, PL080)

struct PL080State {
    SysBusDevice parent_obj;

    MemoryRegion iomem1;
    MemoryRegion iomem2;
    uint8_t tc_int;
    uint8_t tc_mask;
    uint8_t err_int;
    uint8_t err_mask;
    uint32_t conf;
    uint32_t sync;
    uint32_t req_single;
    uint32_t req_burst;
    pl080_channel chan[PL080_MAX_CHANNELS];
    int nchannels;
    /* Flag to avoid recursive DMA invocations.  */
    int running;
    /*
     * Request lines (DMACBREQ index) that must be paced by a peripheral, one
     * bit per line. Default 0: a line not listed here transfers at full speed
     * exactly as before, so this is a no-op unless a peripheral opts in. When a
     * line IS listed, a mem->peri (flow 1) or peri->mem (flow 2) transfer on it
     * only advances while the peripheral has asserted a request, and the
     * request is consumed at each terminal-count -- one chunk per request pulse.
     * This is what lets the iPod's I2S drain the audio DMA at the sample rate
     * instead of all at once. See hw/arm/ipod_classic/ipod_classic_i2s.c.
     */
    uint32_t paced_lines;
    /*
     * Bitmap of paced channels that have completed their terminal LLI (link==0)
     * but are kept ENABLED instead of auto-disabling, so a concurrent guest
     * relink cannot observe CONFIG.E==0 and panic "DMA ch link error". The bit
     * is set in pl080_run and cleared when the guest relinks (pl080_write LLI),
     * reloads the channel (Control with a non-zero count) or disables it
     * (CONFIG.E=0). Never set for non-paced lines, so stock DMA is unaffected.
     */
    uint32_t paced_pending;
    /*
     * Terminal-count tallies, one per channel, kept only while DMATRACE is set.
     * They exist to answer WHICH channel raised a DMA interrupt: a global ISR
     * hit count says the controller is alive but not whether the audio channel
     * is the one doing the work. Not migrated -- purely observational.
     */
    uint32_t tc_count[PL080_MAX_CHANNELS];
    qemu_irq irq;
    qemu_irq interr;
    qemu_irq inttc;

    MemoryRegion *downstream;
    AddressSpace downstream_as;
};

/*
 * Mark a DMA request line as paced (or not). A paced line only advances a
 * mem<->peri transfer when the peripheral has asserted a request via
 * pl080_pace_request(). No effect on any other line.
 */
void pl080_set_paced_line(PL080State *s, int line, bool on);

/*
 * The peripheral asserts a burst request on a paced line: let the controller
 * hand over up to one chunk (until the next terminal-count), then stall again.
 * Call once per drain tick.
 */
void pl080_pace_request(PL080State *s, int line);

#endif
