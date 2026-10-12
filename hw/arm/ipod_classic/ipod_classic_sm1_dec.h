/*
 * Frame decoder behind the SM1 audio engine (sub-block 4).
 *
 * The retailOS hands SM1 one compressed frame at a time and reads the decoded
 * 16-bit stereo PCM back from the engine's output slots. The silicon decoder is
 * not documented; this wraps libavcodec so the model can do what the hardware
 * does (frame in, PCM out). Built only with CONFIG_SM1_DECODER; otherwise
 * sm1_dec_available() is false and the engine stays PCM-less.
 */
#ifndef HW_ARM_IPOD_CLASSIC_SM1_DEC_H
#define HW_ARM_IPOD_CLASSIC_SM1_DEC_H

#include <stdint.h>
#include <stdbool.h>

typedef struct Sm1Dec Sm1Dec;

/* Codecs the engine is selected for (from the guest's enable word, see sm1.c). */
typedef enum { SM1_CODEC_NONE, SM1_CODEC_AAC, SM1_CODEC_ALAC, SM1_CODEC_MP3 } Sm1Codec;

/* Per-stream parameters the guest programs into the engine's control block. */
typedef struct {
    int freq_idx;               /* AAC: MPEG-4 sampling-frequency index */
    int bits, pb, mb, kb;       /* ALAC: bit depth and Rice parameters of the magic cookie */
} Sm1Params;

bool sm1_dec_available(void);
Sm1Dec *sm1_dec_new(void);
void sm1_dec_free(Sm1Dec *d);
void sm1_dec_flush(Sm1Dec *d);
/*
 * Decode one frame to interleaved stereo s16. `p` is what the guest
 * programmed for this stream. Returns halfwords written
 * (2 per stereo frame), 0 if the decoder produced nothing, <0 on error.
 */
int sm1_dec_frame(Sm1Dec *d, Sm1Codec codec, const Sm1Params *p,
                  const uint8_t *frame, int len, int16_t *pcm, int cap);
#endif
