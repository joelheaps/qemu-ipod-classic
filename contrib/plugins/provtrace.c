/*
 * provtrace.c -- store/load provenance tracer (RAM data-flow, no values).
 *
 * Logs runs of guest stores into dst=LO-HI as TSV, each tagged with the last
 * two load addresses seen in src=LO-HI before the run started and ended, so an
 * offline walk (tools/ipod_classic/prov_walk.py) can follow "who wrote this
 * surface, from where did it read". Consecutive stores from one PC to adjacent
 * addresses collapse into one run. Values are not available to 8.2 memory
 * callbacks; take final contents from a memory dump instead.
 *
 *   -plugin libprovtrace.so,out=FILE,dst=0xLO-0xHI[,src=0xLO-0xHI][,start=0xPC][,skip=0xPC]...
 *
 * Output columns: seq0 seq1 pc pc2 dst0 n size ld0_first ld0_last ld1_first ld1_last
 * (hex; seq = index among in-range stores, so it orders runs in time).
 */
#include <glib.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <qemu-plugin.h>

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

static FILE *out;
static uint64_t dst_lo, dst_hi, src_lo, src_hi;
static GArray *skip;
static uint64_t ld0, ld1, seq, start_pc, active = 1;
static struct {
    int live;
    uint64_t pc, pc2, dst0, next, n, size, seq0, seq1, l0a, l0b, l1a, l1b;
} run;

static void flush_run(void)
{
    if (run.live) {
        fprintf(out, "%" PRIx64 "\t%" PRIx64 "\t%" PRIx64 "\t%" PRIx64 "\t%" PRIx64
                "\t%" PRIu64 "\t%" PRIu64 "\t%" PRIx64 "\t%" PRIx64
                "\t%" PRIx64 "\t%" PRIx64 "\n", run.seq0, run.seq1, run.pc, run.pc2,
                run.dst0, run.n, run.size, run.l0a, run.l0b, run.l1a, run.l1b);
        run.live = 0;
    }
}

static void mem_cb(unsigned int cpu, qemu_plugin_meminfo_t info,
                   uint64_t va, void *udata)
{
    if (!active) {
        return;
    }
    if (!qemu_plugin_mem_is_store(info)) {
        if (va >= src_lo && va < src_hi) {
            ld1 = ld0;
            ld0 = va;
        }
        return;
    }
    if (va < dst_lo || va >= dst_hi) {
        return;
    }
    uint64_t pc = (uint64_t)(uintptr_t)udata;
    for (guint i = 0; skip && i < skip->len; i++) {
        if (g_array_index(skip, uint64_t, i) == pc) {
            return;
        }
    }
    uint64_t size = 1u << qemu_plugin_mem_size_shift(info);
    seq++;
    if (run.live && run.next == va && run.size == size) {
        if (pc != run.pc) {
            run.pc2 = pc;
        }
        run.n++;
        run.next = va + size;
        run.seq1 = seq;
        run.l0b = ld0;
        run.l1b = ld1;
        return;
    }
    flush_run();
    run.live = 1;
    run.pc = pc;
    run.pc2 = 0;
    run.dst0 = va;
    run.next = va + size;
    run.n = 1;
    run.size = size;
    run.seq0 = run.seq1 = seq;
    run.l0a = run.l0b = ld0;
    run.l1a = run.l1b = ld1;
}

static void start_cb(unsigned int cpu, void *udata)
{
    active = 1;
}

static void tb_trans(qemu_plugin_id_t id, struct qemu_plugin_tb *tb)
{
    size_t n = qemu_plugin_tb_n_insns(tb);
    for (size_t i = 0; i < n; i++) {
        struct qemu_plugin_insn *insn = qemu_plugin_tb_get_insn(tb, i);
        if (start_pc && qemu_plugin_insn_vaddr(insn) == start_pc) {
            qemu_plugin_register_vcpu_insn_exec_cb(insn, start_cb,
                                                   QEMU_PLUGIN_CB_NO_REGS, NULL);
        }
        qemu_plugin_register_vcpu_mem_cb(insn, mem_cb, QEMU_PLUGIN_CB_NO_REGS,
                                         QEMU_PLUGIN_MEM_RW,
                                         (void *)(uintptr_t)qemu_plugin_insn_vaddr(insn));
    }
}

static void exit_cb(qemu_plugin_id_t id, void *p)
{
    flush_run();
    fclose(out);
}

static void range(const char *s, uint64_t *lo, uint64_t *hi)
{
    char **v = g_strsplit(s, "-", 2);
    *lo = g_ascii_strtoull(v[0], NULL, 0);
    *hi = v[1] ? g_ascii_strtoull(v[1], NULL, 0) : 0;
    g_strfreev(v);
}

QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id,
                                           const qemu_info_t *info,
                                           int argc, char **argv)
{
    const char *path = "provtrace.tsv";
    skip = g_array_new(FALSE, FALSE, sizeof(uint64_t));
    for (int i = 0; i < argc; i++) {
        char **kv = g_strsplit(argv[i], "=", 2);
        if (!g_strcmp0(kv[0], "out") && kv[1]) {
            path = g_strdup(kv[1]);
        } else if (!g_strcmp0(kv[0], "dst") && kv[1]) {
            range(kv[1], &dst_lo, &dst_hi);
        } else if (!g_strcmp0(kv[0], "src") && kv[1]) {
            range(kv[1], &src_lo, &src_hi);
        } else if (!g_strcmp0(kv[0], "start") && kv[1]) {
            start_pc = g_ascii_strtoull(kv[1], NULL, 0);
            active = 0;
        } else if (!g_strcmp0(kv[0], "skip") && kv[1]) {
            uint64_t pc = g_ascii_strtoull(kv[1], NULL, 0);
            g_array_append_val(skip, pc);
        }
        g_strfreev(kv);
    }
    if (!src_hi) {
        src_lo = dst_lo;
        src_hi = dst_hi;
    }
    out = fopen(path, "w");
    if (!out) {
        return -1;
    }
    setvbuf(out, NULL, _IOFBF, 1 << 20);
    qemu_plugin_register_vcpu_tb_trans_cb(id, tb_trans);
    qemu_plugin_register_atexit_cb(id, exit_cb, NULL);
    return 0;
}
