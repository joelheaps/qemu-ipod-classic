/*
 * Apple iPod 5th generation ("Video"), PortalPlayer PP5020/PP5022.
 *
 * First milestone: enough of the SoC to load the osos image the way the ROM
 * loader does and start executing it, with a working serial console. Every
 * peripheral we know the address of but have not modelled is registered as an
 * unimplemented device, so -d unimp reports what the firmware reaches for
 * next.
 *
 * Both cores are created and both are wired to the interrupt controller, and
 * the remap unit at 0xf000f000 is modelled coarsely; an older version of this
 * comment said neither was, and was wrong for long enough to send two
 * investigations down the wrong path.
 *
 * Not modelled, and worth knowing before reading anything into a run:
 *
 *   - Audio: the I2S block and the WM8758 codec. There is a modelled I2C bus
 *     now, but nothing is on it except the PCF50605 power management chip at
 *     address 0x08; the codec's address 0x1a is empty, and so is 0x2a, which
 *     Apple's firmware talks to and no public source names.
 *   - Executing the Broadcom's firmware. The upload path itself is modelled:
 *     the retailOS DMAs all 201376 bytes of the real vmcs image into the
 *     chip's memory and we answer as if it were running, but nothing ever
 *     executes it. GPO32 bit 14 is faked as already set so that Rockbox, which
 *     would otherwise try to upload it too, counts the panel as powered.
 *
 * See hw/arm/ipod_video/README.md.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "hw/arm/boot.h"
#include "hw/loader.h"
#include "elf.h"
#include "exec/address-spaces.h"
#include "hw/misc/unimp.h"
#include "hw/irq.h"
#include "hw/char/serial.h"
#include "sysemu/sysemu.h"
#include "sysemu/reset.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "hw/arm/ipod_video/ipod_video.h"
#include "hw/arm/ipod_video/ipod_video_bcm.h"
#include "hw/arm/ipod_video/ipod_video_intc.h"
#include "hw/sysbus.h"
#include "hw/arm/ipod_video/ipod_video_ide.h"
#include "hw/arm/ipod_video/ipod_video_i2c.h"
#include "hw/arm/ipod_video/ipod_video_pcf50605.h"
#include "hw/arm/ipod_video/ipod_video_dma.h"
#include "hw/i2c/i2c.h"
#include "sysemu/blockdev.h"
#include "hw/qdev-properties.h"
#include "ui/console.h"
#include "semihosting/semihost.h"

/*
 * PROCESSOR_ID, the very first thing the firmware touches. Both cores run the
 * same image out of the same memory and tell themselves apart by reading this
 * byte: 0x55 means "you are the CPU", 0xaa means "you are the COP". Rockbox's
 * crt0-pp.S branches on it before doing anything else, so it cannot be left as
 * an unimplemented stub reading back zero -- that is neither value, and the
 * firmware wanders off.
 *
 * Both cores are modelled, so the answer depends on who is asking rather than
 * being a constant.
 */
static uint64_t pp502x_proc_id_read(void *opaque, hwaddr offset, unsigned size)
{
    if (offset != 0) {
        return 0;
    }
    return (current_cpu && current_cpu->cpu_index == PP_CORE_COP)
           ? PP_PROC_ID_COP : PP_PROC_ID_CPU;
}

static void pp502x_proc_id_write(void *opaque, hwaddr offset, uint64_t value,
                                 unsigned size)
{
    qemu_log_mask(LOG_GUEST_ERROR,
                  "%s: PROCESSOR_ID is read-only (wrote 0x%" PRIx64
                  " at +0x%" HWADDR_PRIx ")\n", __func__, value, offset);
}

static const MemoryRegionOps pp502x_proc_id_ops = {
    .read = pp502x_proc_id_read,
    .write = pp502x_proc_id_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

/*
 * CPU_CTL and COP_CTL, the two core-control registers.
 *
 * Writing the sleep bit puts that core to sleep; writing without it wakes the
 * core up; reading it back says whether the core is asleep. From Rockbox's
 * system-target.h for PP502x:
 *
 *     sleep_core(core):  mov r0, #0x80000000 ; str r0, [&PROC_CTL(core)]
 *     wake_core(core):   mov r0, #0          ; str r0, [&PROC_CTL(core)]
 *
 * and crt0-pp.S has each core mask its own interrupts, the COP put itself to
 * sleep, and the CPU spin on COP_STATUS until COPSLEEPING appears before it
 * touches the remap unit.
 *
 * This used to answer "the COP is asleep and always will be", which was true
 * of a machine with one core and was enough for Rockbox. It is not enough for
 * Apple's firmware, which runs real initialisation on the COP.
 */
static void pp502x_set_core_halted(IPodVideoMachineState *nms, int core,
                                   bool halt)
{
    CPUState *cs = CPU(nms->cpu[core]);

    if (halt) {
        cs->halted = 1;
        cpu_interrupt(cs, CPU_INTERRUPT_HALT);
    } else {
        cpu_reset_interrupt(cs, CPU_INTERRUPT_HALT);
        cs->halted = 0;
        qemu_cpu_kick(cs);
    }
}

static int pp502x_proc_ctl_core(hwaddr offset)
{
    return offset == PP_PROC_CTL_COP ? PP_CORE_COP : PP_CORE_CPU;
}

static uint64_t pp502x_proc_ctl_read(void *opaque, hwaddr offset, unsigned size)
{
    IPodVideoMachineState *nms = opaque;
    int core;

    if (offset != PP_PROC_CTL_CPU && offset != PP_PROC_CTL_COP) {
        qemu_log_mask(LOG_UNIMP, "%s: unknown register +0x%" HWADDR_PRIx "\n",
                      __func__, offset);
        return 0;
    }

    core = pp502x_proc_ctl_core(offset);
    return nms->proc_ctl[core]
         | (CPU(nms->cpu[core])->halted ? PP_PROC_SLEEPING : 0);
}

static void pp502x_proc_ctl_write(void *opaque, hwaddr offset, uint64_t value,
                                  unsigned size)
{
    IPodVideoMachineState *nms = opaque;
    int core;

    if (offset != PP_PROC_CTL_CPU && offset != PP_PROC_CTL_COP) {
        qemu_log_mask(LOG_UNIMP, "%s: unknown register +0x%" HWADDR_PRIx
                      " (wrote 0x%" PRIx64 ")\n", __func__, offset, value);
        return;
    }

    core = pp502x_proc_ctl_core(offset);
    nms->proc_ctl[core] = value & ~PP_PROC_SLEEPING;
    pp502x_set_core_halted(nms, core, value & PP_PROC_SLEEPING);
}

static const MemoryRegionOps pp502x_proc_ctl_ops = {
    .read = pp502x_proc_ctl_read,
    .write = pp502x_proc_ctl_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

/*
 * The timer block.
 *
 * USEC_TIMER is a free-running microsecond counter that every busy-wait in
 * Rockbox is written against. TIMER1 and TIMER2 are programmable and raise
 * IRQ 0 and IRQ 1; TIMER1 is what drives current_tick, which the cooperative
 * scheduler and every timeout in the firmware depend on. From kernel-pp.c:
 *
 *     TIMER1_CFG = 0xc0000000 | (interval_in_ms*1000 - 1);
 *     TIMER1_VAL;   -- read to acknowledge
 *
 * So bit 31 enables the timer, bit 30 enables its interrupt, the low bits are
 * the period in microseconds minus one, and reading VAL acknowledges.
 */
#define PP_TIMER_CFG_ENABLE     0x80000000
#define PP_TIMER_CFG_IRQ_ENABLE 0x40000000
#define PP_TIMER_CFG_PERIOD     0x0fffffff
#define PP_TIMER1_CFG   0x00
#define PP_TIMER1_VAL   0x04
#define PP_TIMER2_CFG   0x08
#define PP_TIMER2_VAL   0x0c
#define PP_USEC_TIMER   0x10

static void pp502x_timer_rearm(IPodVideoMachineState *nms, int n)
{
    uint32_t cfg = n ? nms->timer2_cfg : nms->timer1_cfg;
    QEMUTimer *t = n ? nms->timer2 : nms->timer1;

    if (!(cfg & PP_TIMER_CFG_ENABLE)) {
        timer_del(t);
        return;
    }
    /* The period is stored one less than it is, and counts microseconds. */
    timer_mod(t, qemu_clock_get_us(QEMU_CLOCK_VIRTUAL) +
                 (cfg & PP_TIMER_CFG_PERIOD) + 1);
}

static void pp502x_timer_expired(IPodVideoMachineState *nms, int n)
{
    uint32_t cfg = n ? nms->timer2_cfg : nms->timer1_cfg;

    if (cfg & PP_TIMER_CFG_IRQ_ENABLE) {
        nms->timer_pending[n] = true;
        qemu_set_irq(qdev_get_gpio_in(nms->intc, n), 1);
    }
    pp502x_timer_rearm(nms, n);
}

static void pp502x_timer1_expired(void *opaque)
{
    pp502x_timer_expired(opaque, 0);
}

static void pp502x_timer2_expired(void *opaque)
{
    pp502x_timer_expired(opaque, 1);
}

static uint64_t pp502x_timer_read(void *opaque, hwaddr offset, unsigned size)
{
    IPodVideoMachineState *nms = opaque;

    switch (offset) {
    case PP_USEC_TIMER:
        /* TIMER_FREQ in pp5020.h is 1000000, so this counts microseconds. */
        return qemu_clock_get_us(QEMU_CLOCK_VIRTUAL);
    case PP_TIMER1_CFG:
        return nms->timer1_cfg;
    case PP_TIMER2_CFG:
        return nms->timer2_cfg;
    case PP_TIMER1_VAL:
    case PP_TIMER2_VAL: {
        /* Reading the value is how the firmware acknowledges the interrupt. */
        int n = (offset == PP_TIMER1_VAL) ? 0 : 1;

        nms->timer_pending[n] = false;
        qemu_set_irq(qdev_get_gpio_in(nms->intc, n), 0);
        return 0;
    }
    default:
        qemu_log_mask(LOG_UNIMP, "%s: unknown register +0x%" HWADDR_PRIx "\n",
                      __func__, offset);
        return 0;
    }
}

static void pp502x_timer_write(void *opaque, hwaddr offset, uint64_t value,
                               unsigned size)
{
    IPodVideoMachineState *nms = opaque;

    switch (offset) {
    case PP_TIMER1_CFG:
        nms->timer1_cfg = value;
        pp502x_timer_rearm(nms, 0);
        break;
    case PP_TIMER2_CFG:
        nms->timer2_cfg = value;
        pp502x_timer_rearm(nms, 1);
        break;
    case PP_USEC_TIMER:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: USEC_TIMER is read-only\n",
                      __func__);
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: unknown register +0x%" HWADDR_PRIx
                      " (wrote 0x%" PRIx64 ")\n", __func__, offset, value);
        break;
    }
}

static const MemoryRegionOps pp502x_timer_ops = {
    .read = pp502x_timer_read,
    .write = pp502x_timer_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

/*
 * The cache controller, which is also where the vector table lives or does
 * not. From pp5020.h:
 *
 *     #define CACHE_CTL_ENABLE     0x0001
 *     #define CACHE_CTL_RUN        0x0002
 *     #define CACHE_CTL_INIT       0x0004
 *     #define CACHE_CTL_VECT_REMAP 0x0010
 *     #define CACHE_CTL_READY      0x4000
 *
 * VECT_REMAP is the one that matters. The ARM takes its exceptions from
 * address 0, and on this SoC that address is the flash ROM until the firmware
 * asks for the vectors to be remapped into IRAM, where it has installed its
 * own. Leave the bit unimplemented and every exception vectors into blank
 * space -- which is exactly what was happening: the CPU walked forward through
 * a window of zeroes, one NOP at a time, until it fell out of the map.
 *
 * The cache itself is not modelled. There is nothing to make coherent here,
 * so enabling it only has to be acknowledged.
 */
#define PP_CACHE_CTL            0x00
#define PP_CACHE_CTL_VECT_REMAP 0x0010
#define PP_CACHE_CTL_READY      0x4000

static uint64_t pp502x_cache_ctl_read(void *opaque, hwaddr offset, unsigned size)
{
    IPodVideoMachineState *nms = opaque;

    if (offset == PP_CACHE_CTL) {
        /* Always ready, never busy: there is no cache to wait for. */
        return nms->cache_ctl | PP_CACHE_CTL_READY;
    }
    return 0;
}

static void pp502x_cache_ctl_write(void *opaque, hwaddr offset, uint64_t value,
                                   unsigned size)
{
    IPodVideoMachineState *nms = opaque;

    if (offset != PP_CACHE_CTL) {
        return;
    }
    nms->cache_ctl = value;
    memory_region_set_enabled(nms->vect_remap,
                              !!(value & PP_CACHE_CTL_VECT_REMAP));
}

static const MemoryRegionOps pp502x_cache_ctl_ops = {
    .read = pp502x_cache_ctl_read,
    .write = pp502x_cache_ctl_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

/*
 * The remap unit, MMAP0..7 at 0xf000f000.
 *
 * This is how low addresses stop being the flash ROM and become real memory.
 * Apple's firmware is linked to run from 0: tracing the boot shows the Rockbox
 * bootloader hand control to it at DRAM_START, its reset path run through
 * 0x100001f0..0x10000234, and then jump straight to 0x23c -- an absolute low
 * address that only makes sense once DRAM is mapped there. Without the remap
 * that jump lands in the blank flash window and the CPU NOP-sleds upward until
 * it falls out of the map.
 *
 * Each window is a pair of registers. The top byte of each is a base address:
 * the logical one in MMAPn_LOGICAL, the physical one in MMAPn_PHYSICAL. The
 * low half of the logical register is a mask saying which address bits have to
 * match, and the low half of the physical register carries permission flags --
 * Rockbox names four of them, MMAP_PHYS_READ/WRITE/DATA/CODE_MASK.
 *
 * The mask field is decoded here only as far as reading it: mask bit k covers
 * address bit k+16, which follows from Rockbox's crt0-pp.S using 0x3e00 for a
 * 32 MB window and 0x3c00 for a 64 MB one -- dropping bit 9 doubles the window,
 * so bit 9 is address bit 25. It is not enforced. What is decoded is the pair
 * of base bytes, which is enough to tell which window puts DRAM at zero.
 *
 * That distinction matters, because it used to be got right by accident. The
 * old rule was "any write to MMAP0 switches DRAM in", and the two firmwares
 * disagree about which window does the job:
 *
 *   Rockbox  MMAP0  logical 0x00003e00  physical 0x10000f84   <- this one
 *   Apple    MMAP0  logical 0x00003bf0  physical 0x00003a88
 *            MMAP1  logical 0x00003a00  physical 0x10000f84   <- this one
 *            MMAP2  logical 0x20003800  physical 0x00003f88   flash at 0x20000000
 *
 * Apple's MMAP0 maps low addresses to physical zero, which is not the remap at
 * all; the window that matters is MMAP1, and it only worked because the write
 * to MMAP0 tripped the old rule first. Both firmwares write the logical half
 * before the physical one, but the pair is re-examined on either write so the
 * order does not matter.
 *
 * Still coarse: a matching pair switches the whole of DRAM in at zero rather
 * than the window the mask describes, nothing is ever switched back out, and
 * the permission flags are ignored. A guest asking for anything other than the
 * usual whole-of-DRAM-at-zero mapping will not get what it asked for.
 */
#define PP_MMAP_BASE_MASK   0xff000000

static uint64_t pp502x_mmap_read(void *opaque, hwaddr offset, unsigned size)
{
    IPodVideoMachineState *nms = opaque;

    return offset < sizeof(nms->mmap_regs) ? nms->mmap_regs[offset / 4] : 0;
}

static void pp502x_mmap_write(void *opaque, hwaddr offset, uint64_t value,
                              unsigned size)
{
    IPodVideoMachineState *nms = opaque;
    unsigned pair;

    if (offset >= sizeof(nms->mmap_regs)) {
        return;
    }
    nms->mmap_regs[offset / 4] = value;

    if (nms->dram_remapped) {
        return;
    }

    /* Each window is two consecutive words, logical first. */
    pair = (offset / 4) & ~1u;
    if ((nms->mmap_regs[pair] & PP_MMAP_BASE_MASK) == 0 &&
        (nms->mmap_regs[pair + 1] & PP_MMAP_BASE_MASK) == DRAM_MEM_BASE) {
        nms->dram_remapped = true;
        memory_region_set_enabled(nms->dram_remap, true);
        /* Bit 26 is not decoded, so the twin window moves with it. */
        memory_region_set_enabled(nms->dram_remap_alias, true);
    }
}

static const MemoryRegionOps pp502x_mmap_ops = {
    .read = pp502x_mmap_read,
    .write = pp502x_mmap_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

/*
 * GPIO, enough of it to answer the buttons.
 *
 * The registers go four ports to a group: ENABLE at +0x00, OUTPUT_EN at +0x10,
 * OUTPUT_VAL at +0x20 and INPUT_VAL at +0x30. Only the input values matter
 * here, and they are active low -- from Rockbox's button-clickwheel.c:
 *
 *     bool button_hold(void) { return (GPIOA_INPUT_VAL & 0x20) ? false : true; }
 *
 * Reading back zero therefore means every button is held, including hold,
 * which is why the Rockbox bootloader was taking its "boot Apple's firmware"
 * path instead of loading rockbox.ipod. Inputs read as all ones: nothing
 * pressed.
 */
#define PP_GPIO_INPUT_VAL   0x30
#define PP_GPIO_GROUP_SIZE  0x40

static uint64_t pp502x_gpio_read(void *opaque, hwaddr offset, unsigned size)
{
    IPodVideoMachineState *nms = opaque;
    hwaddr reg = offset % PP_GPIO_GROUP_SIZE;

    if (reg >= PP_GPIO_INPUT_VAL && reg < PP_GPIO_INPUT_VAL + 0x10) {
        return 0xffffffff;
    }
    return offset < sizeof(nms->gpio_regs) ? nms->gpio_regs[offset / 4] : 0;
}

static void pp502x_gpio_write(void *opaque, hwaddr offset, uint64_t value,
                              unsigned size)
{
    IPodVideoMachineState *nms = opaque;

    if (offset < sizeof(nms->gpio_regs)) {
        nms->gpio_regs[offset / 4] = value;
    }
}

static const MemoryRegionOps pp502x_gpio_ops = {
    .read = pp502x_gpio_read,
    .write = pp502x_gpio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

/*
 * The click wheel.
 *
 * Rockbox decodes it in button-clickwheel.c, which pins the format down
 * exactly:
 *
 *     unsigned status = CLICKWHEEL_DATA;
 *     if ((status & 0x800000ff) == 0x8000001a) {
 *         if (status & 0x00000100) btn |= BUTTON_SELECT;
 *         if (status & 0x00000200) btn |= BUTTON_RIGHT;
 *         if (status & 0x00000400) btn |= BUTTON_LEFT;
 *         if (status & 0x00000800) btn |= BUTTON_PLAY;
 *         if (status & 0x00001000) btn |= BUTTON_MENU;
 *
 * So a valid packet is 0x8000001a and the buttons are single bits on top of
 * it. Reading back zero is not "no buttons", it is "no packet at all", which
 * is not the same thing and is part of why the firmware would not proceed.
 *
 * The wheel itself -- bit 30 with a position in bits 16..22 -- is not
 * generated yet; only the five buttons are, from the keyboard.
 *
 * On the PP the wheel is interrupt driven, not polled: ipod_4g_button_read()
 * is only ever called from ipod_4g_button_int(), and it refuses to look at the
 * data at all unless a status bit is set first:
 *
 *     if ((inl(0x7000c104) & 0x04000000) != 0) {
 *         unsigned status = CLICKWHEEL_DATA;
 *         ...
 *     }
 *
 * and the handler acknowledges with outl(inl(0x7000c104) | 0x0c000000, ...).
 * So a key press has to do two things: latch that bit, and raise the wheel's
 * interrupt, which on this SoC is the I2C line in the high bank. Without both
 * the firmware never reads the wheel and nothing at all happens.
 */
#define PP_CLICKWHEEL_PACKET    0x8000001a
#define PP_WHEEL_STATUS_OFF     0x104
#define PP_WHEEL_STATUS_RX      (1 << 26)
#define PP_WHEEL_TOUCHED        0x40000000
#define PP_WHEEL_POS_SHIFT      16
#define PP_WHEEL_POS_MASK       0x7f
/*
 * "Highest wheel = 0x5F, clockwise increases" -- 96 positions to the turn,
 * which is WHEELCLICKS_PER_ROTATION. Rockbox needs WHEEL_SENSITIVITY (4)
 * clicks of accumulated travel before it moves the selection by one, so each
 * key event steps two and a press/release pair moves exactly one item.
 */
#define PP_WHEEL_CLICKS         96
#define PP_WHEEL_STEP           2
#define PP_WHEEL_SELECT         0x00000100
#define PP_WHEEL_RIGHT          0x00000200
#define PP_WHEEL_LEFT           0x00000400
#define PP_WHEEL_PLAY           0x00000800
#define PP_WHEEL_MENU           0x00001000

static uint64_t pp502x_clickwheel_read(void *opaque, hwaddr offset,
                                       unsigned size)
{
    IPodVideoMachineState *nms = opaque;

    if (offset == CLICKWHEEL_DATA_OFF) {
        uint32_t packet = PP_CLICKWHEEL_PACKET | nms->wheel_buttons;
        if (nms->wheel_touched) {
            packet |= PP_WHEEL_TOUCHED
                   | ((nms->wheel_position & PP_WHEEL_POS_MASK)
                      << PP_WHEEL_POS_SHIFT);
        }
        return packet;
    }
    if (offset == PP_WHEEL_STATUS_OFF) {
        return nms->wheel_regs[offset / 4]
             | (nms->wheel_pending ? PP_WHEEL_STATUS_RX : 0);
    }
    return offset < sizeof(nms->wheel_regs) ? nms->wheel_regs[offset / 4] : 0;
}

static void pp502x_clickwheel_write(void *opaque, hwaddr offset,
                                    uint64_t value, unsigned size)
{
    IPodVideoMachineState *nms = opaque;

    if (offset == PP_WHEEL_STATUS_OFF && (value & PP_WHEEL_STATUS_RX)) {
        /* The handler acknowledges the packet by writing the bit back. */
        nms->wheel_pending = false;
        qemu_set_irq(qdev_get_gpio_in(nms->intc, PP_I2C_IRQ), 0);
        value &= ~PP_WHEEL_STATUS_RX;
    }

    if (offset < sizeof(nms->wheel_regs)) {
        nms->wheel_regs[offset / 4] = value;
    }
}

static const MemoryRegionOps pp502x_clickwheel_ops = {
    .read = pp502x_clickwheel_read,
    .write = pp502x_clickwheel_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

/* w/a/s/d around the wheel, enter for select. */
static void ipod_video_key_event(void *opaque, int keycode)
{
    IPodVideoMachineState *nms = opaque;
    bool released = keycode & 0x80;
    uint32_t bit;

    switch (keycode & 0x7f) {
    case 0x11: bit = PP_WHEEL_MENU;   break;   /* w */
    case 0x1e: bit = PP_WHEEL_LEFT;   break;   /* a */
    case 0x1f: bit = PP_WHEEL_PLAY;   break;   /* s */
    case 0x20: bit = PP_WHEEL_RIGHT;  break;   /* d */
    case 0x1c: bit = PP_WHEEL_SELECT; break;   /* enter */

    /*
     * Turning the wheel. Both the press and the release step it, so one
     * keystroke is four clicks: enough for Rockbox to call it a scroll, and
     * frequent enough that its 150 ms untouch timeout never fires and resets
     * the accumulator.
     */
    case 0x12:                                 /* e: clockwise */
    case 0x10:                                 /* q: anticlockwise */
        nms->wheel_position += (keycode & 0x7f) == 0x12
                             ? PP_WHEEL_STEP
                             : PP_WHEEL_CLICKS - PP_WHEEL_STEP;
        nms->wheel_position %= PP_WHEEL_CLICKS;
        nms->wheel_touched = true;
        nms->wheel_pending = true;
        qemu_set_irq(qdev_get_gpio_in(nms->intc, PP_I2C_IRQ), 1);
        return;

    default: return;
    }

    if (released) {
        nms->wheel_buttons &= ~bit;
    } else {
        nms->wheel_buttons |= bit;
    }

    /* A change is a new packet: latch the status bit and interrupt. */
    nms->wheel_pending = true;
    qemu_set_irq(qdev_get_gpio_in(nms->intc, PP_I2C_IRQ), 1);
}

/*
 * Clock and power control.
 *
 * Only PLL_STATUS has behaviour, and it is the one the firmware cannot get
 * past. From system-pp502x.c, raising the clock on a PP5022:
 *
 *     PLL_CONTROL = 0x8a121403;
 *     while (!(PLL_STATUS & 0x80000000));   -- wait for relock
 *
 * Bit 31 says the PLL has locked. There is no PLL here to take time settling,
 * so it is locked as soon as it is asked for. Everything else in the block is
 * stored and handed back.
 */
#define PP_PLL_STATUS       0x3c
#define PP_PLL_LOCKED       0x80000000

static uint64_t pp502x_clock_read(void *opaque, hwaddr offset, unsigned size)
{
    IPodVideoMachineState *nms = opaque;

    if (offset == PP_PLL_STATUS) {
        return nms->clock_regs[offset / 4] | PP_PLL_LOCKED;
    }
    return offset < sizeof(nms->clock_regs) ? nms->clock_regs[offset / 4] : 0;
}

static void pp502x_clock_write(void *opaque, hwaddr offset, uint64_t value,
                               unsigned size)
{
    IPodVideoMachineState *nms = opaque;

    if (offset < sizeof(nms->clock_regs)) {
        nms->clock_regs[offset / 4] = value;
    }
}

static const MemoryRegionOps pp502x_clock_ops = {
    .read = pp502x_clock_read,
    .write = pp502x_clock_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

/*
 * Device control: chip version and straps, DEV_INIT1/2, the external memory
 * bus timings and the 32-bit GPO port. Rockbox's pp5020.h names most of it --
 * PP_VER1 at +0x00, DEV_INIT1 at +0x10, DEV_INIT2 at +0x20, DEV_TIMING1 at
 * +0x34, GPO32_VAL at +0x80, GPO32_ENABLE at +0x84 -- and everything named is
 * a plain register that reads back what was written.
 *
 * The exception is +0x28, which pp5020.h does not name at all but which the
 * USB bring-up in usb-fw-pp502x.c blocks on:
 *
 *     DEV_INIT2 |= INIT_USB;
 *     while ((inl(0x70000028) & 0x80) == 0);
 *     outl(inl(0x70000028) | 0x2, 0x70000028);
 *
 * Bit 7 is the USB block reporting that it came out of reset. Nothing here
 * takes time to power up, so it reads as set from the start; leaving it clear
 * is an unbreakable spin, and it was where the firmware sat once the cache
 * arrays stopped faulting.
 */
#define PP_DEV_USB_STATUS   0x28
#define PP_DEV_USB_READY    0x80
#define PP_GPO32_VAL        0x80
#define PP_GPO32_BCM_POWER  0x4000

/*
 * PP_VER1 and PP_VER2 spell out which chip this is, as eight ASCII characters.
 * Rockbox's debug-pp.c shows the order: the four bytes of PP_VER2 from most
 * significant down, then the four of PP_VER1.
 *
 *     char pp_version[] = { (PP_VER2 >> 24) & 0xff, ... (PP_VER1) & 0xff };
 *
 * The device on the desk reports PP5022C. Apple's firmware cares about exactly
 * one of those characters -- the sixth, the second digit of the part number:
 *
 *     598: ldr r0, [0x70000000]   ; PP_VER1
 *     5a4: lsr r0, r0, #24        ; after lsl #8: bits 23:16, i.e. char 5
 *     5a8: cmp r0, #0x32          ; '2' -> PP5022
 *
 * and picks a different base address in IRAM depending on the answer. Reading
 * back zero is not a harmless unknown here: it is a third answer the firmware
 * has no case for, so it falls through to a base meant for another chip.
 */
#define PP_VER1_OFF         0x00
#define PP_VER2_OFF         0x04
/*
 * Where the globals live, and by whom. This was attributed to the wrong
 * routine when the fix first went in.
 *
 * 0x27ea8c does compare these eight bytes against "PP5020A " and "PP5022A "
 * -- the literals at 0x27ead4 and 0x27eae0 -- and returns 7, 8 or 0, and its
 * caller at 0xf0c does turn that into a base of 0x40018000 or 0x40007ffc. But
 * that path is dead in this build: 0xf0c starts by loading the literal at
 * 0xf34, which is 0x4001c8fc, finds it non-zero and returns through the popne
 * at 0xf1c without ever making the comparison.
 *
 * The routine that actually picks the base is 0x1084, and it does not look at
 * the string at all -- it calls 0x598, which reads one byte:
 *
 *     598: mov r0, #0x70000000
 *     59c: ldr r0, [r0]          ; PP_VER1
 *     5a0: lsl r0, r0, #8
 *     5a4: lsr r0, r0, #24       ; bits 23:16, the sixth character
 *     5a8: cmp r0, #0x32         ; '2' -> PP5022
 *
 * and then returns 0x40020000 + N - 0x100 for a PP5022 or 0x40018000 + N -
 * 0x100 otherwise. So the correction to these registers is still needed and
 * still for the same reason -- reading back zero is a third answer the
 * firmware has no case for -- but it is that single character that decides it,
 * not the whole string.
 *
 * Rockbox's debug screen renders this device as PP5022C, which is where the
 * earlier value came from. The firmware wants the 'A' form, and the real
 * device boots it, so that is what the registers have to say.
 */
#define PP5022C_VER1        0x32324120   /* '2' '2' 'A' ' ' */
#define PP5022C_VER2        0x50503530   /* 'P' 'P' '5' '0'  */

static uint64_t pp502x_dev_ctl_read(void *opaque, hwaddr offset, unsigned size)
{
    IPodVideoMachineState *nms = opaque;

    if (offset >= sizeof(nms->dev_regs)) {
        return 0;
    }
    if (offset == PP_DEV_USB_STATUS) {
        return nms->dev_regs[offset / 4] | PP_DEV_USB_READY;
    }
    return nms->dev_regs[offset / 4];
}

static void pp502x_dev_ctl_write(void *opaque, hwaddr offset, uint64_t value,
                                 unsigned size)
{
    IPodVideoMachineState *nms = opaque;

    if (offset < sizeof(nms->dev_regs)) {
        nms->dev_regs[offset / 4] = value;
    }
}

static const MemoryRegionOps pp502x_dev_ctl_ops = {
    .read = pp502x_dev_ctl_read,
    .write = pp502x_dev_ctl_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static MemoryRegion *allocate_ram(MemoryRegion *top, const char *name,
                                  uint32_t addr, uint32_t size)
{
    MemoryRegion *sec = g_new(MemoryRegion, 1);
    memory_region_init_ram(sec, NULL, name, size, &error_fatal);
    memory_region_add_subregion(top, addr, sec);
    return sec;
}

static void ipod_video_cpu_reset(void *opaque)
{
    IPodVideoMachineState *nms = IPOD_VIDEO_MACHINE((MachineState *)opaque);

    /* Both cores come out of reset at the same vector, as on the hardware. */
    for (int i = 0; i < PP_NUM_CORES; i++) {
        CPUState *cs = CPU(nms->cpu[i]);

        cpu_reset(cs);
        cpu_set_pc(cs, nms->reset_pc);
    }
}

/*
 * The ROM loader reads the osos image off the firmware partition, copies it
 * whole to DRAM_START and jumps to DRAM_START + entryOffset. We do its job
 * directly: there is no point emulating the NOR ROM when all it does is copy
 * and jump, and we do not have a dump of it anyway.
 *
 * An ELF is honoured as an ELF, which is how a freshly built Rockbox
 * bootloader can be loaded without going through an osos image at all.
 */
static void ipod_video_load_image(MachineState *machine,
                                  IPodVideoMachineState *nms)
{
    uint64_t entry = 0, lowaddr = 0, highaddr = 0;
    ssize_t size;

    nms->reset_pc = DRAM_MEM_BASE + nms->entry_offset;

    if (!machine->kernel_filename) {
        return;
    }

    size = load_elf(machine->kernel_filename, NULL, NULL, NULL, &entry,
                    &lowaddr, &highaddr, NULL, 0, EM_ARM, 1, 0);
    if (size > 0) {
        info_report("loaded ELF \"%s\": 0x%" PRIx64 "-0x%" PRIx64
                    ", entry 0x%" PRIx64, machine->kernel_filename,
                    lowaddr, highaddr, entry);
        nms->reset_pc = entry;
        return;
    }

    size = load_image_targphys(machine->kernel_filename, DRAM_MEM_BASE,
                               DRAM_MEM_SIZE);
    if (size < 0) {
        error_report("could not load \"%s\" at 0x%x", machine->kernel_filename,
                     (unsigned)DRAM_MEM_BASE);
        exit(1);
    }
    info_report("loaded 0x%zx bytes at 0x%x, entry 0x%" HWADDR_PRIx,
                (size_t)size, (unsigned)DRAM_MEM_BASE, nms->reset_pc);
}

static void ipod_video_get_entry(Object *obj, Visitor *v, const char *name,
                                 void *opaque, Error **errp)
{
    IPodVideoMachineState *nms = IPOD_VIDEO_MACHINE(obj);
    uint32_t value = nms->entry_offset;

    visit_type_uint32(v, name, &value, errp);
}

static void ipod_video_set_entry(Object *obj, Visitor *v, const char *name,
                                 void *opaque, Error **errp)
{
    IPodVideoMachineState *nms = IPOD_VIDEO_MACHINE(obj);
    uint32_t value;

    if (!visit_type_uint32(v, name, &value, errp)) {
        return;
    }
    if (value >= DRAM_MEM_SIZE) {
        error_setg(errp, "entry offset 0x%x is past the end of DRAM", value);
        return;
    }
    nms->entry_offset = value;
}

static bool ipod_video_get_semihosting(Object *obj, Error **errp)
{
    return IPOD_VIDEO_MACHINE(obj)->semihosting;
}

static void ipod_video_set_semihosting(Object *obj, bool value, Error **errp)
{
    IPOD_VIDEO_MACHINE(obj)->semihosting = value;
}

static bool ipod_video_get_remap(Object *obj, Error **errp)
{
    return IPOD_VIDEO_MACHINE(obj)->force_remap;
}

static void ipod_video_set_remap(Object *obj, bool value, Error **errp)
{
    IPOD_VIDEO_MACHINE(obj)->force_remap = value;
}

static void ipod_video_instance_init(Object *obj)
{
    object_property_add_bool(obj, "remap", ipod_video_get_remap,
                             ipod_video_set_remap);
    object_property_set_description(obj, "remap",
        "Map DRAM at address 0 from reset, as the firmware would through the "
        "remap unit. Needed to run a Rockbox application image directly, since "
        "those are linked for address 0 and normally rely on the bootloader "
        "having set the mapping up first.");

    IPOD_VIDEO_MACHINE(obj)->hw_revision = IPOD_VIDEO_HW_REVISION;

    /*
     * Apple's firmware is built against ARM's own C library and keeps its
     * semihosting-based panic printer in the shipped image. 0xe9b98 loads
     * r0 = 4 (SYS_WRITE0) and executes "svc 0x123456"; the string table just
     * above it holds "Type: ", "undefined instruction", "data abort",
     * "_sys_exit", "CPU ", "Task", which is what an ARM C library prints when
     * it dies. With semihosting off that SVC is a plain supervisor call into a
     * vector table the firmware has not installed yet, and the machine dies
     * silently; with it on the firmware tells us what went wrong and dumps its
     * registers. Costs nothing when nothing panics, so it is on by default.
     */
    IPOD_VIDEO_MACHINE(obj)->semihosting = true;
    object_property_add_bool(obj, "semihosting", ipod_video_get_semihosting,
                             ipod_video_set_semihosting);
    object_property_set_description(obj, "semihosting",
        "Answer ARM semihosting calls (SVC 0x123456). Apple's retailOS prints "
        "its panic messages through them, so this turns a black screen into a "
        "diagnosis. Turn it off to see the firmware take the SVC vector "
        "instead.");

    object_property_add(obj, "entry", "uint32", ipod_video_get_entry,
                        ipod_video_set_entry, NULL, NULL);
    object_property_set_description(obj, "entry",
        "Offset into the loaded image to start executing at, as the ROM "
        "loader would. An osos image with a Rockbox bootloader appended needs "
        "the entryOffset from the firmware partition directory; 0 runs "
        "Apple's firmware instead.");
}

/*
 * Blocks we know the address of but have not modelled. Registering them keeps
 * stray accesses out of the unassigned-memory path and makes -d unimp name the
 * block the firmware is talking to.
 */
static const struct { const char *name; hwaddr base; hwaddr size; }
ipod_video_unimplemented[] = {
    { "pp502x.usb",       USB_MEM_BASE,       0x1000 },
};

static void ipod_video_machine_init(MachineState *machine)
{
    IPodVideoMachineState *nms = IPOD_VIDEO_MACHINE(machine);
    MemoryRegion *sysmem = get_system_memory();
    Object *cpuobj;

    if (nms->semihosting) {
        qemu_semihosting_enable();
    }

    for (int i = 0; i < PP_NUM_CORES; i++) {
        cpuobj = object_new(machine->cpu_type);
        nms->cpu[i] = ARM_CPU(cpuobj);
        object_property_set_link(cpuobj, "memory", OBJECT(sysmem),
                                 &error_abort);
        object_property_set_bool(cpuobj, "realized", true, &error_fatal);
        object_unref(cpuobj);
    }
    nms->nsas = cpu_get_address_space(CPU(nms->cpu[PP_CORE_CPU]),
                                      ARMASIdx_NS);

    MemoryRegion *dram = allocate_ram(sysmem, "dram", DRAM_MEM_BASE,
                                      DRAM_MEM_SIZE);
    MemoryRegion *iram = allocate_ram(sysmem, "iram", IRAM_MEM_BASE,
                                      IRAM_MEM_SIZE);

    /*
     * The PP's NOR flash sits at address 0 while the bootloader runs and is
     * remapped to 0x20000000 afterwards -- Rockbox's hwcompat.h says as much,
     * reading the hardware revision from 0x405c during boot and from
     * 0x2000405c later, with the comment "ROM is remapped".
     *
     * We have no dump of that flash, so this stands in for it: a blank window
     * carrying the one word the firmware actually asks for here. Without it
     * the read faults, the CPU vectors to the prefetch abort handler at 0xc,
     * finds nothing mapped there either, and spins on that forever.
     */
    MemoryRegion *rom = g_new(MemoryRegion, 1);
    memory_region_init_ram(rom, NULL, "flash-rom", FLASH_ROM_MEM_SIZE,
                           &error_fatal);
    memory_region_add_subregion(sysmem, FLASH_ROM_MEM_BASE, rom);

    /*
     * IRAM over the top of the flash window, for when the firmware asks for
     * the vectors to be remapped. Disabled until it does.
     */
    nms->vect_remap = g_new(MemoryRegion, 1);
    memory_region_init_alias(nms->vect_remap, NULL, "vector-remap",
                             iram, 0, IRAM_MEM_SIZE);
    memory_region_add_subregion_overlap(sysmem, 0, nms->vect_remap, 1);
    memory_region_set_enabled(nms->vect_remap, false);

    /* DRAM at address 0, switched in when the firmware programs MMAP0. */
    nms->dram_remap = g_new(MemoryRegion, 1);
    memory_region_init_alias(nms->dram_remap, NULL, "dram-remap", dram, 0,
                             DRAM_MEM_SIZE);
    memory_region_add_subregion_overlap(sysmem, 0, nms->dram_remap, 2);
    memory_region_set_enabled(nms->dram_remap, nms->force_remap);
    nms->dram_remapped = nms->force_remap;

    /*
     * The bit-26 twins of the two DRAM windows -- see DRAM_ALIAS_BIT.
     *
     * Apple's firmware reads its own image through 0x0471619c and writes its
     * RW data through 0x1493690c, both of which are ordinary DRAM addresses
     * with bit 26 set. Rockbox never forms one, which is why a machine that
     * runs Rockbox to its menu still could not get retailOS past its first
     * scatter-load: 0x84390 (ldrcc r3, [r0], #4 inside __scatterload_copy)
     * took a data abort on its very first read.
     *
     * The high twin is permanent because DRAM at DRAM_MEM_BASE is permanent.
     * The low one follows dram_remap, because what lives at address 0 -- and
     * therefore at 0x04000000 -- depends on whether the remap has happened.
     * Before it does, the flash window at 0 has a twin too, at a lower
     * priority so the DRAM alias covers it once enabled.
     */
    MemoryRegion *dram_alias = g_new(MemoryRegion, 1);
    memory_region_init_alias(dram_alias, NULL, "dram-bit26", dram, 0,
                             DRAM_MEM_SIZE);
    memory_region_add_subregion(sysmem, DRAM_MEM_BASE | DRAM_ALIAS_BIT,
                                dram_alias);

    MemoryRegion *rom_alias_lo = g_new(MemoryRegion, 1);
    memory_region_init_alias(rom_alias_lo, NULL, "flash-rom-bit26", rom, 0,
                             FLASH_ROM_MEM_SIZE);
    memory_region_add_subregion_overlap(sysmem, DRAM_ALIAS_BIT,
                                        rom_alias_lo, 1);

    nms->dram_remap_alias = g_new(MemoryRegion, 1);
    memory_region_init_alias(nms->dram_remap_alias, NULL, "dram-remap-bit26",
                             dram, 0, DRAM_MEM_SIZE);
    memory_region_add_subregion_overlap(sysmem, DRAM_ALIAS_BIT,
                                        nms->dram_remap_alias, 2);
    memory_region_set_enabled(nms->dram_remap_alias, nms->force_remap);

    MemoryRegion *rom_alias = g_new(MemoryRegion, 1);
    memory_region_init_alias(rom_alias, NULL, "flash-rom-remapped", rom, 0,
                             FLASH_ROM_MEM_SIZE);
    memory_region_add_subregion(sysmem, FLASH_ROM_REMAP_BASE, rom_alias);
    /*
     * The uncached views of both of those -- see DRAM_UNCACHED_BIT. Apple's
     * allocator hands out every DRAM buffer with bit 31 set, and since its
     * pointers come in both flavours (0x10000000 and its bit-26 twin), so do
     * the uncached ones.
     *
     * The window at address 0 gets no such twin, deliberately. The tag is put
     * on by the buffer allocator, whose heap is up in DRAM proper: every
     * tagged pointer seen so far is 0x9xxxxxxx. If one ever turns up as
     * 0x80xxxxxx the rule is the same and the fix is two more lines, but until
     * then it would be a window nothing has asked for.
     */
    MemoryRegion *dram_uncached = g_new(MemoryRegion, 1);
    memory_region_init_alias(dram_uncached, NULL, "dram-uncached", dram, 0,
                             DRAM_MEM_SIZE);
    memory_region_add_subregion(sysmem, DRAM_MEM_BASE | DRAM_UNCACHED_BIT,
                                dram_uncached);

    MemoryRegion *dram_uncached26 = g_new(MemoryRegion, 1);
    memory_region_init_alias(dram_uncached26, NULL, "dram-uncached-bit26",
                             dram, 0, DRAM_MEM_SIZE);
    memory_region_add_subregion(sysmem,
                                DRAM_MEM_BASE | DRAM_ALIAS_BIT
                                              | DRAM_UNCACHED_BIT,
                                dram_uncached26);


    uint32_t hwrev = cpu_to_le32(nms->hw_revision);
    cpu_physical_memory_write(FLASH_ROM_MEM_BASE + IPOD_HW_REVISION_OFF,
                              &hwrev, sizeof(hwrev));

    MemoryRegion *proc_id = g_new(MemoryRegion, 1);
    memory_region_init_io(proc_id, NULL, &pp502x_proc_id_ops, nms,
                          "pp502x.proc-id", 0x1000);
    memory_region_add_subregion_overlap(sysmem, PROC_ID_MEM_BASE, proc_id, 1);

    MemoryRegion *proc_ctl = g_new(MemoryRegion, 1);
    memory_region_init_io(proc_ctl, NULL, &pp502x_proc_ctl_ops, nms,
                          "pp502x.proc-ctl", 0x1000);
    memory_region_add_subregion_overlap(sysmem, PROC_CTL_MEM_BASE, proc_ctl, 1);

    /*
     * The interrupt controller, which everything below hangs off: without a
     * tick from TIMER1 the firmware's scheduler never runs and no timeout ever
     * expires.
     */
    nms->intc = qdev_new(TYPE_IPOD_VIDEO_INTC);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(nms->intc), &error_fatal);
    sysbus_mmio_map_overlap(SYS_BUS_DEVICE(nms->intc), 0, INTC_MEM_BASE, 1);
    sysbus_connect_irq(SYS_BUS_DEVICE(nms->intc), 0,
                       qdev_get_gpio_in(DEVICE(nms->cpu[PP_CORE_CPU]),
                                        ARM_CPU_IRQ));
    sysbus_connect_irq(SYS_BUS_DEVICE(nms->intc), 1,
                       qdev_get_gpio_in(DEVICE(nms->cpu[PP_CORE_COP]),
                                        ARM_CPU_IRQ));

    /*
     * The cache arrays. These are not registers, they are the cache's own
     * storage exposed on the bus, and PP5022 firmware writes them directly:
     * cache_invalidate_special() in system-pp502x.c walks the whole per-core
     * line-status array marking every line valid, as a workaround for lines
     * that would otherwise corrupt memory.
     *
     *     for (p = &CACHE_STATUS_BASE_CPU;
     *          p < (&CACHE_STATUS_BASE_CPU) + CACHE_SIZE/sizeof(*p);
     *          p += CACHEALIGN_SIZE/sizeof(*p))
     *         *p = CACHE_LINE_VALID | CACHE_ADDRESS_MASK;
     *
     * With nothing mapped there that first store is rejected outright. There
     * is no cache to model, so plain RAM is enough: the firmware only ever
     * reads back what it wrote.
     */
    allocate_ram(sysmem, "cache-arrays", CACHE_ARRAY_MEM_BASE,
                 CACHE_ARRAY_MEM_SIZE);

    MemoryRegion *mmap = g_new(MemoryRegion, 1);
    /*
     * The header documents MMAP0..7 as 0xf000f000..0xf000f03c, but the
     * firmware writes 0xf000f040 as well, so the block is bigger than the
     * eight register pairs Rockbox names. Cover a page.
     */
    memory_region_init_io(mmap, NULL, &pp502x_mmap_ops, nms,
                          "pp502x.mmap", 0x1000);
    memory_region_add_subregion(sysmem, MMAP_MEM_BASE, mmap);

    MemoryRegion *gpio = g_new(MemoryRegion, 1);
    memory_region_init_io(gpio, NULL, &pp502x_gpio_ops, nms,
                          "pp502x.gpio", 0x1000);
    memory_region_add_subregion_overlap(sysmem, GPIO_MEM_BASE, gpio, 1);

    MemoryRegion *wheel = g_new(MemoryRegion, 1);
    memory_region_init_io(wheel, NULL, &pp502x_clickwheel_ops, nms,
                          "pp502x.clickwheel", 0x1000);
    memory_region_add_subregion_overlap(sysmem, CLICKWHEEL_MEM_BASE, wheel, 1);
    qemu_add_kbd_event_handler(ipod_video_key_event, nms);

    MemoryRegion *clock = g_new(MemoryRegion, 1);
    memory_region_init_io(clock, NULL, &pp502x_clock_ops, nms,
                          "pp502x.clock", 0x1000);
    memory_region_add_subregion_overlap(sysmem, CLOCK_MEM_BASE, clock, 1);

    MemoryRegion *dev_ctl = g_new(MemoryRegion, 1);
    memory_region_init_io(dev_ctl, NULL, &pp502x_dev_ctl_ops, nms,
                          "pp502x.dev-ctl", 0x1000);
    memory_region_add_subregion_overlap(sysmem, DEV_CTL_MEM_BASE, dev_ctl, 1);

    /*
     * GPO32 bit 14 powers the Broadcom. Apple's flash ROM loader turns it on
     * before it hands over, and Rockbox reads it back to decide whether it has
     * to bring the chip up itself -- from lcd-video.c:
     *
     *     if (GPO32_VAL & 0x4000) {
     *         lcd_state.display_on = true;      -- BCM is powered
     *     } else {
     *         lcd_state.display_on = false;     -- must upload BCM firmware
     *         lcd_awake();
     *     }
     *
     * That firmware is the 'vmcs' section of the flash ROM, which we do not
     * have, so the second path leaves display_on false forever -- and
     * lcd_update_rect() returns early when it is, so every draw is dropped
     * without a word. The screen stayed black while the rest of Rockbox ran
     * perfectly happily. We skip the ROM loader, so its effect has to be
     * reproduced here.
     */
    nms->dev_regs[PP_GPO32_VAL / 4] = PP_GPO32_BCM_POWER;

    /* Identify the chip, or Apple's firmware picks another chip's addresses. */
    nms->dev_regs[PP_VER1_OFF / 4] = PP5022C_VER1;
    nms->dev_regs[PP_VER2_OFF / 4] = PP5022C_VER2;

    /*
     * The other thing the ROM loader leaves behind: the SysInfo pointer in the
     * top of IRAM. See SYSINFO_PTR_ADDR.
     *
     * Apple's firmware sizes its SDRAM in 0x13bc. It starts from a value the
     * image itself carries -- 0x1f4 does "mov fp, #0x700", whose nibble 2
     * indexes the table at 0x16d8 to 0x02000000, the right answer for a 5G --
     * and then throws it away in favour of the byte count it reads from
     * SysInfo + 0xe0. With no SysInfo the pointer is null, 0x13e8 reads
     * [0x000000e0] instead, which after the remap is the word 0x00000100 out
     * of the firmware's own vector table, and 256 bytes rounds down to a bank
     * of zero. All four banks then read back as empty, 0x40006020 comes to
     * zero, and the assert at 0xda0 -- "end of image and bss must fit in RAM",
     * line 231 -- fails with 0x00a0e294 > 0. The firmware prints that itself
     * once semihosting is on:
     *
     *     Type: _sys_exit
     *     : FFFFFFFF   x4      <- the four bank sizes, "empty"
     *     : 4001FF18           <- the tag address it was working from
     *     : 00001424           <- lr, inside 0x13bc after "bl 0xc90"
     *     : 0000027C           <- lr, inside reset after "bl 0x13bc"
     *
     * So we answer the one question the firmware asks: how much DRAM is there.
     * The value is DRAM_MEM_SIZE rather than a constant, because it is a fact
     * about this machine and not a number picked to make an assert pass.
     *
     * The magic is written too, and that was not obvious: leaving it at zero
     * gets the firmware past the RAM assert on its own, because 0x13e8
     * dereferences the pointer before 0x1400 checks the magic. It then dies a
     * little later, and the reason turns out to be the same block. 0x140c
     * publishes the pointer in 0x4000608c only when the magic matched, 0x1658
     * carries that word into DRAM, 0x281224 refuses to install the block
     * without it, and 0x265164 ends up reading the hardware id out of a bss
     * object nobody filled in. With the id reading zero, 0x2653a4's jump table
     * answers "no such machine", 0x111c58 returns a null driver, and 0x2668e4
     * takes a data abort dereferencing it. So the magic is not decoration; it
     * is the switch that turns the block on, and there is no way to hand the
     * firmware its hardware id without it.
     *
     * Only the four fields listed with SYSINFO_MAGIC in the header are
     * written -- the magic, the length, the hardware revision and the SDRAM
     * size. The rest
     * of the block stays at the zeros IRAM comes up with: the serial number,
     * the FireWire GUID, the board name and the image revisions are all
     * unknown to us, and a plausible-looking invention is worse than a zero
     * because it cannot be told apart from a fact. When a disassembly shows
     * who reads one of them and what for, that field gets filled in then.
     */
    uint32_t sysinfo[] = {
        [SYSINFO_LENGTH_OFF / 4]     = cpu_to_le32(SYSINFO_BLOCK_SIZE),
        [SYSINFO_HW_REV_OFF / 4]     = cpu_to_le32(nms->hw_revision),
        [SYSINFO_SDRAM_SIZE_OFF / 4] = cpu_to_le32(DRAM_MEM_SIZE),
    };
    uint32_t sysinfo_tag = cpu_to_le32(SYSINFO_MAGIC);
    uint32_t sysinfo_ptr = cpu_to_le32(SYSINFO_BLOCK_ADDR);

    QEMU_BUILD_BUG_ON(sizeof(sysinfo) > SYSINFO_BLOCK_SIZE);
    cpu_physical_memory_write(SYSINFO_BLOCK_ADDR, sysinfo, sizeof(sysinfo));
    cpu_physical_memory_write(SYSINFO_TAG_ADDR, &sysinfo_tag,
                              sizeof(sysinfo_tag));
    cpu_physical_memory_write(SYSINFO_PTR_ADDR, &sysinfo_ptr,
                              sizeof(sysinfo_ptr));

    MemoryRegion *cache_ctl = g_new(MemoryRegion, 1);
    memory_region_init_io(cache_ctl, NULL, &pp502x_cache_ctl_ops, nms,
                          "pp502x.cache-ctl", 0x1000);
    memory_region_add_subregion_overlap(sysmem, CACHE_CTL_MEM_BASE, cache_ctl, 1);

    nms->timer1 = timer_new_us(QEMU_CLOCK_VIRTUAL, pp502x_timer1_expired, nms);
    nms->timer2 = timer_new_us(QEMU_CLOCK_VIRTUAL, pp502x_timer2_expired, nms);

    MemoryRegion *timer = g_new(MemoryRegion, 1);
    memory_region_init_io(timer, NULL, &pp502x_timer_ops, nms,
                          "pp502x.timer", 0x1000);
    memory_region_add_subregion_overlap(sysmem, TIMER_MEM_BASE, timer, 1);

    /*
     * A floor under the whole SoC register window. Rockbox's header documents
     * the blocks Rockbox uses, but Apple's firmware reaches for others -- the
     * first fault after the remap went in was a read of 0x60009000, which
     * appears nowhere in pp5020.h. An unknown register reading back zero is
     * survivable; an unmapped one is a data abort, and the handler it vectors
     * to faults in turn and never returns. The modelled blocks sit on top of
     * this at a higher priority.
     */
    create_unimplemented_device("pp502x.soc-regs", 0x60000000, 0x10000);
    create_unimplemented_device("pp502x.dev-regs", 0x70000000, 0x10000);

    for (size_t i = 0; i < ARRAY_SIZE(ipod_video_unimplemented); i++) {
        create_unimplemented_device(ipod_video_unimplemented[i].name,
                                    ipod_video_unimplemented[i].base,
                                    ipod_video_unimplemented[i].size);
    }

    /*
     * SER0 is a plain 16550 with its registers four bytes apart, so QEMU's own
     * model does the job. The interrupt is deliberately left unwired -- the
     * NULL below -- because neither firmware drives the UART by interrupt in
     * any path we reach, and transmission does not depend on it. PP_SER0_IRQ
     * is defined but unused for the same reason.
     */
    serial_mm_init(sysmem, SER0_MEM_BASE, 2, NULL, 115200, serial_hd(0),
                   DEVICE_LITTLE_ENDIAN);

    /*
     * The two DMA blocks. They sit inside the pp502x.soc-regs floor, so they
     * go in at a higher overlap priority rather than beside it.
     *
     * Block A is the one Apple uses to push the Broadcom's firmware into the
     * chip; block B is the one Rockbox documents and uses for audio. Both are
     * wired because the retailOS's boot sweep touches both and its driver
     * builds channels on both.
     */
    static const struct { hwaddr base; int irq; } dma_blocks[] = {
        { DMA_A_MEM_BASE, PP_DMA_A_IRQ },
        { DMA_B_MEM_BASE, PP_DMA_B_IRQ },
    };
    for (size_t i = 0; i < ARRAY_SIZE(dma_blocks); i++) {
        DeviceState *dma = qdev_new(TYPE_PP502X_DMA);
        sysbus_realize_and_unref(SYS_BUS_DEVICE(dma), &error_fatal);
        sysbus_connect_irq(SYS_BUS_DEVICE(dma), 0,
                           qdev_get_gpio_in(nms->intc, dma_blocks[i].irq));
        sysbus_mmio_map_overlap(SYS_BUS_DEVICE(dma), 0, dma_blocks[i].base, 1);
    }

    /*
     * The panel hangs off a Broadcom companion chip rather than off the SoC,
     * so this is what actually puts pixels on screen.
     */
    DeviceState *bcm = qdev_new(TYPE_IPOD_VIDEO_BCM);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(bcm), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(bcm), 0, BCM_MEM_BASE);

    /*
     * The disk. The task file is standard ATA with the registers four bytes
     * apart, but the PP wraps it in its own configuration, timing and DMA
     * registers, and the application needs that DMA: the Rockbox bootloader
     * reads with READ MULTIPLE, the app issues READ DMA. So the whole window
     * is one device rather than the generic mmio-ide with a stub beside it.
     */
    DeviceState *ide = qdev_new(TYPE_PP502X_IDE);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(ide), &error_fatal);
    sysbus_connect_irq(SYS_BUS_DEVICE(ide), 0,
                       qdev_get_gpio_in(nms->intc, PP_IDE_IRQ));
    sysbus_mmio_map(SYS_BUS_DEVICE(ide), 0, IDE_MEM_BASE);
    pp502x_ide_init_drive(ide, drive_get(IF_IDE, 0, 0));

    /*
     * I2C, and the power management chip on it. This shares its window with
     * the click wheel: the controller has +0x00..+0xff and the wheel +0x100
     * upwards, so it goes in at a higher overlap priority than the wheel's
     * page-sized region rather than beside it.
     *
     * No interrupt is connected. Neither firmware uses one for I2C -- both
     * poll STATUS -- and on this SoC the line the header calls I2C_IRQ is the
     * click wheel's.
     */
    DeviceState *i2c = qdev_new(TYPE_PP502X_I2C);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(i2c), &error_fatal);
    sysbus_mmio_map_overlap(SYS_BUS_DEVICE(i2c), 0, I2C_MEM_BASE, 2);

    /*
     * The PCF50605 at 7-bit address 0x08, which is where Rockbox's
     * pcf50605.c, iPodLinux and Apple's 0x283540 all address it. It is the
     * only device we can identify on this bus; Apple also talks to something
     * at 0x2a, which nothing in Rockbox or iPodLinux names, so that address
     * stays empty and transfers to it are answered with a bus error, which is
     * what an unpopulated address does.
     */
    i2c_slave_create_simple(pp502x_i2c_get_bus(i2c), TYPE_PCF50605,
                            PCF50605_I2C_ADDR);

    ipod_video_load_image(machine, nms);

    qemu_register_reset(ipod_video_cpu_reset, nms);
}

static void ipod_video_machine_class_init(ObjectClass *klass, void *data)
{
    MachineClass *mc = MACHINE_CLASS(klass);

    mc->desc = "Apple iPod 5th generation (Video), PortalPlayer PP502x";
    mc->init = ipod_video_machine_init;
    /* Two ARM7TDMI cores, CPU and COP, both running the same image. */
    mc->max_cpus = PP_NUM_CORES;
    mc->min_cpus = PP_NUM_CORES;
    mc->default_cpus = PP_NUM_CORES;
    /*
     * The real cores are ARM7TDMI, which is ARMv4T, and QEMU has no model for
     * it. arm926 is ARMv5TE: a superset, so v4T firmware runs unchanged, and
     * it is the best exercised ARM model in the tree. -cpu ti925t gets the
     * architecture level exactly right if that ever turns out to matter.
     */
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("arm926");
}

static const TypeInfo ipod_video_machine_info = {
    .name          = TYPE_IPOD_VIDEO_MACHINE,
    .parent        = TYPE_MACHINE,
    .instance_size = sizeof(IPodVideoMachineState),
    .class_size    = sizeof(IPodVideoMachineClass),
    .class_init    = ipod_video_machine_class_init,
    .instance_init = ipod_video_instance_init,
};

static void ipod_video_machine_types(void)
{
    type_register_static(&ipod_video_machine_info);
}

type_init(ipod_video_machine_types)
