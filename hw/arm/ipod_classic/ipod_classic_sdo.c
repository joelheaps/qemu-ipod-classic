/*
 * S5L8702 display/scaler output block, 0x39300000. See the header for why.
 * Minimal: the frame-done edge on VIC line 30 and its +0x280 ack.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "hw/arm/ipod_classic/ipod_classic_sdo.h"

#define SDO_FPS_DEFAULT 60

static bool sdo_frame_irq_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("SDO_FRAME_IRQ");
        on = (e && *e && *e != '0') ? 1 : 0;
        if (on) {
            fprintf(stderr, "SDO_FRAME_IRQ: display frame-done on VIC line %d\n",
                    S5L8702_SDO_IRQ);
        }
    }
    return on == 1;
}

static bool sdo_trace_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("SDOTRACE");
        on = (e && *e && *e != '0') ? 1 : 0;
    }
    return on == 1;
}

static void sdo_frame_tick(void *opaque)
{
    IPodClassicSdoState *s = opaque;

    s->frames++;
    /*
     * A fresh edge every frame. The guest registers line 30 only when the video
     * engine starts, so a single level raised before that is missed if the VIC
     * latches on the rising edge; lower then raise guarantees the edge lands
     * once the line is enabled. The guest's +0x280 ack drops the line.
     */
    qemu_irq_lower(s->irq);
    s->frame_pending = true;
    qemu_irq_raise(s->irq);
    if (sdo_trace_on()) {
        fprintf(stderr, "SDOTRACE  frame #%" PRIu64 " raised, pending\n",
                s->frames);
    }
    timer_mod(s->frame_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + s->irq_ns);
}

static uint64_t ipod_classic_sdo_read(void *opaque, hwaddr offset, unsigned size)
{
    IPodClassicSdoState *s = opaque;

    if (offset == S5L8702_SDO_FRAME_DONE) {
        return (s->reg[offset / 4] & ~1u) | (s->frame_pending ? 1u : 0u);
    }
    if (offset < S5L8702_SDO_MEM_SIZE) {
        return s->reg[offset / 4];
    }
    return 0;
}

static void ipod_classic_sdo_write(void *opaque, hwaddr offset,
                                   uint64_t value, unsigned size)
{
    IPodClassicSdoState *s = opaque;

    if (offset == S5L8702_SDO_FRAME_DONE) {
        s->reg[offset / 4] = value;
        if (value & 1) {
            s->acks++;
            if (s->frame_pending) {
                s->frame_pending = false;
                qemu_irq_lower(s->irq);
            }
            if (sdo_trace_on()) {
                fprintf(stderr, "SDOTRACE  ack #%" PRIu64 " (frame #%" PRIu64 ")\n",
                        s->acks, s->frames);
            }
        }
        return;
    }
    if (offset < S5L8702_SDO_MEM_SIZE) {
        s->reg[offset / 4] = value;
    }
}

static const MemoryRegionOps ipod_classic_sdo_ops = {
    .read = ipod_classic_sdo_read,
    .write = ipod_classic_sdo_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void ipod_classic_sdo_reset(DeviceState *dev)
{
    IPodClassicSdoState *s = IPOD_CLASSIC_SDO(dev);

    memset(s->reg, 0, sizeof(s->reg));
    s->frame_pending = false;
    qemu_irq_lower(s->irq);
    if (s->frame_timer && sdo_frame_irq_on()) {
        timer_mod(s->frame_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + s->irq_ns);
    }
}

static void ipod_classic_sdo_realize(DeviceState *dev, Error **errp)
{
    IPodClassicSdoState *s = IPOD_CLASSIC_SDO(dev);
    const char *fps = getenv("SDO_FPS");
    int hz = fps && *fps ? atoi(fps) : SDO_FPS_DEFAULT;

    if (hz < 1) {
        hz = SDO_FPS_DEFAULT;
    }
    s->irq_ns = NANOSECONDS_PER_SECOND / (uint64_t)hz;

    memory_region_init_io(&s->iomem, OBJECT(s), &ipod_classic_sdo_ops, s,
                          TYPE_IPOD_CLASSIC_SDO, S5L8702_SDO_MEM_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);

    s->frame_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, sdo_frame_tick, s);
    if (sdo_frame_irq_on()) {
        timer_mod(s->frame_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + s->irq_ns);
    }
}

static const VMStateDescription vmstate_ipod_classic_sdo = {
    .name = TYPE_IPOD_CLASSIC_SDO,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_UINT32_ARRAY(reg, IPodClassicSdoState, S5L8702_SDO_NREG),
        VMSTATE_BOOL(frame_pending, IPodClassicSdoState),
        VMSTATE_END_OF_LIST()
    },
};

static void ipod_classic_sdo_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->desc = "S5L8702 display output (frame-done only)";
    dc->realize = ipod_classic_sdo_realize;
    device_class_set_legacy_reset(dc, ipod_classic_sdo_reset);
    dc->vmsd = &vmstate_ipod_classic_sdo;
    dc->user_creatable = false;
}

static const TypeInfo ipod_classic_sdo_info = {
    .name          = TYPE_IPOD_CLASSIC_SDO,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IPodClassicSdoState),
    .class_init    = ipod_classic_sdo_class_init,
};

static void ipod_classic_sdo_register_types(void)
{
    type_register_static(&ipod_classic_sdo_info);
}

type_init(ipod_classic_sdo_register_types)
