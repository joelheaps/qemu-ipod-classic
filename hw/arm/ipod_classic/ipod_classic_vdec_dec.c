/* See ipod_classic_vdec_dec.h. */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/arm/ipod_classic/ipod_classic_vdec_dec.h"

#ifdef CONFIG_SM1_DECODER
#include <libavcodec/avcodec.h>

struct VdecDec {
    AVCodecContext *ctx;
    AVPacket *pkt;
    AVFrame *frm;
    uint8_t *extradata;      /* last config blob, to detect a change */
    int extradata_len;
};

bool vdec_dec_available(void) { return true; }

VdecDec *vdec_dec_new(void)
{
    VdecDec *d = g_new0(VdecDec, 1);
    d->pkt = av_packet_alloc();
    d->frm = av_frame_alloc();
    return d;
}

void vdec_dec_free(VdecDec *d)
{
    if (!d) {
        return;
    }
    avcodec_free_context(&d->ctx);
    g_free(d->extradata);
    av_packet_free(&d->pkt);
    av_frame_free(&d->frm);
    g_free(d);
}

void vdec_dec_flush(VdecDec *d)
{
    if (d->ctx) {
        avcodec_flush_buffers(d->ctx);
    }
}

/*
 * The codec-config blob the guest parsed is the MP4 avcC record, possibly with
 * the 8-byte box header (size + "avcC") still in front of it. libavcodec's
 * H.264 decoder recognises an AVCDecoderConfigurationRecord by its leading
 * byte 0x01; strip the box header if it is there, otherwise hand it over as is
 * (Annex-B SPS/PPS also works).
 */
static const uint8_t *vdec_cfg_normalise(const uint8_t *cfg, int len, int *out)
{
    if (len >= 8 && cfg[4] == 'a' && cfg[5] == 'v' &&
        cfg[6] == 'c' && cfg[7] == 'C') {
        *out = len - 8;
        return cfg + 8;
    }
    *out = len;
    return cfg;
}

static bool vdec_open(VdecDec *d, const uint8_t *cfg, int cfg_len)
{
    const AVCodec *c = avcodec_find_decoder(AV_CODEC_ID_H264);
    int norm_len;
    const uint8_t *norm;

    avcodec_free_context(&d->ctx);
    if (!c) {
        return false;
    }
    d->ctx = avcodec_alloc_context3(c);
    norm = vdec_cfg_normalise(cfg, cfg_len, &norm_len);
    d->ctx->extradata = av_mallocz(norm_len + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!d->ctx->extradata) {
        avcodec_free_context(&d->ctx);
        return false;
    }
    memcpy(d->ctx->extradata, norm, norm_len);
    d->ctx->extradata_size = norm_len;
    d->ctx->thread_count = 1;   /* the guest expects frames in order */
    if (avcodec_open2(d->ctx, c, NULL) < 0) {
        avcodec_free_context(&d->ctx);
        return false;
    }
    g_free(d->extradata);
    d->extradata = g_memdup2(norm, norm_len);
    d->extradata_len = norm_len;
    return true;
}

int vdec_dec_frame(VdecDec *d, const uint8_t *cfg, int cfg_len,
                   const uint8_t *au, int au_len,
                   uint8_t *y, int ys, uint8_t *u, int us, uint8_t *v, int vs,
                   int width, int height)
{
    int ret;

    if (!d || !cfg || cfg_len <= 0 || !au || au_len <= 0) {
        return -1;
    }
    if (!d->ctx || d->extradata_len != cfg_len ||
        memcmp(d->extradata, cfg, cfg_len) != 0) {
        if (!vdec_open(d, cfg, cfg_len)) {
            return -1;
        }
    }
    if (av_new_packet(d->pkt, au_len) < 0) {
        return -1;
    }
    memcpy(d->pkt->data, au, au_len);
    ret = avcodec_send_packet(d->ctx, d->pkt);
    av_packet_unref(d->pkt);
    if (ret < 0) {
        return -1;
    }
    ret = avcodec_receive_frame(d->ctx, d->frm);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
        return 1;
    }
    if (ret < 0) {
        return -1;
    }
    if (d->frm->format != AV_PIX_FMT_YUV420P &&
        d->frm->format != AV_PIX_FMT_YUVJ420P) {
        qemu_log_mask(LOG_UNIMP, "s5l8702.vdec: decoded pixel format %d is not "
                      "yuv420p; frame dropped\n", d->frm->format);
        av_frame_unref(d->frm);
        return -1;
    }
    if (d->frm->width != width || d->frm->height != height) {
        qemu_log_mask(LOG_UNIMP, "s5l8702.vdec: decoded %dx%d but the guest "
                      "programmed %dx%d; frame dropped\n",
                      d->frm->width, d->frm->height, width, height);
        av_frame_unref(d->frm);
        return -1;
    }
    for (int row = 0; row < height; row++) {
        memcpy(y + (size_t)row * ys,
               d->frm->data[0] + (size_t)row * d->frm->linesize[0], width);
    }
    for (int row = 0; row < height / 2; row++) {
        memcpy(u + (size_t)row * us,
               d->frm->data[1] + (size_t)row * d->frm->linesize[1], width / 2);
        memcpy(v + (size_t)row * vs,
               d->frm->data[2] + (size_t)row * d->frm->linesize[2], width / 2);
    }
    av_frame_unref(d->frm);
    return 0;
}

#else
struct VdecDec { int unused; };
bool vdec_dec_available(void) { return false; }
VdecDec *vdec_dec_new(void) { return NULL; }
void vdec_dec_free(VdecDec *d) { }
void vdec_dec_flush(VdecDec *d) { }
int vdec_dec_frame(VdecDec *d, const uint8_t *cfg, int cfg_len,
                   const uint8_t *au, int au_len,
                   uint8_t *y, int ys, uint8_t *u, int us, uint8_t *v, int vs,
                   int width, int height) { return -1; }
#endif
