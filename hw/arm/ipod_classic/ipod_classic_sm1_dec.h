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
typedef enum { SM1_CODEC_AAC = 1 } Sm1Codec;

bool sm1_dec_available(void);
Sm1Dec *sm1_dec_new(void);
void sm1_dec_free(Sm1Dec *d);
void sm1_dec_flush(Sm1Dec *d);
/*
 * Decode one frame to interleaved stereo s16. aac_freq_idx is the MPEG-4
 * sampling-frequency index the guest programmed. Returns halfwords written
 * (2 per stereo frame), 0 if the decoder produced nothing, <0 on error.
 */
int sm1_dec_frame(Sm1Dec *d, Sm1Codec codec, int aac_freq_idx,
                  const uint8_t *frame, int len, int16_t *pcm, int cap);
#endif
