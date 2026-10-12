/* See ipod_classic_sm1_dec.h. */
#include "qemu/osdep.h"
#include "hw/arm/ipod_classic/ipod_classic_sm1_dec.h"

#ifdef CONFIG_SM1_DECODER
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>

struct Sm1Dec {
    AVCodecContext *ctx;
    AVPacket *pkt;
    AVFrame *frm;
    Sm1Codec codec;
    Sm1Params params;
    int chans;
};

bool sm1_dec_available(void) { return true; }

Sm1Dec *sm1_dec_new(void)
{
    Sm1Dec *d = g_new0(Sm1Dec, 1);
    d->pkt = av_packet_alloc();
    d->frm = av_frame_alloc();
    return d;
}

static void dec_close(Sm1Dec *d)
{
    avcodec_free_context(&d->ctx);
}

void sm1_dec_free(Sm1Dec *d)
{
    if (!d) {
        return;
    }
    dec_close(d);
    av_packet_free(&d->pkt);
    av_frame_free(&d->frm);
    g_free(d);
}

void sm1_dec_flush(Sm1Dec *d)
{
    if (d->ctx) {
        avcodec_flush_buffers(d->ctx);
    }
}

/* Raw AAC-LC needs an AudioSpecificConfig: object 2, rate index, channels. */
static bool dec_open_aac(Sm1Dec *d, int freq_idx, int chans)
{
    const AVCodec *c = avcodec_find_decoder(AV_CODEC_ID_AAC);

    dec_close(d);
    if (!c) {
        return false;
    }
    d->ctx = avcodec_alloc_context3(c);
    d->ctx->extradata = av_mallocz(2 + AV_INPUT_BUFFER_PADDING_SIZE);
    d->ctx->extradata_size = 2;
    d->ctx->extradata[0] = (2 << 3) | (freq_idx >> 1);
    d->ctx->extradata[1] = ((freq_idx & 1) << 7) | (chans << 3);
    if (avcodec_open2(d->ctx, c, NULL) < 0) {
        dec_close(d);
        return false;
    }
    d->codec = SM1_CODEC_AAC;
    d->params.freq_idx = freq_idx;
    d->chans = chans;
    return true;
}


/*
 * ALAC needs the 36-byte magic cookie that the container's 'alac' atom carries; the guest
 * hands the engine its depth and Rice parameters instead (bit depth, pb, kb, mb in the control
 * block). The frame length is the format's fixed 4096 and the rate does not matter to the
 * decoder: the I2S clock sets the playback rate.
 */
static bool dec_open_alac(Sm1Dec *d, const Sm1Params *p)
{
    const AVCodec *c = avcodec_find_decoder(AV_CODEC_ID_ALAC);
    uint8_t cookie[36] = {
        0, 0, 0, 36, 'a', 'l', 'a', 'c', 0, 0, 0, 0,
        0, 0, 0x10, 0,              /* frameLength 4096 */
        0, p->bits, p->pb, p->mb, p->kb, 2,
        0, 255,                     /* maxRun */
        0, 0, 0, 0, 0, 0, 0, 0,     /* maxFrameBytes, avgBitRate */
        0, 0, 0xac, 0x44,           /* sampleRate 44100 */
    };

    dec_close(d);
    if (!c) {
        return false;
    }
    d->ctx = avcodec_alloc_context3(c);
    d->ctx->extradata = av_mallocz(sizeof(cookie) + AV_INPUT_BUFFER_PADDING_SIZE);
    memcpy(d->ctx->extradata, cookie, sizeof(cookie));
    d->ctx->extradata_size = sizeof(cookie);
    if (avcodec_open2(d->ctx, c, NULL) < 0) {
        dec_close(d);
        return false;
    }
    d->codec = SM1_CODEC_ALAC;
    d->params = *p;
    d->chans = 2;
    return true;
}

/* MP3 frames are self-describing: no extradata. */
static bool dec_open_mp3(Sm1Dec *d)
{
    const AVCodec *c = avcodec_find_decoder(AV_CODEC_ID_MP3);

    dec_close(d);
    if (!c) {
        return false;
    }
    d->ctx = avcodec_alloc_context3(c);
    if (avcodec_open2(d->ctx, c, NULL) < 0) {
        dec_close(d);
        return false;
    }
    d->codec = SM1_CODEC_MP3;
    return true;
}

static int16_t to_s16(float v)
{
    int x = (int)lrintf(v * 32768.0f);
    return x > 32767 ? 32767 : x < -32768 ? -32768 : x;
}

/* One sample of plane `ch` as s16, whatever the decoder's sample format. */
static int16_t sample_s16(const AVFrame *f, int ch, int i)
{
    switch (f->format) {
    case AV_SAMPLE_FMT_FLTP:
        return to_s16(((float *)f->extended_data[ch])[i]);
    case AV_SAMPLE_FMT_S16P:
        return ((int16_t *)f->extended_data[ch])[i];
    case AV_SAMPLE_FMT_S32P: {   /* left-justified: keep the top 16 bits, rounded */
        int64_t x = ((int32_t *)f->extended_data[ch])[i];
        x = (x + 0x8000) >> 16;
        return x > 32767 ? 32767 : x;
    }
    default:
        return 0;
    }
}

static bool dec_supported(const AVFrame *f)
{
    return f->format == AV_SAMPLE_FMT_FLTP || f->format == AV_SAMPLE_FMT_S16P ||
           f->format == AV_SAMPLE_FMT_S32P;
}

int sm1_dec_frame(Sm1Dec *d, Sm1Codec codec, const Sm1Params *p,
                  const uint8_t *frame, int len, int16_t *pcm, int cap)
{
    /* The first syntactic element tells mono (SCE) from stereo (CPE); ALAC is always stereo here. */
    int chans = codec == SM1_CODEC_ALAC || (len > 0 && (frame[0] >> 5) != 0) ? 2 : 1;

    if (len <= 0 || codec == SM1_CODEC_NONE) {
        return -1;
    }
    if (codec == SM1_CODEC_AAC &&
        (!d->ctx || d->codec != codec || d->params.freq_idx != p->freq_idx || d->chans != chans)) {
        if (!dec_open_aac(d, p->freq_idx, chans)) {
            return -1;
        }
    }
    if (codec == SM1_CODEC_ALAC &&
        (!d->ctx || d->codec != codec || memcmp(&d->params, p, sizeof(*p)))) {
        if (!dec_open_alac(d, p)) {
            return -1;
        }
    }
    if (codec == SM1_CODEC_MP3 && (!d->ctx || d->codec != codec) && !dec_open_mp3(d)) {
        return -1;
    }
    if (av_new_packet(d->pkt, len) < 0) {
        return -1;
    }
    memcpy(d->pkt->data, frame, len);
    int r = avcodec_send_packet(d->ctx, d->pkt);
    av_packet_unref(d->pkt);
    if (r < 0 || avcodec_receive_frame(d->ctx, d->frm) < 0) {
        return r < 0 ? -1 : 0;
    }
    int n = d->frm->nb_samples, ch = d->frm->ch_layout.nb_channels;
    if (2 * n > cap || ch < 1 || !dec_supported(d->frm)) {
        av_frame_unref(d->frm);
        return -1;
    }
    for (int i = 0; i < n; i++) {
        pcm[2 * i] = sample_s16(d->frm, 0, i);
        pcm[2 * i + 1] = sample_s16(d->frm, ch > 1 ? 1 : 0, i);
    }
    av_frame_unref(d->frm);
    return 2 * n;
}
#else
struct Sm1Dec { int unused; };
bool sm1_dec_available(void) { return false; }
Sm1Dec *sm1_dec_new(void) { return NULL; }
void sm1_dec_free(Sm1Dec *d) { }
void sm1_dec_flush(Sm1Dec *d) { }
int sm1_dec_frame(Sm1Dec *d, Sm1Codec c, const Sm1Params *pr, const uint8_t *fr, int l,
                  int16_t *p, int cap) { return -1; }
#endif
