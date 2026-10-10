/*
 * H.264 decoder behind the S5L8702 video block (0x39800000).
 *
 * The retailOS parses the container/SPS/PPS/slice headers on the ARM and hands
 * the block per-slice entropy payloads; this wraps libavcodec so the model can
 * do what the silicon block does (access unit in, YUV420 planes out). The
 * front-end destroys the access unit in place before submit, so under the
 * VDEC_DECODE shortcut the device is fed the access unit and codec config from
 * files dumped by the pctrace `audump=` tap (see ipod_classic_vdec.c).
 *
 * Built only with CONFIG_SM1_DECODER (the same libavcodec dependency); without
 * it vdec_dec_available() is false and the device stays a latch-only stub.
 */
#ifndef HW_ARM_IPOD_CLASSIC_VDEC_DEC_H
#define HW_ARM_IPOD_CLASSIC_VDEC_DEC_H

#include <stdint.h>
#include <stdbool.h>

typedef struct VdecDec VdecDec;

bool vdec_dec_available(void);
VdecDec *vdec_dec_new(void);
void vdec_dec_free(VdecDec *d);
void vdec_dec_flush(VdecDec *d);

/*
 * Decode one H.264 access unit (4-byte length-prefixed NALs) using the codec
 * config blob (the MP4 avcC record) and copy the YUV420 planes into the
 * caller's buffers at the given strides. width/height are the geometry the
 * guest programmed; the decoded frame must match. Returns 0 on a frame copied,
 * 1 if the decoder produced no frame yet, <0 on error.
 */
int vdec_dec_frame(VdecDec *d, const uint8_t *cfg, int cfg_len,
                   const uint8_t *au, int au_len,
                   uint8_t *y, int ys, uint8_t *u, int us, uint8_t *v, int vs,
                   int width, int height);

#endif
