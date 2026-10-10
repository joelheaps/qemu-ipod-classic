/* See ipod_classic_edgeic.h. */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "migration/vmstate.h"
#include "hw/arm/ipod_classic/ipod_classic_edgeic.h"

#define EDGEIC_ENABLE  0x004
#define EDGEIC_PENDING 0x008
#define EDGEIC_ACK     0x00c

static void edgeic_update(IPodClassicEdgeICState *s)
{
    qemu_set_irq(s->irq, (s->pending & s->enable) != 0);
}

/* Input line: latch a rising edge; a falling edge does not clear it. */
static void edgeic_set_irq(void *opaque, int n, int level)
{
    IPodClassicEdgeICState *s = opaque;

    if (level && n < EDGEIC_NIN) {
        s->pending |= 1u << n;
        edgeic_update(s);
    }
}

static uint64_t edgeic_read(void *opaque, hwaddr offset, unsigned size)
{
    IPodClassicEdgeICState *s = opaque;

    if (offset >= EDGEIC_MEM_SIZE) {
        return 0;
    }
    switch (offset) {
    case EDGEIC_ENABLE:
        return s->enable;
    case EDGEIC_PENDING:
        return s->pending;
    default:
        return s->reg[offset / 4];
    }
}

static void edgeic_write(void *opaque, hwaddr offset, uint64_t value,
                         unsigned size)
{
    IPodClassicEdgeICState *s = opaque;

    if (offset >= EDGEIC_MEM_SIZE) {
        return;
    }
    switch (offset) {
    case EDGEIC_ENABLE:
        s->enable = (uint32_t)value;
        edgeic_update(s);
        break;
    case EDGEIC_ACK:
        /* Write-1-to-clear the latched sources (the video handler writes 8). */
        s->pending &= ~(uint32_t)value;
        edgeic_update(s);
        break;
    default:
        s->reg[offset / 4] = (uint32_t)value;
        break;
    }
}

static const MemoryRegionOps edgeic_ops = {
    .read = edgeic_read,
    .write = edgeic_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static void edgeic_reset(DeviceState *dev)
{
    IPodClassicEdgeICState *s = IPOD_CLASSIC_EDGEIC(dev);

    memset(s->reg, 0, sizeof(s->reg));
    s->pending = 0;
    s->enable = 0;
    edgeic_update(s);
}

static void edgeic_init(Object *obj)
{
    IPodClassicEdgeICState *s = IPOD_CLASSIC_EDGEIC(obj);

    memory_region_init_io(&s->iomem, obj, &edgeic_ops, s,
                          TYPE_IPOD_CLASSIC_EDGEIC, EDGEIC_MEM_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    qdev_init_gpio_in(DEVICE(obj), edgeic_set_irq, EDGEIC_NIN);
}

static const VMStateDescription vmstate_edgeic = {
    .name = TYPE_IPOD_CLASSIC_EDGEIC,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_UINT32_ARRAY(reg, IPodClassicEdgeICState, EDGEIC_NREG),
        VMSTATE_UINT32(pending, IPodClassicEdgeICState),
        VMSTATE_UINT32(enable, IPodClassicEdgeICState),
        VMSTATE_END_OF_LIST()
    },
};

static void edgeic_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->desc = "S5L8702 edge interrupt controller";
    dc->reset = edgeic_reset;
    dc->vmsd = &vmstate_edgeic;
    dc->user_creatable = false;
}

static const TypeInfo edgeic_info = {
    .name          = TYPE_IPOD_CLASSIC_EDGEIC,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IPodClassicEdgeICState),
    .instance_init = edgeic_init,
    .class_init    = edgeic_class_init,
};

static void edgeic_register_types(void)
{
    type_register_static(&edgeic_info);
}

type_init(edgeic_register_types)
