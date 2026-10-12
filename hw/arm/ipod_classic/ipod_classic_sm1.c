/*
 * S5L8702 "SM1" audio clock/stream engine, 0x38500000.
 *
 * See include/hw/arm/ipod_classic/ipod_classic_sm1.h for what the block is and
 * hw/arm/ipod_classic/README.md for the knobs. This is the
 * real, stateful replacement for the two hand-faked reads (identity magic + a
 * hard-coded "clock ready" bit) that the previous session dropped into
 * ipod_classic.c. Nothing here is a constant pulled out of thin air: every read
 * that unblocks the guest is derived from a write the guest itself made, so the
 * two bring-up paths in the retailOS -- the codec bring-up 0x0808170c and the
 * start/resume 0x08017378 -- each complete on their own terms instead of racing
 * a timeout or spinning on a zero.
 *
 * WHAT THE GUEST DOES (all VERIFIED in the osos disassembly; roles marked):
 *
 *  - Identity: getter 0x08075d64 reads +0xc80 and the caller compares it to
 *    0x004e7309 (literal *0x080821cc); a mismatch is `b .` forever at 0x080819b8.
 *    VERIFIED role. We answer the magic. (Was the one thing the old stub got.)
 *
 *  - Per-channel sub-blocks: 0x080ba768 drives a small state machine on a
 *    sub-block whose base is base + {idx*0x20 for idx<4; 0x100/0x180/0x200/0x230
 *    for idx 4..7} (computed by 0x080bce04). To power a channel up it writes 4 to
 *    sub+0x10, then polls sub+0x14 (read by 0x080b8470, masked &7) up to ten
 *    times waiting for 7, then writes 16 (run). If the poll never reads 7 it
 *    STILL writes 16 but stamps an error (-236) that the caller can act on. So a
 *    model that leaves sub+0x14 at zero does not hang -- it makes the firmware
 *    believe every channel failed to power up. We make sub+0x14 read 7 once its
 *    sub+0x10 has been commanded, i.e. the channel reached its target state.
 *    VERIFIED offsets/flow; the 4/16/7 meanings are INFERIDO (strong).
 *
 *  - Clock-ready: after the channel and codec config, the bring-up spins on
 *    +0xa98 bit2 with NO timeout (0x08081af8 `tst r0,#4; beq`), and the
 *    start/resume path gates its whole body on the same bit (0x08017394). On
 *    silicon the bit rises when the gated audio clock locks. We raise it once the
 *    firmware has begun powering the engine (its first channel command, the codec
 *    enable at +0x824, the +0x400 start, or the +0xc48 commit). VERIFIED that
 *    bit2 is the awaited bit and that it is un-timed; "clock locked" is INFERIDO.
 *
 *  - Run: the start/resume path writes 1 to +0x400 (0x080ae97c), which itself
 *    re-reads +0x400 expecting exactly 1, and then 0x08017378 polls +0x400 (&3)
 *    until 1 with NO timeout. A plain write latch answers both: write 1, read 1.
 *    VERIFIED both accessors; "run bit" is INFERIDO (strong).
 *
 * Everything else in the low page is a latch (writes stored, reads return the
 * stored word, zero at reset) -- the config strobes at +0x8xx/+0x9xx, +0xa8c,
 * +0xc48, and the status words we have not decoded (+0x404, +0x860, +0xb18,
 * +0x1e0) which stay zero because nothing has been measured to want otherwise.
 * The retailOS never touches +0x1000 (Rockbox's conjectured SM1_DIV), so the
 * upper window is answered as zero exactly like the unimplemented device.
 *
 * This device only ever affects the retailOS audio-clock bring-up. Rockbox never
 * reads SM1, and boot/navigation never enter the bring-up, so both are byte-for-
 * byte unchanged (the block was a zero window for them and, outside +0xc80/
 * +0xa98/+0x400/sub-state, still reads zero).
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "migration/vmstate.h"
#include "hw/core/cpu.h"
#include "exec/cpu-common.h"   /* cpu_physical_memory_write: el estado en IRAM1 */
#include "hw/irq.h"
#include "qemu/timer.h"
#include "hw/arm/ipod_classic/ipod_classic_sm1.h"
#include "hw/arm/ipod_classic/ipod_classic_i2s.h"
#include "hw/arm/ipod_classic/ipod_classic_sm1_dec.h"

/*
 * True and returns the value if `offset` is one of the eight per-channel
 * position registers (+0xa44/+0xa48 for idx 0..3, stride 0x14).
 *
 * +0xa48 is the engine's stream position, read by the audio output path
 * (getter 0x08091710, used by 0x08016e44 and 0x080b3104). We advance it from
 * the I2S transmit-sample count -- the real amount of audio clocked out -- as a
 * stereo-frame count, which is the natural "real clock" reading. MEASURED
 * caveat: on the reachable play path the copy that would
 * consume this position (0x08016eb4) is skipped by an upstream gate, so the
 * value is presently inconsequential to the audio that is produced; the true
 * wall is further upstream, at the decoder that never fills the mixer's source.
 * It is modelled anyway because it is a real register and a faked constant here
 * would be exactly the kind of thing that stalls a path we have not yet walked.
 */
static bool sm1_pos_reg(IPodClassicSM1State *s, hwaddr offset, uint32_t *out)
{
    for (int idx = 0; idx < S5L8702_SM1_POS_CHANS; idx++) {
        hwaddr a48 = S5L8702_SM1_POS_A48 + idx * S5L8702_SM1_POS_STRIDE;
        hwaddr a44 = S5L8702_SM1_POS_A44 + idx * S5L8702_SM1_POS_STRIDE;
        if (offset != a48 && offset != a44) {
            continue;
        }
        /*
         * Position in stereo frames, from the I2S transmit-sample count -- the
         * real amount of audio clocked out by the guest's own DMA.
         */
        uint64_t pos = s->i2s ? s->i2s->tx_samples / 2 : 0;
        /*
         * WRAP THE POSITION INTO THE RING. This is the fix for the wall that
         * fourteen assaults called a "structural deadlock".
         *
         * MEASURED: the render's output pointer is computed by the firmware as
         *     dest = 0x22020000 + (this position * 4)
         * i.e. IRAM1's base plus the position in stereo frames. Proof: with the
         * position forced to 0 the copy's destination is exactly 0x22020000
         * (IRAM1 base); left free-running it was 0x222dc000 and then 0x222fbc00
         * on a later run -- 0x22020000 plus the audio bytes elapsed since play
         * started.
         *
         * So the engine's position register is a RING OFFSET, not a cumulative
         * total. Reporting the raw count walked the firmware's write pointer out
         * of IRAM1 after ~1.9 s of playback; the stores then hit unmapped memory
         * and took a data abort inside the tiny silence-copy at 0x08085ea0
         * (40 bytes, 10 words), so the copy never returned -- and neither did
         * the mixer 0x0809a74c, the render 0x0808170c, or the pump 0x22006118.
         * That is what looked like a rendezvous deadlock from every angle: the
         * render was not waiting for anything, it was faulting.
         *
         * The ring is IRAM1 (0x22020000, 128 KiB), so the position wraps every
         * 128 KiB / 4 bytes-per-frame = 32768 stereo frames, which keeps every
         * write the firmware issues inside that buffer.
         *
         * SM1_POS overrides the wrap for experiments (0 = pin the position at
         * the ring base).
         */
        {
            static int pos_mode = -1;
            static uint64_t pos_wrap;
            if (pos_mode < 0) {
                const char *e = getenv("SM1_POS");
                pos_wrap = (e && *e) ? strtoull(e, NULL, 0)
                                     : S5L8702_SM1_POS_RING_FRAMES;
                pos_mode = 1;
                if (e && *e) {
                    fprintf(stderr, "SM1_POS: ring wrap overridden to %llu frames\n",
                            (unsigned long long)pos_wrap);
                }
            }
            /*
             * Progress is reported per DMA burst (1024 stereo frames = the
             * 2048 x 16-bit LLI chunk), not per sample: the guest copies whole
             * 4 KiB blocks at this offset, and a position inside a burst made a
             * block straddle the end of the ring and overrun into the slot
             * array above it.
             */
            pos &= ~(uint64_t)1023;
            pos = pos_wrap ? (pos % pos_wrap) : 0;
        }
        /* +0xa48 is the position the copy uses; +0xa44 is its companion,
         * answered 0 until measurement says otherwise. */
        *out = (offset == a48) ? (uint32_t)pos : 0;
        return true;
    }
    return false;
}

void ipod_classic_sm1_set_i2s(IPodClassicSM1State *s,
                              struct IPodClassicI2SState *i2s)
{
    s->i2s = i2s;
}

/* --------------------------------------------------------- interrupt ------ */
/*
 * The engine's "playback advanced" interrupt, VIC0 line 18.
 *
 * On real silicon the audio engine tells the CPU when it has consumed audio;
 * Apple's driver hangs its whole render pump off that report (see the
 * header). We had no line at all here, so the report could
 * never arrive. Raise it when the stream position has moved since the last
 * tick -- i.e. when audio really was clocked out -- which is the condition the
 * handler itself checks for (it reads the engine status and tests bit 2, the
 * same bit our `powered` state answers).
 *
 * Level-style pulse: raise, then drop on the next tick, so a handler that does
 * not clear anything still sees discrete events rather than a stuck line.
 * Gated by SM1_IRQ=1; with it unset nothing here runs and the machine behaves
 * exactly as the measured baseline.
 */
#define SM1_IRQ_TICK_MS 5

static uint64_t sm1_stream_pos(IPodClassicSM1State *s)
{
    /*
     * The audio the guest's own DMA clocks out: the I2S transmit-sample count
     * in stereo frames. Every position read and the slot-freeing cadence below
     * derive from this one real clock.
     */
    return s->i2s ? s->i2s->tx_samples / 2 : 0;
}

/* Drop the engine interrupt line; see the note where it is scheduled. */
static void sm1_irq_off(void *opaque)
{
    IPodClassicSM1State *s = opaque;

    if (s->irq_pending) {
        s->irq_pending = false;
        qemu_irq_lower(s->irq);
    }
}

/*
 * THE ENGINE'S MEMORY-SIDE STATE (SM1_SLOTS=1) -- DEV TOGGLE, DEFAULT OFF.
 *
 * This publishes INVENTED engine geometry (an enable mask, slot count and
 * per-slot "ready" halfwords) into IRAM1. It writes no PCM: it is not a codec
 * and it does not make audio. It exists only so the buffer-return handshake
 * can be experimented with while the real engine state is UNDETERMINED. Off by
 * default so the default build is byte-for-byte as measured.
 *
 * The render pump stops after eight invocations because 0x080a3c00 -- the
 * "give me the next render buffer" call -- returns 0 where 0x22006358 requires
 * 2. MEASURED, it returns 0 at its FIRST guard:
 *
 *     80a3c2c: tst r2, r0    ; r2 = enable mask, halfword at [table+4]
 *     80a3c30: beq salir     ; mask = 0x0000 -> give up
 *
 * where `table` is 0x0896a684 = { _, 0x2203ff28, 0x2203ff00, 0x22038000,
 * 0x22038100, idx, ... }. With `memw=` over a whole run (~180 s, boot
 * included), NO PC of the CPU ever writes 0x2203ff28, and the slot array at
 * 0x22038000 is written only once at boot. Forcing the mask by hand reaches
 * the second guard -- the per-slot "ready" halfword -- which is zero for the
 * same reason. So this is not a missing bit: it is state the ENGINE publishes
 * and our register-only model never produced.
 *
 * THE GEOMETRY IS OURS TO CHOOSE, and that is what makes this implementable
 * rather than a guess. The firmware does not carry these numbers; it reads
 * them back from what the engine published and derives everything:
 *
 *     80a3c54: ldrh r1,[desc,#4]      ; frames per slot  <- we publish
 *     80a3c58: ldrh r0,[slot,#12]     ; samples in slot  <- we publish
 *     80a3c68: add r1, pcm, r1,lsl#1  ; buffer = PCM + idx*frames*2
 *     80a3c64: lsl r0, r0, #1         ; size   = samples*2
 *
 * So any self-consistent geometry that fits below the descriptors works. We
 * take 2048 frames/slot (the audio DMA's own burst: 2048 x 16-bit, measured
 * from the self-linking LLI) and 7 slots: 7 * 4096 = 28 KiB, which ends at
 * 0x2203f100, clear of 0x2203ff00.
 *
 * What stays UNDETERMINED and is NOT modelled here: the descriptor chain the
 * firmware hands the engine at sub-block 4 +0x00 (measured 0x08ad003d = chain
 * | go, chain entry { next, ctrl, buf }, buf holding real MP3 frames). Where
 * the engine would put ITS output is programmed in no register on the whole
 * 4 KiB page -- swept, only +0x100/+0x110/+0x9xx/+0xaxx are non-zero -- so it
 * cannot be derived from anything measured. This routine deliberately models
 * only the half that CAN be grounded: the buffer-return handshake, whose
 * cadence follows the I2S the same way the position register already does.
 *
 * Gated by SM1_SLOTS so the default build stays byte-for-byte as measured.
 */
#define SM1_SLOT_DESC        0x2203ff28   /* [table+4]  engine descriptor  */
#define SM1_SLOT_ARRAY       0x22038000   /* [table+12] per-slot state     */
#define SM1_SLOT_FRAMES      2048         /* frames per slot (DMA burst)   */
#define SM1_SLOT_COUNT_MAX   7            /* 7 * 4096 = 28 KiB below 0x…ff00 */
#define SM1_SLOT_COUNT_DEFAULT 2          /* 2 does not stomp IRAM1 */

/*
 * HOW MUCH OF IRAM1 IS ACTUALLY OURS is not something the firmware tells us --
 * we publish the geometry, so we also choose how much memory it implies. Seven
 * slots claims 28 KiB from 0x22038100, and MEASURED, declaring all of it freezes
 * the user interface mid-transition into Now Playing while the audio keeps
 * playing. Publishing the state is the whole toggle; how large a ring the real
 * engine keeps is undetermined, which is why the geometry is not a default.
 *
 * SM1_NSLOTS sweeps it so the real size can be found by bisection instead of
 * guessed.
 */
/*
 * MASTER SWITCH. SM1=0 returns the block to the register-only model (including
 * no line-18 report). It is off by default, i.e. the device model is active;
 * the individual SM1_* knobs remain as overrides for experiments (SM1_SLOTS=1,
 * SM1_IRQ=0, ...). Only the state publication (SM1_SLOTS) is default-off; the
 * VIC line-18 report and the playback-position register are model
 * approximations and stay on.
 */
static bool sm1_master_off(void)
{
    static int off = -1;
    if (off < 0) {
        const char *e = getenv("SM1");
        off = (e && *e && *e == '0') ? 1 : 0;
    }
    return off == 1;
}

static unsigned sm1_slot_count(void)
{
    static int n = -1;
    if (n < 0) {
        const char *e = getenv("SM1_NSLOTS");
        n = (e && *e) ? atoi(e) : SM1_SLOT_COUNT_DEFAULT;
        if (n < 1) {
            n = 1;
        }
        if (n > SM1_SLOT_COUNT_MAX) {
            n = SM1_SLOT_COUNT_MAX;
        }
    }
    return (unsigned)n;
}
#define SM1_SLOT_COUNT       sm1_slot_count()

static bool sm1_slots_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("SM1_SLOTS");
        on = (!sm1_master_off() && e && *e && *e != '0') ? 1 : 0;
        if (on) {
            fprintf(stderr, "SM1_SLOTS: DEV TOGGLE -- publishing invented engine "
                    "geometry in IRAM1 (%d slots of %d frames); no PCM is "
                    "written\n", SM1_SLOT_COUNT, SM1_SLOT_FRAMES);
        }
    }
    return on == 1;
}

static int sm1_publish_slots(IPodClassicSM1State *s)
{
    uint16_t h;
    int freed_now = 0;

    if (!sm1_slots_on()) {
        return -1;              /* not modelling: caller keeps its own cadence */
    }

    /*
     * The descriptor: enable mask bit 0 (guard 1 asks for bit 1 instead only
     * when the stream state byte is 6, so publish both), the halfword the
     * guard requires to be zero, and the slot geometry.
     */
    h = cpu_to_le16(0x0003);
    cpu_physical_memory_write(SM1_SLOT_DESC + 0, &h, 2);
    h = cpu_to_le16(0);
    cpu_physical_memory_write(SM1_SLOT_DESC + 2, &h, 2);
    h = cpu_to_le16(SM1_SLOT_FRAMES);
    cpu_physical_memory_write(SM1_SLOT_DESC + 4, &h, 2);

    /* The slot count the firmware wraps its index against. */
    h = cpu_to_le16(SM1_SLOT_COUNT);
    cpu_physical_memory_write(SM1_SLOT_ARRAY + 2, &h, 2);

    /* Per-slot stride in halfwords, read by getdata from the slot block at +4
     * (see the decoder below): buffers are SM1_SLOT_FRAMES*2 bytes apart. */
    h = cpu_to_le16(SM1_SLOT_FRAMES);
    cpu_physical_memory_write(SM1_SLOT_ARRAY + 4, &h, 2);

    /*
     * FREEING SLOTS AT THE RATE PLAYBACK ACTUALLY CONSUMES THEM.
     *
     * The first cut re-armed all seven slots on every frame report. It broke
     * the wall -- rendret 8 -> 2505 -- but it also destroys flow control: the
     * firmware believes it has seven free buffers forever, never waits, and
     * MEASURED, its producer ran LESS (the submit gate 0x081fee60 went 8 -> 1,
     * "audio available" 7 -> 0). A ring the consumer never has to wait on is
     * not a ring.
     *
     * The right cadence is not invented either: the engine frees a slot when
     * it has PLAYED it, and how much has been played is something we already
     * know exactly -- it is the same I2S transmit count the position register
     * is derived from, just expressed in slots. So the credit ahead of the
     * play point is SM1_SLOT_COUNT slots, and each slot boundary the stream
     * crosses frees exactly one more, in ring order.
     */
    {
        static uint64_t freed;      /* one SM1 in the machine */
        static uint64_t base_pos;
        uint64_t pos = sm1_stream_pos(s);

        if (freed == 0) {
            /* Nothing played yet: the whole ring is the firmware's to fill. */
            base_pos = pos;
        }
        /*
         * A slot is SM1_SLOT_FRAMES *16-bit samples* = 4096 bytes, which at
         * stereo is 1024 FRAMES -- and `pos` counts frames, not samples. The
         * first cut divided by SM1_SLOT_FRAMES and so freed slots at half the
         * rate playback consumes them. Confirmed by the pump itself: it copies
         * r5*4 = 4096 bytes per request with r5 = 1024 (measured).
         */
        uint64_t played = (pos - base_pos) / (SM1_SLOT_FRAMES / 2);
        while (freed < played + SM1_SLOT_COUNT) {
            unsigned i = (unsigned)(freed % SM1_SLOT_COUNT);

            h = cpu_to_le16(1);
            cpu_physical_memory_write(SM1_SLOT_ARRAY + 10 + i * 4, &h, 2);
            h = cpu_to_le16(SM1_SLOT_FRAMES);
            cpu_physical_memory_write(SM1_SLOT_ARRAY + 12 + i * 4, &h, 2);
            freed++;
            freed_now++;
        }
    }
    return freed_now;
}

static int sm1trace = -1;

static bool sm1_trace_on(void)
{
    if (sm1trace < 0) {
        const char *e = getenv("SM1TRACE");
        sm1trace = (e && *e && *e != '0') ? 1 : 0;
    }
    return sm1trace == 1;
}

/*
 * FRAME DECODE (sub-block 4) -- what the engine does with a submitted frame.
 *
 * MEASURED guest protocol (retailOS 2.0.5):
 *  - the guest writes the chain pointer | 1 to +0x100 once; the chain entry
 *    (desc) is {0, ctrl, buf}: ctrl = frame_len << 16 | flags (built by
 *    0x080d463c, sub-block 4 branch), buf = the frame bytes, bit 31 set (an
 *    uncached alias of the physical address);
 *  - per frame the guest only rewrites desc[1]/buf (pcm_push 0x0809a898), then
 *    waits for the frame-done interrupt (VIC line 18) and reads the decoded PCM
 *    from the output slot ring that 0x080bdda8 walks: header 0x2203ff28
 *    {u16 enable mask, u16 0}, slot array 0x22038000 {+2 slot count, +4 stride
 *    in halfwords, +10+4*i state (1 = ready), +12+4*i halfword count},
 *    buffers 0x22038100 + i*stride*2. It sets state 0 once consumed.
 *    (The stride lives in the SLOT BLOCK at +4, not in the header: getdata
 *    reads `ldrh r1,[r1,#4]` with r1 = [table+0xc] = 0x22038000. Publishing it
 *    only to the header left stride 0, so every slot read the same buffer.)
 *  - the guest-written enable word 0x2203ff00 selects the codec: low bits 0x7
 *    is the AAC setup (0x0809b0a0) and +6 holds the MPEG-4 sampling-frequency
 *    index (the rate -> code table at 0x0809b0b0..0x0809b19c is exactly that
 *    table).
 *
 * The model consumes the chain entry on each tick when the output slot is free:
 * decode, write PCM and the slot state, zero the descriptor length (the entry is
 * consumed, as a DMA descriptor's completion write-back would), then report
 * frame-done on line 18. INFERRED (not measured on silicon): the descriptor
 * length write-back, the slot count/stride we choose (the guest reads them
 * back), and that codecs other than the AAC enable word are unsupported here.
 * SM1_DECODE=0 disables (default on when built with libavcodec).
 */
#define SM1_CTL          0x2203ff00
#define SM1_DEC_CAP      4096        /* halfwords per slot (HE-AAC frame)   */
#define SM1_FRAME_MAX    32768       /* bytes of one compressed packet (ALAC 24-bit ~8 KiB) */
#define SM1_DEC_SLOTS    2            /* power of two (the guest wraps its index) */
/*
 * ALAC: one 4096-frame packet is 8192 halfwords, and the guest counts every frame-done report as
 * one packet consumed, so the packet must leave in one slot. Two such slots would not fit IRAM1
 * below the control block at 0x2203ff00, so ALAC publishes a ring of one (a power of two).
 */
#define SM1_DEC_CAP_ALAC 8192
#define SM1_DEC_SLOTS_ALAC 1
#define SM1_SLOT_BUF     0x22038100

static bool sm1_decode_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("SM1_DECODE");
        on = (!sm1_master_off() && sm1_dec_available() &&
              !(e && *e && *e == '0')) ? 1 : 0;
        fprintf(stderr, "SM1_DECODE: %s\n", on ? "frame decoder on (libavcodec)"
                : "off (no PCM is produced)");
    }
    return on == 1;
}

static uint16_t sm1_rd16(hwaddr a)
{
    uint16_t v;
    cpu_physical_memory_read(a, &v, 2);
    return le16_to_cpu(v);
}

static void sm1_wr16(hwaddr a, uint16_t v)
{
    v = cpu_to_le16(v);
    cpu_physical_memory_write(a, &v, 2);
}

/* Consume the pending frame, if any. True when a slot was published. */
static bool sm1_decode_frame(IPodClassicSM1State *s)
{
    uint32_t chain = s->reg[S5L8702_SM1_CHAIN / 4];
    uint32_t d[3];
    uint8_t frame[SM1_FRAME_MAX];
    uint16_t ctl = sm1_rd16(SM1_CTL);
    /*
     * The enable word names the codec (measured, 2.0.5 pcm_push): AAC 0x2d2f and ALAC 0x2d3f
     * both have &7 == 7 (ALAC also sets 0x1e and fills +4/+6/+8/+10 with the magic cookie's bit
     * depth, pb, kb and mb: 24, 40, 14, 10 for an ffmpeg file; AAC leaves them zero and puts the
     * frequency index at +6); MP3 is 0x2d2d, &7 == 5, with raw frames and no parameters.
     */
    Sm1Codec codec = (ctl & 7) == 7 ? ((ctl & 0x1e) == 0x1e ? SM1_CODEC_ALAC : SM1_CODEC_AAC)
                     : (ctl & 7) == 5 ? SM1_CODEC_MP3 : SM1_CODEC_NONE;
    bool alac = codec == SM1_CODEC_ALAC;
    unsigned slots = alac ? SM1_DEC_SLOTS_ALAC : SM1_DEC_SLOTS;
    unsigned cap = alac ? SM1_DEC_CAP_ALAC : SM1_DEC_CAP;
    unsigned w;
    hwaddr arr = SM1_SLOT_ARRAY;
    int16_t pcm[S5L8702_SM1_DEC_PCM_MAX];
    uint32_t len;
    int n;

    if (!(chain & 1)) {
        return false;
    }
    cpu_physical_memory_read(chain & ~3u, d, sizeof(d));
    len = le32_to_cpu(d[1]) >> 16;
    if (sm1_trace_on()) {
        fprintf(stderr, "SM1DEC chain=0x%08x desc={%08x,%08x,%08x} len=%u "
                "slot%u state=%u ctl=%04x %04x %04x %04x %04x %04x\n", chain,
                le32_to_cpu(d[0]), le32_to_cpu(d[1]), le32_to_cpu(d[2]), len, s->dec_w % slots,
                sm1_rd16(arr + 10 + s->dec_w % slots * 4), sm1_rd16(SM1_CTL),
                sm1_rd16(SM1_CTL + 2), sm1_rd16(SM1_CTL + 4),
                sm1_rd16(SM1_CTL + 6), sm1_rd16(SM1_CTL + 8),
                sm1_rd16(SM1_CTL + 10));
        {
            uint8_t head[16];
            cpu_physical_memory_read(le32_to_cpu(d[2]) & 0x7fffffff, head,
                                     sizeof(head));
            fprintf(stderr, "SM1DEC data %02x%02x%02x%02x %02x%02x%02x%02x "
                    "%02x%02x%02x%02x %02x%02x%02x%02x\n", head[0], head[1],
                    head[2], head[3], head[4], head[5], head[6], head[7],
                    head[8], head[9], head[10], head[11], head[12], head[13],
                    head[14], head[15]);
        }
    }
    if (len == 0 || len > sizeof(frame)) {
        return false;
    }
    if (s->dec_alac != alac) {           /* codec changed: republish the slot geometry */
        s->dec_alac = alac;
        s->dec_published = false;
    }
    if (!s->dec_published) {
        for (unsigned i = 0; i < SM1_DEC_SLOTS; i++) {
            sm1_wr16(arr + 10 + i * 4, 0);
        }
        s->dec_w = 0;
        s->dec_published = true;
    }
    w = s->dec_w % slots;
    if (sm1_rd16(arr + 10 + w * 4) != 0) {
        return false;                    /* output slot still owned by the guest */
    }
    {
        cpu_physical_memory_read(le32_to_cpu(d[2]) & 0x7fffffff, frame, len);
        if (!s->dec) {
            s->dec = sm1_dec_new();
        }
        if (codec != SM1_CODEC_NONE) {
            Sm1Params p = {
                .freq_idx = sm1_rd16(SM1_CTL + 6) & 15,
                .bits = sm1_rd16(SM1_CTL + 4), .pb = sm1_rd16(SM1_CTL + 6),
                .kb = sm1_rd16(SM1_CTL + 8), .mb = sm1_rd16(SM1_CTL + 10),
            };

            n = sm1_dec_frame(s->dec, codec, &p, frame, len, pcm, cap);
        } else {
            n = -1;
            qemu_log_mask(LOG_UNIMP, "SM1: codec enable word 0x%04x not modelled\n", ctl);
        }
        if (sm1_trace_on()) {
            fprintf(stderr, "SM1DEC frame len=%u -> n=%d slot%u data %02x%02x%02x%02x%02x%02x\n",
                    len, n, w, frame[0], frame[1], frame[2], frame[3], frame[4], frame[5]);
        }
        if (n <= 0) {                    /* no output / bad frame: a silent slot */
            n = 2048;
            memset(pcm, 0, n * 2);
        }
    }
    {   /* SM1_DEC_DUMP=file: dev aid, appends each decoded frame (s16le) */
        static FILE *dump;
        static int tried;
        if (!tried++ && getenv("SM1_DEC_DUMP")) {
            dump = fopen(getenv("SM1_DEC_DUMP"), "wb");
        }
        if (dump) {
            fwrite(pcm, 2, n, dump);
            fflush(dump);
        }
        if (dump && getenv("SM1_DEC_DUMP_FRAMES")) {
            static FILE *fd;
            if (!fd) {
                fd = fopen(getenv("SM1_DEC_DUMP_FRAMES"), "wb");
            }
            fwrite(&len, 4, 1, fd);
            fwrite(frame, 1, len, fd);
            fflush(fd);
        }
    }
    cpu_physical_memory_write(SM1_SLOT_BUF + w * cap * 2, pcm, n * 2);
    sm1_wr16(SM1_SLOT_DESC + 0, 3);
    sm1_wr16(SM1_SLOT_DESC + 2, 0);
    sm1_wr16(SM1_SLOT_DESC + 4, cap);
    sm1_wr16(arr + 2, slots);
    /*
     * The per-slot stride getdata (0x080bdda8) actually reads lives in the
     * slot block at +4, NOT in the header at 0x2203ff2c: getdata computes
     *     ptr = 0x22038100 + idx * u16[0x22038004] * 2
     * (ldrh r1,[r1,#4] at 0x080bddfc with r1 = [table+0xc] = 0x22038000).
     * Leaving it zero collapsed every idx onto 0x22038100, so the guest read
     * slot0's PCM twice and never consumed slot1's -- the even-frame-doubled /
     * odd-frame-dropped stutter. Publish the same
     * stride the buffers are written with (SM1_DEC_CAP halfwords).
     */
    sm1_wr16(arr + 4, cap);
    sm1_wr16(arr + 12 + w * 4, n);
    sm1_wr16(arr + 10 + w * 4, 1);
    d[1] = cpu_to_le32(le32_to_cpu(d[1]) & 0xffff);   /* descriptor consumed */
    cpu_physical_memory_write((chain & ~3u) + 4, &d[1], 4);
    s->dec_w = (w + 1) % slots;
    return true;
}

static void sm1_irq_tick(void *opaque)
{
    IPodClassicSM1State *s = opaque;
    uint64_t pos = sm1_stream_pos(s);

    /*
     * The trigger is the guest's own arming write to +0xa8c (see the write
     * handler). Earlier attempts gated on our derived `powered` flag (never set
     * on the play path) or on the stream position advancing (circular: the
     * position cannot move until the render this interrupt unblocks has run).
     * Both measured raised=0. Arming is the firmware's request, so honouring it
     * is what the hardware would do.
     *
     * Cadence: the audio DMA delivers ~43 bursts/s (2048 stereo frames each,
     * measured), so a frame-done report every ~25 ms is the right order. We tick
     * at 5 ms and fire every fifth tick.
     *
     * Shape: assert with the status bit set, then drop both on the next tick, so
     * the handler sees a discrete event and a missed one cannot wedge the line.
     */
    s->ticks++;
    if (sm1_decode_on() && (s->reg[S5L8702_SM1_CHAIN / 4] & 1)) {
        /* Engine running a chain: frame-done is reported per decoded frame. */
        if (s->irq_armed && !s->irq_pending && sm1_decode_frame(s)) {
            s->irq_pending = true;
            s->last_pos = pos;
            s->irqs++;
            s->dec_irq_level = true;     /* held until the guest acknowledges */
            qemu_irq_raise(s->irq);
        }
        timer_mod(s->irq_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  (int64_t)SM1_IRQ_TICK_MS * 1000000);
        return;
    }
    /*
     * SM1_IRQ_EVERY overrides how many 5 ms ticks pass between reports.
     *
     * Needed to test a specific failure: the audio task's loop at 0x22006324
     * calls the event waiter, and when the waiter returns failure it sleeps and
     * retries -- but only TWICE (`cmp r7,#1 ; bgt exit`) before giving up. The
     * waiter's own wait carries a timeout of 500. So if a frame report does not
     * arrive inside that timeout the whole audio pump bails after two attempts,
     * which is exactly the eight invocations measured. Making the report more
     * frequent says whether the cadence is what misses the timeout.
     */
    static int every = -1;
    if (every < 0) {
        const char *e = getenv("SM1_IRQ_EVERY");
        every = (e && *e) ? atoi(e) : 5;
        if (every < 1) {
            every = 1;
        }
    }
    if (!s->irq_pending && (s->irq_armed || s->irq_always) && (s->ticks % every == 0)) {
        /*
         * Publish the memory-side state BEFORE signalling: the frame report is
         * what makes the firmware come and read it, so it has to already be
         * there when the handler runs.
         *
         * And when we ARE modelling it, the report has to MEAN what its name
         * says. MEASURED: waking the pump on the bare 25 ms timer, whether or
         * not a slot had actually come free, let it run 16 times and then give
         * up -- it asks, misses, and 0x22006358 abandons after two retries
         * rather than waiting. So with SM1_SLOTS on, a frame report is raised
         * only when a frame really completed. Returns -1 when not modelling,
         * which keeps the default build's cadence byte-for-byte.
         */
        int freed = sm1_publish_slots(s);
        if (freed == 0) {
            timer_mod(s->irq_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                      (int64_t)SM1_IRQ_TICK_MS * 1000000);
            s->irq_pending = false;
            return;
        }
        s->irq_pending = true;
        s->last_pos = pos;
        s->irqs++;
        qemu_irq_raise(s->irq);
        /*
         * The line is a LEVEL, but a short one.
         *
         * Holding it until the next 5 ms tick produced the storm in s11.41:
         * 4 raises, 789 182 handler entries even after any block read became an
         * acknowledge. That is ~197 000 re-entries inside one 5 ms window, which
         * is only possible because virtual time barely advances while the CPU
         * spins in the interrupt loop -- so a coarse timer is exactly the wrong
         * thing to be holding an interrupt line with.
         *
         * MEASURED, a bare pulse does not work: with SM1_IRQ_PULSE=1 (raise and
         * lower in the same instant) the handler count went to ZERO and so did
         * creditsig. The VIC wants a level; an edge is lost. So the line has to
         * stay up long enough to be dispatched and no longer.
         *
         * SM1_IRQ_ASSERT_US is that window, and it is a MODEL CHOICE, not a fact
         * about the part: what the real engine's line does is UNDETERMINED, and
         * what the firmware touches to acknowledge it has not been found -- the
         * handler's status getter at 0x080a7638 does not appear to reach this
         * block, or broadening the read acknowledge would have ended the storm
         * instead of trimming it by a third.
         */
        timer_mod(s->irq_off_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  (int64_t)S5L8702_SM1_IRQ_ASSERT_US * 1000);
        if (s->irqs <= 4 || s->irqs % 2000 == 0) {
            fprintf(stderr, "SM1_IRQ: line 18 raised #%" PRIu64
                    " (armed=%d powered=%d pos=%" PRIu64 ")\n",
                    s->irqs, (int)s->irq_armed, (int)s->powered, pos);
        }
    }
    timer_mod(s->irq_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              (int64_t)SM1_IRQ_TICK_MS * 1000000);
}

/*
 * SM1TRACE=1 prints every access with its value and the guest PC, the same
 * pattern as I2STRACE/ATATRACE/DISPTRACE. SM1 is touched only during the audio
 * bring-up (a few dozen accesses), so this is cheap and stays in as the way to
 * watch the handshake -- e.g. to confirm which bring-up path (0x0808170c codec
 * bring-up vs 0x08017378 start/resume) runs and in what order.
 */

/*
 * SM1HI=1: report writes ABOVE the modelled 4 KB register page.
 *
 * Its own variable on purpose. The question -- does the firmware put anything
 * in the other 1020 KB of this block's window? -- needs a handful of lines,
 * while SM1TRACE prints one line per register access and produced a log big
 * enough to take the whole WSL filesystem down with it. Cheap when unset.
 */
static int sm1hi = -1;

static bool sm1_hi_on(void)
{
    if (sm1hi < 0) {
        const char *e = getenv("SM1HI");
        sm1hi = (e && *e && *e != '0') ? 1 : 0;
    }
    return sm1hi == 1;
}

static uint32_t sm1_trace_pc(void)
{
    CPUState *cs = current_cpu;
    return cs ? (uint32_t)CPU_GET_CLASS(cs)->get_pc(cs) : 0;
}

/*
 * The eight sub-block "state" registers (sub+0x14). Their absolute offsets are
 * the sub-block bases {0,0x20,0x40,0x60,0x100,0x180,0x200,0x230} plus 0x14. Each
 * one's command register (sub+0x10) sits exactly 4 bytes below it. We enumerate
 * them rather than test "offset ends in 4" so we never mistake an unrelated
 * register (e.g. +0x404, whose 4-below neighbour is the +0x400 run register) for
 * a sub-block state.
 */
static const hwaddr sm1_sub_state_off[] = {
    0x0014, 0x0034, 0x0054, 0x0074,   /* idx 0..3: base + idx*0x20 + 0x14 */
    0x0114, 0x0194, 0x0214, 0x0244,   /* idx 4..7                          */
};

static bool sm1_is_sub_state(hwaddr offset)
{
    for (size_t i = 0; i < ARRAY_SIZE(sm1_sub_state_off); i++) {
        if (offset == sm1_sub_state_off[i]) {
            return true;
        }
    }
    return false;
}

static bool sm1_is_sub_cmd(hwaddr offset)
{
    /* A command register is the state register minus 4. */
    for (size_t i = 0; i < ARRAY_SIZE(sm1_sub_state_off); i++) {
        if (offset == sm1_sub_state_off[i] - 4) {
            return true;
        }
    }
    return false;
}

static uint64_t ipod_classic_sm1_read(void *opaque, hwaddr offset,
                                      unsigned size)
{
    IPodClassicSM1State *s = opaque;
    uint32_t val;
    bool was_pending = s->irq_pending;

    /*
     * ANY read of this block acknowledges the engine interrupt.
     *
     * It used to be only +0xa98, on the reasoning that the line-18 handler's
     * status getter reads exactly that. MEASURED, that is not enough: in one run
     * the model raised the line FOUR times and the handler at 0x08017378 was
     * entered 1,196,485 times -- and QEMU's own -d int log counted ~1.05 million
     * IRQ exceptions. The line stays asserted for the whole 5 ms until the next
     * tick drops it, and the guest re-enters the handler for that entire window,
     * which starves every audio thread. That is why the decode side crawled:
     * dpump ran 8 times and creditsig 493 while the handler ran 1.2 million.
     *
     * Whatever the getter at 0x080a7638 actually touches, the handler does reach
     * this block, so taking any read as the acknowledge cannot leave the level
     * wedged. The STAT value below still reports bit 2 from the pre-clear state,
     * so the handler that triggered on this interrupt still sees it set and
     * signals its event.
     */
    if (was_pending && !s->dec_irq_level) {
        s->irq_pending = false;
        qemu_irq_lower(s->irq);
    }

    /* Above the modelled low page: a plain zero window, as before. */
    if (offset >= S5L8702_SM1_REG_BYTES) {
        return 0;
    }

    switch (offset) {
    case S5L8702_SM1_ID:
        /* Block identity. A mismatch here is an eternal `b .`. */
        val = S5L8702_SM1_ID_MAGIC;
        break;

    case S5L8702_SM1_STAT:
        /*
         * Clock/engine status, read by getter 0x080a7638 (r1=0 -> [base+0xa98],
         * top five bits masked off by the firmware itself).
         *
         * Bit 2 serves two readers:
         *  - the bring-up spin at 0x08081af8 ("clock ready"), answered from
         *    `powered` as before; and
         *  - the VIC line-18 handler 0x08017378, which does `tst r0,#4` and only
         *    signals the render-credit event 0x22008c58 when the bit is set. So
         *    while we are asserting the engine interrupt the bit must read set,
         *    or the handler would take its early exit and signal nothing.
         */
        val = (s->powered || was_pending) ? S5L8702_SM1_STAT_READY : 0;
        /*
         * SM1_FRAME_BIT=1 also reports bit 12 while a frame report is pending.
         *
         * The waiter on the render event (0x0808af58) reads this same word and
         * does `tst r0,#0x1000` before doing its per-frame work, so with only
         * bit 2 answered that branch is never taken -- which looked like the
         * missing piece. MEASURED, it is not: with the bit set, dpump went 16 ->
         * 14 and aacmath stayed at 1. So bit 12 means something else, and it is
         * left OFF by default rather than inventing a status the part may never
         * report. Kept as a switch because the finding -- that this waiter gates
         * on bit 12 while the interrupt handler gates on bit 2 -- is real and
         * worth being able to re-test.
         */
        if (was_pending && getenv("SM1_FRAME_BIT")) {
            val |= S5L8702_SM1_STAT_FRAME;
        }
        /*
         * The acknowledge itself happens at the top of this function, for ANY
         * offset -- see the note there for why narrowing it to this register was
         * measured insufficient. `was_pending` is the pre-clear state, so a
         * handler that got here because of this interrupt still reads bit 2 set.
         */
        break;

    case S5L8702_SM1_RUN:
        /*
         * Run/run-state. The start path writes 1 here and both it and the
         * writer's own retry loop read the value straight back, so return the
         * latched word (write 1 -> read 1). The &3 the poll applies is the
         * firmware's, not ours.
         */
        val = s->reg[S5L8702_SM1_RUN / 4];
        break;

    default:
        if (sm1_is_sub_state(offset)) {
            /*
             * Sub-block state (sub+0x14): a STATE MACHINE, not a sticky latch.
             *
             * It reports "reached target state" (7) while the channel is
             * commanded up or running, and returns to 0 (idle) once it has been
             * commanded to stop. Two firmware sites read it and they want
             * OPPOSITE values, which is what forces the distinction:
             *
             *  - The power-up 0x080ba768 writes cmd 4, polls for 7 up to ten
             *    times, writes cmd 16 (run) -- and finally writes cmd 96 (stop).
             *  - The render 0x0808170c then reads sub-block 4's state at
             *    0x080820f0 and REQUIRES 0:
             *        80820f0: bl   0x80b8470     ; read sub+0x14, & 7
             *        80820f4: cmp  r0, #0
             *        80820f8: bne  0x80821a8     ; non-zero -> SKIP the build
             *    The block it skips, 0x08082100-0x080821a0, is the engine
             *    CONTEXT CONSTRUCTION: the buffer copy, the descriptor
             *    allocation (0x080b67a8), the chain append (0x080bfa58) and the
             *    address translation (0x08075d80).
             *
             * Answering a sticky 7 satisfied the power-up poll but made the
             * render skip its own context build forever -- which is exactly the
             * "missing render-context construction step" the audio work was
             * looking for, and why every gate forced downstream revealed the
             * layer below it was also unbuilt.
             *
             * So: 7 while the last command is up (4) or run (16); 0 once the
             * channel has been stopped (96) or never commanded.
             */
            uint32_t cmd = s->reg[(offset - 4) / 4];
            switch (cmd) {
            case S5L8702_SM1_SUB_CMD_UP:
            case S5L8702_SM1_SUB_CMD_RUN:
                val = S5L8702_SM1_SUB_STATE_READY;
                break;
            default:            /* 0 = never commanded, 96 = stopped */
                val = 0;
                break;
            }
        } else if (sm1_pos_reg(s, offset, &val)) {
            /* Per-channel playback position; advances with I2S playback. */
        } else {
            /* Latch: config strobes and undecoded status words read back as
             * stored (zero at reset). */
            val = s->reg[offset / 4];
        }
        break;
    }

    if (sm1_trace_on()) {
        fprintf(stderr, "SM1TRACE rd +0x%03" HWADDR_PRIx " -> 0x%08x  "
                "pc~0x%08x%s\n", offset, val, sm1_trace_pc(),
                offset == S5L8702_SM1_STAT ? "  [STAT bit2=ready]" :
                offset == S5L8702_SM1_ID ? "  [ID]" :
                offset == S5L8702_SM1_RUN ? "  [RUN]" :
                sm1_is_sub_state(offset) ? "  [SUBSTATE]" : "");
    }
    return val;
}

static void ipod_classic_sm1_write(void *opaque, hwaddr offset,
                                   uint64_t value, unsigned size)
{
    IPodClassicSM1State *s = opaque;

    if (offset >= S5L8702_SM1_REG_BYTES) {
        /*
         * A BLIND SPOT worth naming. The block's window is 1 MB and only its
         * first 4 KB is modelled; everything above was dropped here silently,
         * BEFORE the trace below, so SM1TRACE could never show it either. If
         * the firmware puts descriptors -- or DSP code -- up there, the whole
         * lot has been going into a hole and no measurement in this
         * investigation would have noticed.
         *
         * Counting is enough to answer that, and costs nothing when the trace
         * is off. Doubling schedule so a bulk upload is loud and a stray write
         * stays quiet.
         */
        if (sm1_hi_on()) {
            static uint64_t hi_writes;
            static hwaddr hi_lo = (hwaddr)-1, hi_hi;
            uint64_t n = ++hi_writes;

            if (offset < hi_lo) {
                hi_lo = offset;
            }
            if (offset > hi_hi) {
                hi_hi = offset;
            }
            if ((n & (n - 1)) == 0 || n % 100000 == 0) {
                fprintf(stderr, "SM1TRACE HI-WRITE #%" PRIu64 " +0x%05"
                        HWADDR_PRIx " = 0x%08x  (rango visto +0x%05" HWADDR_PRIx
                        "..+0x%05" HWADDR_PRIx ")  pc~0x%08x\n",
                        n, offset, (uint32_t)value, hi_lo, hi_hi,
                        sm1_trace_pc());
            }
        }
        return;
    }

    /*
     * The engine is "powered" (clock will lock, run bit will follow) once the
     * firmware issues any of its bring-up writes. This is the single derived
     * signal behind +0xa98 bit2; deriving it from the guest's own first move
     * keeps the model coherent instead of hard-wiring the bit on. VERIFIED that
     * each of these writes is part of a bring-up; that they are what "powers"
     * the clock is INFERIDO.
     */
    switch (offset) {
    case S5L8702_SM1_CODEC_EN:   /* +0x824 codec enable   */
    case S5L8702_SM1_COMMIT:     /* +0xc48 config commit  */
        s->powered = true;
        /*
         * Also the status acknowledge: the guest writes the status bit it has
         * handled (4 = frame done) here (0x080c1860 after polling +0xa98, and
         * per frame at 0x080c1878). INFERRED write-1-to-clear. A frame-done line
         * raised by the decoder stays asserted until this write.
         */
        if (s->dec_irq_level && (value & S5L8702_SM1_STAT_READY)) {
            s->dec_irq_level = false;
            s->irq_pending = false;
            qemu_irq_lower(s->irq);
        }
        break;
    case S5L8702_SM1_RUN:        /* +0x400 start (nonzero) */
        if (value & S5L8702_SM1_RUN_MASK) {
            s->powered = true;
        }
        break;
    case S5L8702_SM1_INTEN:      /* +0xa8c interrupt enable */
        /*
         * The engine's interrupt enable. The firmware arms it from a four-
         * instruction leaf that writes the bare constant:
         *
         *   08017364: ldr r1, =0x38500000 ; mov r0, #4 ; str r0, [r1, #0xa8c]
         *
         * called at 0x080820e4 from the render's own path. Bit 2 here pairs with
         * bit 2 of the status at +0xa98, which the VIC line-18 handler
         * (0x08017378) tests before signalling the render-credit event
         * 0x22008c58. In other words the firmware is asking the engine to
         * interrupt it when a frame is done -- and then waits for exactly that.
         *
         * We used to keep +0xa8c as a plain latch and had no interrupt line at
         * all, so the request could never be answered: the render pump's 500 ms
         * wait (0x0808af58) always timed out and tore the codec down. Arming
         * here makes the trigger the guest's own, rather than a timer we chose.
         */
        if (value & S5L8702_SM1_INTEN_FRAME) {
            if (!s->irq_armed) {
                s->irq_armed = true;
                fprintf(stderr, "SM1_IRQ: guest armed the engine interrupt "
                        "(+0xa8c = 0x%02x); VIC line 18 live\n",
                        (uint32_t)value);
            }
        } else {
            s->irq_armed = false;
        }
        break;
    default:
        if (sm1_is_sub_cmd(offset) && value != 0) {
            s->powered = true;   /* a channel was commanded up */
        }
        break;
    }

    s->reg[offset / 4] = (uint32_t)value;

    if (sm1_trace_on()) {
        fprintf(stderr, "SM1TRACE wr +0x%03" HWADDR_PRIx " = 0x%08x  "
                "pc~0x%08x%s%s\n", offset, (uint32_t)value, sm1_trace_pc(),
                sm1_is_sub_cmd(offset) ? "  [SUBCMD]" : "",
                s->powered ? "  powered" : "");
    }
}

static const MemoryRegionOps ipod_classic_sm1_ops = {
    .read = ipod_classic_sm1_read,
    .write = ipod_classic_sm1_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void ipod_classic_sm1_reset(DeviceState *dev)
{
    IPodClassicSM1State *s = IPOD_CLASSIC_SM1(dev);

    memset(s->reg, 0, sizeof(s->reg));
    s->powered = false;
    s->dec_w = 0;
    s->dec_published = false;
    s->dec_irq_level = false;
}

static void ipod_classic_sm1_init(Object *obj)
{
    IPodClassicSM1State *s = IPOD_CLASSIC_SM1(obj);

    memory_region_init_io(&s->iomem, obj, &ipod_classic_sm1_ops, s,
                          TYPE_IPOD_CLASSIC_SM1, S5L8702_SM1_MEM_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);

    /*
     * The engine's interrupt line. Always created so the machine can wire it to
     * VIC0 line 18; raised on the guest's arming write unless SM1_IRQ=0.
     */
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);

    const char *e = getenv("SM1_IRQ");
    s->irq_enabled = !sm1_master_off() && !(e && *e && *e == '0');
    s->irq_always = e && *e == '2';
    /*
     * Always allocate the timers so they can be described by VMSTATE_TIMER_PTR
     * regardless of the SM1_IRQ setting; only arm them when the interrupt is
     * enabled. A timer that is never armed has no effect.
     */
    s->irq_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, sm1_irq_tick, s);
    s->irq_off_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, sm1_irq_off, s);
    if (s->irq_enabled) {
        timer_mod(s->irq_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  (int64_t)SM1_IRQ_TICK_MS * 1000000);
        fprintf(stderr, "SM1_IRQ: engine interrupt armed on VIC0 line 18\n");
    }
}

static int ipod_classic_sm1_post_load(void *opaque, int version_id)
{
    IPodClassicSM1State *s = opaque;

    /* The engine IRQ is a sysbus output; re-drive it from the loaded state. */
    if (s->irq_pending) {
        qemu_irq_raise(s->irq);
    } else {
        qemu_irq_lower(s->irq);
    }
    return 0;
}

static const VMStateDescription vmstate_ipod_classic_sm1 = {
    .name = TYPE_IPOD_CLASSIC_SM1,
    .version_id = 2,
    .minimum_version_id = 1,
    .post_load = ipod_classic_sm1_post_load,
    .fields = (VMStateField[]) {
        VMSTATE_UINT32_ARRAY(reg, IPodClassicSM1State, S5L8702_SM1_REG_WORDS),
        VMSTATE_BOOL(powered, IPodClassicSM1State),
        VMSTATE_BOOL(irq_enabled, IPodClassicSM1State),
        VMSTATE_BOOL(irq_always, IPodClassicSM1State),
        VMSTATE_BOOL(irq_armed, IPodClassicSM1State),
        VMSTATE_BOOL(irq_pending, IPodClassicSM1State),
        VMSTATE_UINT64(last_pos, IPodClassicSM1State),
        VMSTATE_UINT64(irqs, IPodClassicSM1State),
        VMSTATE_UINT64(ticks, IPodClassicSM1State),
        VMSTATE_UINT8_V(dec_w, IPodClassicSM1State, 2),
        VMSTATE_BOOL_V(dec_published, IPodClassicSM1State, 2),
        VMSTATE_BOOL_V(dec_irq_level, IPodClassicSM1State, 2),
        VMSTATE_TIMER_PTR(irq_timer, IPodClassicSM1State),
        VMSTATE_TIMER_PTR(irq_off_timer, IPodClassicSM1State),
        VMSTATE_END_OF_LIST()
    },
};

static void ipod_classic_sm1_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->desc = "S5L8702 SM1 audio clock/stream engine";
    dc->reset = ipod_classic_sm1_reset;
    dc->vmsd = &vmstate_ipod_classic_sm1;
    dc->user_creatable = false;
}

static const TypeInfo ipod_classic_sm1_info = {
    .name          = TYPE_IPOD_CLASSIC_SM1,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IPodClassicSM1State),
    .instance_init = ipod_classic_sm1_init,
    .class_init    = ipod_classic_sm1_class_init,
};

static void ipod_classic_sm1_register_types(void)
{
    type_register_static(&ipod_classic_sm1_info);
}

type_init(ipod_classic_sm1_register_types)
