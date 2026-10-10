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
    int freq_idx, chans;
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
    d->freq_idx = freq_idx;
    d->chans = chans;
    return true;
}

static int16_t to_s16(float v)
{
    int x = (int)lrintf(v * 32768.0f);
    return x > 32767 ? 32767 : x < -32768 ? -32768 : x;
}

int sm1_dec_frame(Sm1Dec *d, Sm1Codec codec, int aac_freq_idx,
                  const uint8_t *frame, int len, int16_t *pcm, int cap)
{
    /* The first syntactic element tells mono (SCE) from stereo (CPE). */
    int chans = (len > 0 && (frame[0] >> 5) == 0) ? 1 : 2;

    if (codec != SM1_CODEC_AAC || len <= 0) {
        return -1;
    }
    if (!d->ctx || d->freq_idx != aac_freq_idx || d->chans != chans) {
        if (!dec_open_aac(d, aac_freq_idx, chans)) {
            return -1;
        }
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
    if (2 * n > cap || ch < 1 || d->frm->format != AV_SAMPLE_FMT_FLTP) {
        av_frame_unref(d->frm);
        return -1;
    }
    for (int i = 0; i < n; i++) {
        float l = ((float *)d->frm->extended_data[0])[i];
        float rr = ch > 1 ? ((float *)d->frm->extended_data[1])[i] : l;
        pcm[2 * i] = to_s16(l);
        pcm[2 * i + 1] = to_s16(rr);
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
int sm1_dec_frame(Sm1Dec *d, Sm1Codec c, int f, const uint8_t *fr, int l,
                  int16_t *p, int cap) { return -1; }
#endif
