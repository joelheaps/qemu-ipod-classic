/*
 * S5L8702 clock and PLL block.
 *
 * A register file, plus the two pieces of behaviour the guest can observe:
 * PLLLOCK has to report the PLLs locked or a driver that waits for them never
 * comes back, and a write to SWRCON reboots the machine.
 *
 * See include/hw/arm/ipod_classic/ipod_classic_clk.h for where the register
 * map comes from.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "exec/cpu-common.h"
#include "exec/exec-all.h"
#include "hw/core/cpu.h"
#include "target/arm/cpu.h"
#include "migration/vmstate.h"
#include "sysemu/runstate.h"
#include "qemu/error-report.h"
#include "exec/memory.h"
#include "hw/arm/ipod_classic/ipod_classic_clk.h"
#include "hw/arm/ipod_classic/ipod_classic.h"

/*
 * The 256 bytes at the top of IRAM1 that the bootrom and the OS use to talk to
 * each other. The first eight bytes are the name of the image that should run
 * next, which is exactly what we want to see when the guest asks for a reboot:
 * 0x0827a8d4 copies one of "--------", "diskmode", "diskscan", "softupdt" or
 * "retailOS" there from the table at 0x0837be25, and 0x08322b10 then writes
 * SWRCON. See wiki/11-classic6g-retailos.md.
 */
#define S5L8702_BOOT_TAG_ADDR   0x2203ff00

static uint64_t ipod_classic_clk_read(void *opaque, hwaddr offset,
                                      unsigned size)
{
    IPodClassicClkState *s = opaque;
    hwaddr word_off = offset & ~(hwaddr)3;
    unsigned shift = 8 * (offset & 3);
    uint32_t word;

    if (offset + size > S5L8702_CLK_MEM_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: read past the block at 0x%"
                      HWADDR_PRIx "\n", __func__, offset);
        return 0;
    }

    switch (word_off) {
    case S5L8702_CLK_PLLLOCK:
        /*
         * "Start status: 0 -> in progress, 1 -> locked" for bits 0..2, and the
         * divisor-mode lock in bits 4..6. Our PLLs settle instantly, so report
         * every PLL the guest enabled in PLLMODE as both started and locked.
         * Reporting a PLL locked that was never enabled would be a lie the
         * guest could catch; reporting zero would hang whoever waits.
         */
        {
            uint32_t mode = s->reg[S5L8702_CLK_PLLMODE / 4];
            uint32_t lock = 0;
            for (int n = 0; n < 3; n++) {
                if (mode & (1u << n)) {
                    lock |= S5L8702_PLLLOCK_LCK(n) | S5L8702_PLLLOCK_DMLCK(n);
                }
            }
            word = lock;
        }
        break;
    default:
        word = s->reg[word_off / 4];
        break;
    }

    /*
     * Sub-word reads are real: Rockbox's cg16_config() configures a 16-bit
     * clock gate with a 32-bit read-modify-write and then polls it with an
     * ldrh (clocking-s5l8702.c). The register file is 32-bit words, so hand
     * back only the bytes the access asked for. The caller is aligned (the
     * core rejects unaligned IO because .valid.unaligned is false), so
     * shift + size * 8 never exceeds 32.
     */
    return extract32(word, shift, size * 8);
}

/*
 * Clk_SM1 (PWRCON0 bit 4) clocks IRAM1; Rockbox's app gates it in system_init().
 * On hardware any IRAM1 access then hangs the bus. Here the RAM simply vanishes
 * (reads 0, writes dropped), so code or data placed there dies loudly.
 */
static void ipod_classic_clk_gate_iram1(uint32_t pwrcon0)
{
    static MemoryRegion *iram1;     /* cached: find() cannot see it once gated */
    bool gated = pwrcon0 & (1u << 4);

    if (!iram1) {
        MemoryRegionSection sec = memory_region_find(get_system_memory(),
                                                     IRAM1_MEM_BASE, 1);
        iram1 = sec.mr;
        if (iram1) {
            memory_region_unref(iram1);
        }
    }
    if (iram1) {
        if (iram1->enabled == gated) {                    /* state changes */
            warn_report("S5L8702: Clk_SM1 %s -- IRAM1 is %s",
                        gated ? "gated" : "ungated",
                        gated ? "DEAD (real bus would hang)" : "back");
        }
        memory_region_set_enabled(iram1, !gated);
    }
}

static void ipod_classic_clk_write(void *opaque, hwaddr offset,
                                   uint64_t value, unsigned size)
{
    IPodClassicClkState *s = opaque;
    hwaddr word_off = offset & ~(hwaddr)3;
    unsigned shift = 8 * (offset & 3);
    uint32_t *reg = &s->reg[word_off / 4];

    if (offset + size > S5L8702_CLK_MEM_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: write past the block at 0x%"
                      HWADDR_PRIx " (value 0x%" PRIx64 ")\n", __func__, offset,
                      value);
        return;
    }

    /* Keep only the bytes the access carries, then place them in the word. */
    value &= size < 4 ? ((1u << (size * 8)) - 1) : 0xffffffffu;

    switch (word_off) {
    case S5L8702_CLK_SWRCON:
        if (value != 0) {
            char tag[9] = { 0 };

            cpu_physical_memory_read(S5L8702_BOOT_TAG_ADDR, tag, 8);
            for (int i = 0; i < 8; i++) {
                if (tag[i] < 0x20 || tag[i] > 0x7e) {
                    tag[i] = '.';
                }
            }
            CPUState *cs = current_cpu;
            uint32_t pc = cs ? (uint32_t)CPU_GET_CLASS(cs)->get_pc(cs) : 0;

            warn_report("S5L8702: guest asked for a software reset "
                        "(SWRCON = 0x%" PRIx64 "), next image \"%s\" pc~0x%08x",
                        value, tag, pc);
            s->reg[S5L8702_CLK_RSTSR / 4] |= S5L8702_RSTSR_SWR;
            qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
        }
        *reg = deposit32(*reg, shift, size * 8, value);
        break;
    case S5L8702_CLK_PWRCON0:
        *reg = deposit32(*reg, shift, size * 8, value);
        ipod_classic_clk_gate_iram1(*reg);
        break;
    case S5L8702_CLK_RSTSR:
        /* Write-one-to-clear, the usual Samsung convention for this one. */
        *reg &= ~(uint32_t)(value << shift);
        break;
    case S5L8702_CLK_PLLLOCK:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: PLLLOCK is read-only\n", __func__);
        break;
    default:
        *reg = deposit32(*reg, shift, size * 8, value);
        break;
    }
}

static const MemoryRegionOps ipod_classic_clk_ops = {
    .read = ipod_classic_clk_read,
    .write = ipod_classic_clk_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    /*
     * The block is byte-addressable; Rockbox reaches the 16-bit clock gates
     * (CG16_*, e.g. CG16_RTIME at 0x12) with ldrh/strh, and its cg16_config()
     * polls one after a 32-bit read-modify-write.
     */
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static void ipod_classic_clk_reset(DeviceState *dev)
{
    IPodClassicClkState *s = IPOD_CLASSIC_CLK(dev);

    memset(s->reg, 0, sizeof(s->reg));
    ipod_classic_clk_gate_iram1(0);
    /*
     * Tell the guest the last reset was a power-on one. Apple's retailOS reads
     * RSTSR; leaving it at zero would mean "no reset happened", which is not a
     * state real silicon can be in.
     */
    s->reg[S5L8702_CLK_RSTSR / 4] = S5L8702_RSTSR_HWR;
}

static void ipod_classic_clk_init(Object *obj)
{
    IPodClassicClkState *s = IPOD_CLASSIC_CLK(obj);

    memory_region_init_io(&s->iomem, obj, &ipod_classic_clk_ops, s,
                          TYPE_IPOD_CLASSIC_CLK, S5L8702_CLK_MEM_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static const VMStateDescription vmstate_ipod_classic_clk = {
    .name = TYPE_IPOD_CLASSIC_CLK,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_UINT32_ARRAY(reg, IPodClassicClkState,
                             S5L8702_CLK_MEM_SIZE / 4),
        VMSTATE_END_OF_LIST()
    },
};

static void ipod_classic_clk_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->desc = "S5L8702 clock and PLL block";
    dc->reset = ipod_classic_clk_reset;
    dc->vmsd = &vmstate_ipod_classic_clk;
}

static const TypeInfo ipod_classic_clk_info = {
    .name          = TYPE_IPOD_CLASSIC_CLK,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IPodClassicClkState),
    .instance_init = ipod_classic_clk_init,
    .class_init    = ipod_classic_clk_class_init,
};

static void ipod_classic_clk_register_types(void)
{
    type_register_static(&ipod_classic_clk_info);
}

type_init(ipod_classic_clk_register_types)
