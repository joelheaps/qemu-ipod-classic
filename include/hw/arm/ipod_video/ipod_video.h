/*
 * Apple iPod 5th generation ("Video") — PortalPlayer PP5020/PP5022 SoC.
 *
 * Addresses come from Rockbox's firmware/export/pp5020.h and the boot model
 * from bootloader/ipod.c. See hw/arm/ipod_video/README.md for the derivation
 * and the device this was taken off.
 */

#ifndef HW_ARM_IPOD_VIDEO_H
#define HW_ARM_IPOD_VIDEO_H

#include "exec/hwaddr.h"
#include "hw/boards.h"
#include "hw/arm/boot.h"
#include "cpu.h"
#include "qemu/timer.h"
#include "hw/qdev-core.h"
#include "exec/memory.h"

#define TYPE_IPOD_VIDEO "iPod-Video"

#define TYPE_IPOD_VIDEO_MACHINE   MACHINE_TYPE_NAME(TYPE_IPOD_VIDEO)
#define IPOD_VIDEO_MACHINE(obj) \
    OBJECT_CHECK(IPodVideoMachineState, (obj), TYPE_IPOD_VIDEO_MACHINE)

/*
 * DRAM_START in pp5020.h, and the same value the firmware partition's
 * directory carries in its addr field for every image.
 */
#define DRAM_MEM_BASE       0x10000000
/*
 * 64 MB, which is what this unit reports -- and which two other sources
 * contradict. Apple's own firmware carries 32 MB as its built-in default
 * (0x1f4 does "mov fp, #0x700", and nibble 2 of that indexes the table at
 * 0x16d8 to 0x02000000), and iPodLinux's ipodhw.c lists mem_size 0x02000000
 * for hw_ver 0xb. Left at 64 MB deliberately: Rockbox boots on it and must not
 * be disturbed, both figures clear the firmware's "image plus bss must fit"
 * assert at 0xda0 (0x00a0e294), and the SysInfo block below reports whatever
 * this says rather than a number of its own, so the machine stays self
 * consistent either way. Worth revisiting when something is seen to size a
 * buffer off 0x40006020.
 */
#define DRAM_MEM_SIZE       0x04000000

/*
 * Address bit 26 is not decoded by the SDRAM controller: DRAM answers both
 * with it clear and with it set, so every DRAM address has a twin 0x04000000
 * higher. Apple's firmware relies on this and Rockbox never touches it, which
 * is why it took until now to notice.
 *
 * VERIFIED, from the firmware's own scatter-load table at 0x84040..0x840a8 in
 * apple_retailos.bin. Sources and destinations there run in one contiguous
 * sweep that crosses the bit:
 *
 *   src 0x006c9a64 + len 0x0004c734 = 0x00716198, and the next src is
 *   0x0471619c -- 0x00716198 + 4 with bit 26 stuck on.
 *   dst 0x1493690c + len 0x0004c734 = 0x14983040, the next dst.
 *
 * A discontinuity of exactly 0x04000000 in the middle of a run of consecutive
 * file offsets is not an address change; it is the same address wearing a flag.
 * The firmware also builds such addresses at runtime rather than loading them
 * whole: 0xdb8 does "bic r0, r2, #0xfc000000" then 0xdbc "orr r0, r0,
 * #0x14000000".
 *
 * INFERRED: what the bit means to the hardware. The PP502x remap unit's mask
 * field says the same thing indirectly -- MMAP1 is programmed at 0x228 with
 * logical 0x00003a00 / physical 0x10000f84, and comparing that against the
 * masks Rockbox's crt0-pp.S uses (0x3e00 for a 32 MB window, 0x3c00 for 64 MB)
 * puts mask bit k over address bit k+16, so bits 26 and 27 of a logical
 * address are passed through to the physical bus untouched. Something below
 * has to be ignoring bit 26, and the only candidate is the memory controller.
 * Whether it also means uncached, unbuffered or nothing at all is not known,
 * and nothing here depends on the answer: an alias behaves identically either
 * way as long as there is no cache to be coherent with.
 */
#define DRAM_ALIAS_BIT      0x04000000
/*
 * And address bit 31 is the uncached view of the same memory. Apple's firmware
 * hands out every buffer through it.
 *
 * VERIFIED, and this one comes with its own explanation rather than needing to
 * be inferred from a discontinuity. Ten buffer constructors in the image share
 * one idiom -- 0x22131c, and nine in the allocator module at 0x28c8fc,
 * 0x28c9dc, 0x28cab4, 0x28cb80, 0x28cca8, 0x28cd74, 0x28ce0c, 0x28ced0 and
 * 0x28d0ec. Written out, 0x28cebc onwards:
 *
 *     str  r0, [r4, #4]         ; the aligned buffer pointer
 *     ldrb r1, [r4, #12]        ; "caller says it is already uncached"
 *     cmp  r1, #0
 *     bne  done
 *     mov  r1, #4
 *     cmp  r1, r0, lsr #28      ; top nibble 4, i.e. IRAM?
 *     beq  done                 ; then leave it alone
 *     orr  r0, r0, #0x80000000  ; otherwise this is how the buffer is used
 *     str  r0, [r4, #4]
 *     ...
 *     bl   0x9aa18              ; over [ptr - slack, ptr + size + slack]
 *
 * The exemption is what identifies the bit: IRAM is the one memory on this
 * chip that is never cached, and it is exactly the one the firmware does not
 * tag. Rockbox says the same thing in C, with a different constant because it
 * runs its cached view somewhere else -- system-target.h:164, "#define
 * UNCACHED_ADDR(a) ((typeof (a))((uintptr_t)(a) | UNCACHED_BASE_ADDR))".
 *
 * Caught by the retailOS taking a data abort reading 0x93EE1B70 -- a 512-byte
 * memcpy at 0x21701c out of a 32 KB disk block buffer whose descriptor, at
 * 0x13ee9b90, holds the raw allocation at +0 and that same address plus bit 31
 * at +4.
 *
 * There is no cache in this model, so the uncached view is the same memory: an
 * alias, exactly like the bit-26 twin. If a cache is ever modelled this stops
 * being a detail and starts being the whole point of the bit.
 */
#define DRAM_UNCACHED_BIT   0x80000000

/*
 * IRAMORIG in Rockbox's app-pp.lds. That script only claims 0xc000 for the
 * application, which is a budget rather than the size of the hardware, so this
 * started out as a deliberately generous guess.
 *
 * It is no longer a guess. Apple's firmware hands out its globals at
 * IRAM_END - 0x100 + N (0x1084, via 0x598) and picks IRAM_END from a single
 * character of PP_VER1: 0x40020000 for a PP5022, 0x40018000 otherwise. So the
 * firmware itself puts the top of IRAM at 0x40020000 on this chip, which is
 * 128 KB, and that is what makes the SysInfo letterbox at 0x4001ff18 land
 * where iPodLinux says it does. 96 KB would be the PP5020 figure.
 */
#define IRAM_MEM_BASE       0x40000000
#define IRAM_MEM_SIZE       0x00020000

/*
 * The last 256 bytes of IRAM are the boot ROM's letterbox. iPodLinux's
 * ipodloader2/ipodhw.c reads a magic word and a pointer from the top of it and
 * comments that "the last 256 bytes appear to be used for special things by
 * the Flash ROM":
 *
 *     #define SYSINFO_TAG_PP5022 0x4001ff18
 *     #define SYSINFO_PTR_PP5022 0x4001ff1c
 *
 * Apple's firmware agrees to the byte. 0x1084 hands out globals at
 * IRAM_END - 0x100 + N; 0x13bc asks it for N = 24, reads the pointer at +4 and
 * compares the word at +0 against 0x53797349 ("IsyS"), which is loaded by
 * exactly one instruction in the whole image (0x13fc) and only ever compared,
 * never stored. Nothing in the osos writes either word, so whatever is there
 * was left by the ROM we skip -- exactly like GPO32_VAL bit 14.
 */
#define SYSINFO_TAG_ADDR    0x4001ff18
#define SYSINFO_PTR_ADDR    0x4001ff1c
#define SYSINFO_MAGIC       0x53797349   /* "IsyS" */

/*
 * Where we put the block the pointer points at. The ROM's own choice is not
 * known -- no dump of it exists -- so this is ours: the 256 bytes immediately
 * below the letterbox. It is clear of everything the firmware uses early on:
 * the overlay copied by 0x8392c ends at 0x4001ded4, the reset stack starts at
 * 0x40003ff8 and grows down, and the scatter-load at 0x83848 stops at
 * 0x40001554.
 */
#define SYSINFO_BLOCK_ADDR  0x4001fe00
#define SYSINFO_BLOCK_SIZE  0x100

/*
 * The four fields of it we can account for, and only those. Each one is here
 * because a disassembly shows the firmware reading that exact offset and what
 * it does with the answer; the other 240 bytes stay at zero because we do not
 * know, and guessing them from iPodLinux's sysinfo_t would be inventing with
 * the appearance of rigour -- that struct does not even agree with this
 * firmware (it puts sdram_size at +0x80, while what gets read here is +0xe0,
 * which falls inside a pad iPodLinux never decoded).
 *
 *   +0x04  length. 0x281238 reads it, clamps it to 248 and uses it as a
 *          memcpy count.
 *
 *          +0x00 is left alone. iPodLinux's struct starts with its own "IsyS"
 *          there, but no instruction in this image reads it, so we have
 *          nothing to go on and write nothing.
 *
 *   +0x84  hardware id, model in the top half and revision in the bottom.
 *          0x26516c reads it out of the installed block; 0x2653a4 shifts the
 *          top half down and runs it through a 13-entry jump table at
 *          0x2653c8 that answers 1, 2 or 3; 0x111c58 turns that into one of
 *          three driver objects and returns null for anything else. Our
 *          IPOD_VIDEO_HW_REVISION, which came off the real device through
 *          Rockbox's HW info screen, has 0x000b on top: entry 11, which
 *          answers 3. Apple's own table having a live entry exactly where the
 *          device's own revision points is the check that this offset really
 *          is the hardware id.
 *
 *   +0xe0  SDRAM size in bytes. 0x13e8 reads it and hands it to 0xe6c, which
 *          rounds it down to a power of two against the table at 0x16d8 and
 *          encodes it as the size of bank 2; 0xdf8 sums the four banks into
 *          0x40006020.
 *
 * The magic itself is not in the block: it is the word beside the pointer, at
 * SYSINFO_TAG_ADDR. 0x13f8 loads it from there.
 *
 * How the block reaches the code that reads +0x84, all verified:
 *   0x140c   [0x4000608c] = pointer, if the magic matched
 *   0x2b8    bl 0x1658, memcpy(0x14983060, 0x40006000, 0x90) -- the whole
 *            memory-config block, pointer included, into DRAM
 *   0x2bc    bl 0x281224, which reads [0x149830ec] (that pointer), returns
 *            immediately if it is null, and otherwise copies the block to
 *            0x108710dc and installs it as the hardware-info singleton
 *   0x2d0    bl 0x2668c4 -> 0x111c58 -> 0x2653a4 -> 0x265164, which reads
 *            +0x84 out of the singleton
 */
#define SYSINFO_LENGTH_OFF      0x04
#define SYSINFO_HW_REV_OFF      0x84
#define SYSINFO_SDRAM_SIZE_OFF  0xe0

/*
 * The PP's NOR flash. It is what address 0 answers to while the bootloader
 * runs; afterwards it is remapped to 0x20000000. Rockbox's hwcompat.h reads
 * the board revision from 0x405c through the first window and 0x2000405c
 * through the second.
 */
#define FLASH_ROM_MEM_BASE      0x00000000
#define FLASH_ROM_MEM_SIZE      0x00100000
#define FLASH_ROM_REMAP_BASE    0x20000000
#define IPOD_HW_REVISION_OFF    0x405c

/*
 * What the device on the desk reports, straight out of Rockbox's HW info
 * screen. The top half identifies the family: 0x000b is what ipodpatcher
 * calls "Video (aka 5th Generation)".
 */
#define IPOD_VIDEO_HW_REVISION  0x000b0005

/* The Broadcom BCM2722 that owns the framebuffer and drives the panel. */
#define BCM_MEM_BASE        0x30000000

/* Peripherals. */
#define PROC_ID_MEM_BASE    0x60000000   /* PROCESSOR_ID: 0x55 = CPU, 0xaa = COP */
#define INTC_MEM_BASE       0x60004000   /* CPU_INT_EN at +0x24, hi bank +0x120 */
#define TIMER_MEM_BASE      0x60005000   /* TIMER1_CFG +0x00, USEC_TIMER +0x10 */
#define CLOCK_MEM_BASE      0x60006000   /* DEV_EN +0x0c, PLL_CONTROL +0x34 */
#define PROC_CTL_MEM_BASE   0x60007000   /* CPU_CTL +0x00, COP_CTL +0x04 */
#define CACHE_CTL_MEM_BASE  0x6000c000   /* includes CACHE_CTL_VECT_REMAP */
#define GPIO_MEM_BASE       0x6000d000
/*
 * Two DMA blocks, identical register for register. Only the second is in
 * pp5020.h (DMA_MASTER_CONTROL 0x6000a000, DMA0..3 at 0x6000b000); the first
 * is documented by nothing but Apple's own firmware, which loads 0x60008000 at
 * 0x1da198 and hangs two channels off it. Each block's master page is at the
 * base and its channels 0x1000 higher.
 */
#define DMA_A_MEM_BASE      0x60008000
#define DMA_B_MEM_BASE      0x6000a000
#define IDE_MEM_BASE        0xc3000000   /* the disk, IDE_IRQ 23 */
/*
 * ATA_DATA..ATA_COMMAND live at IDE_BASE + 0x1e0 four bytes apart and
 * ATA_CONTROL at +0x3f8, per Rockbox's ata-target.h. Everything below the
 * task file is PP-specific timing and configuration.
 */
#define IDE_TASKFILE_OFF    0x1e0
#define IDE_CONTROL_OFF     0x3f8
#define DEV_CTL_MEM_BASE    0x70000000   /* DEV_INIT1/2, GPO32_VAL, GPO32_ENABLE */
/*
 * One window, two unrelated blocks. The I2C controller has +0x00..+0xff and
 * the click wheel +0x100 upwards, and they even share an interrupt line. Which
 * of the two owns the window was got wrong once already, in both directions:
 * the whole thing was first labelled I2C, then relabelled click wheel when
 * Rockbox was seen reading CLICKWHEEL_DATA at 0x7000c140. Both were half
 * right.
 */
#define I2C_MEM_BASE        0x7000c000
#define CLICKWHEEL_MEM_BASE 0x7000c000
#define CLICKWHEEL_DATA_OFF 0x140
#define I2S_MEM_BASE        0x70002800
#define LCD_MEM_BASE        0x70003000
#define SER0_MEM_BASE       0x70006000   /* 16550-compatible, stride 4 */
#define SER1_MEM_BASE       0x70006040
#define USB_MEM_BASE        0xc5000000
/*
 * The cache's own arrays, which are addressable memory rather than registers:
 * data at 0xf0000000, its mirror at 0xf0002000, and the per-core line status
 * at 0xf0004000 (CPU) and 0xf0006000 (COP), with flush and invalidate windows
 * at 0xf0008000 and 0xf000c000. Rockbox writes the whole status array by hand
 * in cache_invalidate_special(), so it has to be backed by something.
 */
#define CACHE_ARRAY_MEM_BASE    0xf0000000
#define CACHE_ARRAY_MEM_SIZE    0x0000f000   /* up to the remap unit */
#define MMAP_MEM_BASE       0xf000f000   /* remap unit, MMAP0..7 */

/* IRQ numbers from pp5020.h. The high bank starts at 32. */
#define PP_IDE_IRQ          23
/*
 * DMA_IRQ in pp5020.h is 26, and it belongs to the block that header
 * documents, which is the one at 0x6000a000. The other block's line is 27, and
 * that comes from the retailOS rather than from Rockbox, which never saw the
 * block at all: 0x1d98d0 registers block B's handler under the firmware's own
 * interrupt index 27 and block A's under 28, and the table built at 0x1fcaa0
 * translates index 27 to hardware line 26 (0x1fcc38) and index 28 to line 27
 * (0x1fcc44). Index 27 landing on Rockbox's documented DMA_IRQ is what makes
 * the pair trustworthy; a running machine agrees, with CPU_INT_EN_STAT reading
 * 0xcc002017 -- lines 26 and 27 enabled, 28 not.
 */
#define PP_DMA_B_IRQ        26
#define PP_DMA_A_IRQ        27
#define PP_SER0_IRQ         (32 + 4)
/* The click wheel shares the I2C line; see button-clickwheel.c. */
#define PP_I2C_IRQ          (32 + 8)

/* Identity read back by each core so it can tell which one it is. */
#define PP_PROC_ID_CPU      0x55
#define PP_PROC_ID_COP      0xaa

/*
 * The two cores, in the order QEMU creates them, so cpu_index doubles as the
 * core number. Both start at the same reset vector and the firmware sorts
 * itself out by reading PROCESSOR_ID.
 */
#define PP_CORE_CPU         0
#define PP_CORE_COP         1
#define PP_NUM_CORES        2

/*
 * CPU_CTL / COP_CTL bits. Bit 31 doubles as the "this core is asleep" status
 * and as the request to put it to sleep; crt0-pp-bl.S spins on the COP's until
 * it is set before touching anything else.
 */
#define PP_PROC_SLEEPING    0x80000000
#define PP_PROC_CTL_CPU     0x00
#define PP_PROC_CTL_COP     0x04

typedef struct {
    MachineClass parent;
} IPodVideoMachineClass;

typedef struct {
    MachineState parent;
    AddressSpace *nsas;
    ARMCPU *cpu[PP_NUM_CORES];
    /*
     * The ROM loader copies the whole osos image to DRAM_START and jumps to
     * DRAM_START + entryOffset, which is where ipodpatcher put the Rockbox
     * bootloader. Offset 0 runs Apple's firmware instead.
     */
    uint32_t entry_offset;
    hwaddr reset_pc;
    /* Answer ARM semihosting calls; Apple's firmware panics through them. */
    bool semihosting;
    /* CPU_CTL and COP_CTL, indexed by core. The sleep bit is not stored
     * here: it is read back from the core's own halted state. */
    uint32_t proc_ctl[PP_NUM_CORES];
    uint32_t timer1_cfg;
    uint32_t timer2_cfg;
    uint32_t hw_revision;
    DeviceState *intc;
    QEMUTimer *timer1;
    QEMUTimer *timer2;
    bool timer_pending[2];
    uint32_t cache_ctl;
    MemoryRegion *vect_remap;
    MemoryRegion *dram_remap;
    /* The bit-26 twin of dram_remap, switched in with it. */
    MemoryRegion *dram_remap_alias;
    bool dram_remapped;
    bool force_remap;
    uint32_t mmap_regs[1024];
    uint32_t gpio_regs[1024];
    uint32_t wheel_regs[1024];
    uint32_t wheel_buttons;
    bool wheel_pending;
    bool wheel_touched;
    uint32_t wheel_position;
    uint32_t clock_regs[1024];
    uint32_t dev_regs[1024];
} IPodVideoMachineState;

#endif
