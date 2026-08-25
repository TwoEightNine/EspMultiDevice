/*
 * Decoder-only implementation of OpenCORE AMR-NB's public decoder interface.
 * Kept separate from upstream's wrapper.cpp so no encoder code is linked.
 */
#include "third_party/opencore-amr/opencore/codecs_v2/audio/gsm_amr/amr_nb/dec/src/sp_dec.h"
#include "third_party/opencore-amr/opencore/codecs_v2/audio/gsm_amr/amr_nb/dec/src/amrdecode.h"
#include "third_party/opencore-amr/amrnb/interf_dec.h"

void *Decoder_Interface_init(void) {
  void *state = 0;
  return GSMInitDecode(&state, (Word8 *)"Decoder") ? 0 : state;
}

void Decoder_Interface_exit(void *state) {
  GSMDecodeFrameExit(&state);
}

void Decoder_Interface_Decode(void *state, const unsigned char *in, short *out, int bfi) {
  unsigned char type = (in[0] >> 3) & 0x0f;
  if (bfi) type = AMR_NO_DATA;
  AMRDecode(state, (enum Frame_Type_3GPP)type, (UWord8 *)(in + 1), (Word16 *)out, MIME_IETF);
}
