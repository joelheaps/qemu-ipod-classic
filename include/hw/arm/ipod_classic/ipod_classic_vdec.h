/*
 * S5L8702 video decoder, 0x39800000.
 *
 * The video analogue of the SM1 audio engine (ipod_classic_sm1.h): the ARM
 * front-end parses the H.264 container/SPS/PPS/slice headers and offloads
 * pixel reconstruction to this block. It is undocumented (no Rockbox or
 * freemyipod name); the register map here
 * is from the guest submit routine FUN_081b65c4 and the context reset
 * FUN_081b6a14, decompiled with Ghidra. See hw/arm/ipod_classic/README.md,
 * "Video".
 *
 * DRAFT / STUB. Nothing is decoded yet: reads and writes latch into a register
 * file, and VDECTRACE=1 logs every access with addr/value/PC so the block's
 * semantics can be observed before a decoder is wired in. The map below is the
 * submit-side view only; the offsets are VERIFIED as accesses, their exact
 * meaning is not.
 */
#ifndef HW_ARM_IPOD_CLASSIC_VDEC_H
#define HW_ARM_IPOD_CLASSIC_VDEC_H

#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "hw/irq.h"
#include "qom/object.h"

#define TYPE_IPOD_CLASSIC_VDEC "s5l8702.vdec"
OBJECT_DECLARE_SIMPLE_TYPE(IPodClassicVdecState, IPOD_CLASSIC_VDEC)

/*
 * 1 MiB window like every other block, so an undecoded offset still reads 0
 * exactly as the io_floor did. Every register the retailOS touches is in the
 * low 4 KiB (context reset zeroes 0x00..0x12f), so only that page is latched.
 */
#define S5L8702_VDEC_MEM_SIZE  0x100000
#define S5L8702_VDEC_REG_BYTES 0x1000
#define S5L8702_VDEC_REG_WORDS (S5L8702_VDEC_REG_BYTES / 4)

/*
 * Register offsets, from FUN_081b65c4 (base = obj[0xe], i.e. [obj+0x38]).
 * Reads of an unmodelled offset return the latched word (zero at reset).
 */

/* Per-reference-frame Y/U/V plane addresses, one triple per entry in the
 * reference list (FUN_0809000c), written low to high. FUN_081b6a14 zeroes
 * these. */
#define S5L8702_VDEC_REF_TABLE   0x000
/* Current frame Y/U/V plane addresses: uVar10/uVar8/local_34 -> +0xcc/d0/d4.
 * Y at +0xcc, U at +0xd0, V at +0xd4 (masked 0x7fffffff). */
#define S5L8702_VDEC_CUR_Y       0x0cc
#define S5L8702_VDEC_CUR_U       0x0d0
#define S5L8702_VDEC_CUR_V       0x0d4
/* obj[3] copied straight through. */
#define S5L8702_VDEC_OBJ3        0x0d8
/* Descriptor-table pointer (obj[7]). One 8-word descriptor per slice. */
#define S5L8702_VDEC_DESC_TABLE  0x0dc
/* Packed size: width in bits 0..5, height in bits 8..13
 * (`&0xffffc0ff | height<<8`, then `&0xffffffc0 | width`). */
#define S5L8702_VDEC_SIZE        0x0e0
/* Second packed word (obj[0x61] << 16 | obj[0x62]). Role unknown. */
#define S5L8702_VDEC_DIM2        0x0e4
/* Control/size: bits 0..11 = width*height*2, bits 16..18 = slice count,
 * then OR 0x88003001 (bit0 = start). */
#define S5L8702_VDEC_CTRL        0x0e8
#define S5L8702_VDEC_CTRL_START  0x00000001u
#define S5L8702_VDEC_CTRL_MAGIC  0x88003001u
/* Context control. FUN_081b6a64 clears bit 0. */
#define S5L8702_VDEC_CTX         0x0ec
/* Status, checked as done: `(v & 1)`. */
#define S5L8702_VDEC_STATUS_F0   0x0f0
/* Status: done/success when bit 29 is CLEAR and
 * `((v & 0x1fffffff) >> 18) == 0`. */
#define S5L8702_VDEC_STATUS_F4   0x0f4
/* Result word, written from DAT_081b69a0/69a4 on submit/complete. */
#define S5L8702_VDEC_RESULT      0x118

/*
 * Done status. +0xf0 bit 0 is checked as "completed"; +0xf4 is checked as
 * success when bit 29 is CLEAR and ((v & 0x1fffffff) >> 18) == 0. Both are
 * device-owned: the guest's context reset writes 0 to them, so writes are
 * ignored and reads return the model's status.
 */
#define S5L8702_VDEC_DONE        0x00000001u

/* Geometry packed in +0xe0: width macroblocks in bits 0..5, height in 8..13. */
#define S5L8702_VDEC_MB_W(v)     ((int)((v) & 0x3fu) * 16)
#define S5L8702_VDEC_MB_H(v)     ((int)(((v) >> 8) & 0x3fu) * 16)
/* Reference frame Y/U/V plane addresses (+0x120/0x124/0x128) and the
 * reference-set flag +0x12c = 1. */
#define S5L8702_VDEC_REF_Y       0x120
#define S5L8702_VDEC_REF_U       0x124
#define S5L8702_VDEC_REF_V       0x128
#define S5L8702_VDEC_REF_FLAG    0x12c

/* VIC0 line for the video-decoder done interrupt (guest handler 0x0804b36c). */
#define S5L8702_VDEC_IRQ         35

#include "hw/arm/ipod_classic/ipod_classic_vdec_dec.h"

struct IPodClassicVdecState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;
    uint32_t reg[S5L8702_VDEC_REG_WORDS];

    /* Device-owned status (see S5L8702_VDEC_DONE); not in the latch file. */
    uint32_t status_f0;
    uint32_t status_f4;

    /*
     * Done interrupt. The line is pulsed; the edge-IC latches the edge and the
     * guest handler (0x0804b36c) acks it at +0xc.
     */
    uint64_t irqs;

    /*
     * VDEC_DECODE=1 (default off): the model reads the access unit and codec
     * config from files dumped by the pctrace audump= tap and decodes them with
     * libavcodec. This is a labelled host-side shortcut for the fact that the
     * firmware strips the AU in place before submit; with it off the block is
     * the latch-only stub.
     */
    bool decode;
    char *au_path;
    char *cfg_path;
    VdecDec *dec;
    uint64_t decodes;
    uint64_t dec_errors;

    /* VDECTRACE=1: log every register access with value and guest PC. */
    bool trace;
    uint64_t accesses;
};

#endif
