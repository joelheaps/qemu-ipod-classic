/*
 * S5L8702 video decoder, 0x39800000. See ipod_classic_vdec.h.
 *
 * Two modes:
 *  - default: latch-only stub. Every access is stored and read back, nothing is
 *    decoded and no status/event is produced. VDECTRACE=1 prints each access
 *    (addr, value, guest PC) so the block's protocol can be observed.
 *  - VDEC_DECODE=1 (default off): on the START write the block reads the H.264
 *    access unit and codec config that the pctrace `audump=` tap wrote to files,
 *    decodes the whole AU with libavcodec, writes YUV420 into the +0xcc/d0/d4
 *    planes, sets the done status and raises the VIC0 line-35 done interrupt.
 *    The file handoff is a labelled host-side shortcut: the firmware compacts
 *    the AU in place before submit, so the block itself cannot see it.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "migration/vmstate.h"
#include "hw/core/cpu.h"
#include "system/physmem.h"    /* physical_memory_write */
#include "hw/arm/ipod_classic/ipod_classic_vdec.h"

static uint32_t vdec_trace_pc(void)
{
    CPUState *cs = current_cpu;
    return cs ? (uint32_t)CPU_GET_CLASS(cs)->get_pc(cs) : 0;
}

static uint32_t vdec_reg_read(IPodClassicVdecState *s, hwaddr offset)
{
    switch (offset) {
    case S5L8702_VDEC_STATUS_F0:
        return s->status_f0;
    case S5L8702_VDEC_STATUS_F4:
        return s->status_f4;
    default:
        return s->reg[offset / 4];
    }
}

/*
 * The done signal is an edge: pulse the line. The edge-IC (bit 3) latches the
 * rising edge and holds the VIC line until the guest's handler acks it at
 * +0xc, so the pulse need not survive on its own (and a held level would
 * re-enter the handler for ever).
 */
static void vdec_pulse_irq(IPodClassicVdecState *s)
{
    s->irqs++;
    qemu_irq_raise(s->irq);
    qemu_irq_lower(s->irq);
}

/* Read a whole file into a freshly g_malloc'd buffer; NULL on any error. */
static uint8_t *vdec_read_file(const char *path, int *out_len)
{
    GError *err = NULL;
    gchar *data = NULL;
    gsize len = 0;

    if (!path || !*path) {
        return NULL;
    }
    if (!g_file_get_contents(path, &data, &len, &err)) {
        g_clear_error(&err);
        return NULL;
    }
    *out_len = (int)len;
    return (uint8_t *)data;
}

/*
 * Decode the tapped access unit into the guest's Y/U/V planes. Called on the
 * START write. Returns true if a frame was copied and the done status set.
 */
static bool vdec_decode_submit(IPodClassicVdecState *s)
{
    int au_len = 0, cfg_len = 0, ret;
    uint8_t *au = NULL, *cfg = NULL, *planes = NULL;
    int width, height, ysz, csz, cstride;
    uint32_t yaddr = s->reg[S5L8702_VDEC_CUR_Y / 4];
    uint32_t uaddr = s->reg[S5L8702_VDEC_CUR_U / 4];
    uint32_t vaddr = s->reg[S5L8702_VDEC_CUR_V / 4];

    au = vdec_read_file(s->au_path, &au_len);
    if (!au) {
        if (s->trace || s->dec_errors < 4) {
            fprintf(stderr, "VDEC: cannot read AU file %s\n",
                    s->au_path ? s->au_path : "(unset)");
        }
        s->dec_errors++;
        return false;
    }
    cfg = vdec_read_file(s->cfg_path, &cfg_len);
    if (!cfg) {
        fprintf(stderr, "VDEC: cannot read config file %s\n",
                s->cfg_path ? s->cfg_path : "(unset)");
        s->dec_errors++;
        g_free(au);
        return false;
    }
    width = S5L8702_VDEC_MB_W(s->reg[S5L8702_VDEC_SIZE / 4]);
    height = S5L8702_VDEC_MB_H(s->reg[S5L8702_VDEC_SIZE / 4]);
    if (width <= 0 || height <= 0 || yaddr == 0 || uaddr == 0 || vaddr == 0) {
        fprintf(stderr, "VDEC: bad geometry (%dx%d) or null plane "
                "(y=0x%08x u=0x%08x v=0x%08x)\n",
                width, height, yaddr, uaddr, vaddr);
        s->dec_errors++;
        goto out;
    }
    ysz = width * height;
    csz = ysz / 4;
    cstride = width / 2;
    planes = g_malloc(ysz + 2 * csz);
    ret = vdec_dec_frame(s->dec, cfg, cfg_len, au, au_len,
                         planes, width, planes + ysz, cstride,
                         planes + ysz + csz, cstride, width, height);
    if (ret != 0) {
        if (s->dec_errors < 8 || s->trace) {
            fprintf(stderr, "VDEC: decode failed (ret=%d, au=%d cfg=%d)\n",
                    ret, au_len, cfg_len);
        }
        s->dec_errors++;
        goto out;
    }
    physical_memory_write(yaddr, planes, ysz);
    physical_memory_write(uaddr, planes + ysz, csz);
    physical_memory_write(vaddr, planes + ysz + csz, csz);
    s->decodes++;
    if (s->decodes <= 4 || s->trace) {
        fprintf(stderr, "VDEC: decoded frame #%" PRIu64 " %dx%d au=%d "
                "cfg=%d -> y=0x%08x u=0x%08x v=0x%08x\n",
                s->decodes, width, height, au_len, cfg_len, yaddr, uaddr,
                vaddr);
    }
    g_free(planes);
    g_free(au);
    g_free(cfg);
    return true;
out:
    g_free(planes);
    g_free(au);
    g_free(cfg);
    return false;
}

static uint64_t ipod_classic_vdec_read(void *opaque, hwaddr offset,
                                       unsigned size)
{
    IPodClassicVdecState *s = opaque;
    uint32_t val;

    if (offset >= S5L8702_VDEC_REG_BYTES) {
        return 0;
    }
    val = vdec_reg_read(s, offset);
    s->accesses++;

    /*
     * The done poll: the guest's submit reads +0xf4 (twice) then +0xf0 once,
     * and the context reset never writes +0xf0. Clearing the done latch on the
     * +0xf0 read is exactly the guest's own completion test, and it drops the
     * line the guest cannot clear itself (the handler only acks the edge-IC,
     * which QEMU models as RAM).
     */
    if (offset == S5L8702_VDEC_STATUS_F0 && (s->status_f0 & S5L8702_VDEC_DONE)) {
        s->status_f0 &= ~S5L8702_VDEC_DONE;
    }

    if (s->trace) {
        fprintf(stderr, "VDECTRACE rd +0x%03" HWADDR_PRIx " -> 0x%08x  "
                "pc~0x%08x%s\n", offset, val, vdec_trace_pc(),
                offset == S5L8702_VDEC_STATUS_F0 ? "  [STATUS_F0]" :
                offset == S5L8702_VDEC_STATUS_F4 ? "  [STATUS_F4]" : "");
    }
    return val;
}

static void ipod_classic_vdec_write(void *opaque, hwaddr offset,
                                    uint64_t value, unsigned size)
{
    IPodClassicVdecState *s = opaque;

    if (offset >= S5L8702_VDEC_REG_BYTES) {
        return;
    }
    /*
     * +0xf0/+0xf4 are device-owned status. The guest's context reset writes 0
     * to them; latching that would erase the done bits before the poll reads
     * them, so ignore the write (readback stays the model's status).
     */
    if (offset != S5L8702_VDEC_STATUS_F0 &&
        offset != S5L8702_VDEC_STATUS_F4) {
        s->reg[offset / 4] = (uint32_t)value;
    }
    s->accesses++;

    if (s->trace) {
        fprintf(stderr, "VDECTRACE wr +0x%03" HWADDR_PRIx " = 0x%08x  "
                "pc~0x%08x%s%s\n", offset, (uint32_t)value, vdec_trace_pc(),
                offset == S5L8702_VDEC_CTRL ? "  [CTRL]" :
                offset == S5L8702_VDEC_DESC_TABLE ? "  [TABLE]" : "",
                offset == S5L8702_VDEC_CTRL &&
                (value & S5L8702_VDEC_CTRL_START) ? "  START" : "");
    }

    if (offset == S5L8702_VDEC_CTRL &&
        (value & S5L8702_VDEC_CTRL_START)) {
        s->status_f0 = 0;
        s->status_f4 = 0;
        if (s->decode && s->dec && vdec_decode_submit(s)) {
            s->status_f0 = S5L8702_VDEC_DONE;   /* +0xf4 = 0 = success */
            vdec_pulse_irq(s);
        }
    }
}

static const MemoryRegionOps ipod_classic_vdec_ops = {
    .read = ipod_classic_vdec_read,
    .write = ipod_classic_vdec_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static void ipod_classic_vdec_reset(DeviceState *dev)
{
    IPodClassicVdecState *s = IPOD_CLASSIC_VDEC(dev);

    memset(s->reg, 0, sizeof(s->reg));
    s->accesses = 0;
    s->status_f0 = 0;
    s->status_f4 = 0;
    if (s->dec) {
        vdec_dec_flush(s->dec);
    }
}

static void ipod_classic_vdec_init(Object *obj)
{
    IPodClassicVdecState *s = IPOD_CLASSIC_VDEC(obj);

    memory_region_init_io(&s->iomem, obj, &ipod_classic_vdec_ops, s,
                          TYPE_IPOD_CLASSIC_VDEC, S5L8702_VDEC_MEM_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);

    const char *e = getenv("VDECTRACE");
    s->trace = e && *e && *e != '0';

    e = getenv("VDEC_DECODE");
    s->decode = e && *e && *e != '0';
    if (s->decode) {
        s->au_path = g_strdup(getenv("VDEC_AU_FILE"));
        s->cfg_path = g_strdup(getenv("VDEC_CFG_FILE"));
        if (!vdec_dec_available() || !s->au_path || !*s->au_path ||
            !s->cfg_path || !*s->cfg_path) {
            fprintf(stderr, "VDEC: VDEC_DECODE requested but the decoder or "
                    "VDEC_AU_FILE/VDEC_CFG_FILE is missing; staying a stub\n");
            s->decode = false;
        } else {
            s->dec = vdec_dec_new();
            fprintf(stderr, "VDEC: decode model on (libavcodec); AU=%s cfg=%s\n",
                    s->au_path, s->cfg_path);
        }
    }
    fprintf(stderr, "VDEC: 0x39800000 %s device up%s\n",
            s->decode ? "decoder" : "stub",
            s->trace ? " (VDECTRACE on)" : "");
}

static void ipod_classic_vdec_finalize(Object *obj)
{
    IPodClassicVdecState *s = IPOD_CLASSIC_VDEC(obj);

    g_free(s->au_path);
    g_free(s->cfg_path);
    vdec_dec_free(s->dec);
}

static const VMStateDescription vmstate_ipod_classic_vdec = {
    .name = TYPE_IPOD_CLASSIC_VDEC,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_UINT32_ARRAY(reg, IPodClassicVdecState,
                             S5L8702_VDEC_REG_WORDS),
        VMSTATE_UINT64(accesses, IPodClassicVdecState),
        VMSTATE_END_OF_LIST()
    },
};

static void ipod_classic_vdec_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->desc = "S5L8702 video decoder";
    device_class_set_legacy_reset(dc, ipod_classic_vdec_reset);
    dc->vmsd = &vmstate_ipod_classic_vdec;
    dc->user_creatable = false;
}

static const TypeInfo ipod_classic_vdec_info = {
    .name          = TYPE_IPOD_CLASSIC_VDEC,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IPodClassicVdecState),
    .instance_init = ipod_classic_vdec_init,
    .instance_finalize = ipod_classic_vdec_finalize,
    .class_init    = ipod_classic_vdec_class_init,
};

static void ipod_classic_vdec_register_types(void)
{
    type_register_static(&ipod_classic_vdec_info);
}

type_init(ipod_classic_vdec_register_types)
