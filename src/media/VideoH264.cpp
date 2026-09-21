#include "VideoH264.h"

extern "C" {
#include "../../third_party/lib264/decoder/ih264_typedefs.h"
#include "../../third_party/lib264/decoder/ih264d.h"
}

#include <esp32-hal-psram.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

namespace {
const uint8_t ReservationLevels[] = {16, 8, 4, 2, 1};
const uint8_t H264UnavailablePictureBuffer = 0x61;
const uint8_t H264UnavailableMotionVectorBuffer = 0x62;

class SpsBitReader {
public:
  SpsBitReader(const uint8_t *bytes, uint32_t byteCount)
      : _bytes(bytes), _byteCount(byteCount), _offset(0), _bit(8), _current(0),
        _zeroBytes(0) {
  }

  bool readBit(uint32_t &value) {
    if (_bit == 8) {
      if (!nextByte(_current)) return false;
      _bit = 0;
    }
    value = (_current >> (7 - _bit)) & 1U;
    ++_bit;
    return true;
  }

  bool readBits(uint8_t count, uint32_t &value) {
    value = 0;
    if (count > 32) return false;
    for (uint8_t index = 0; index < count; ++index) {
      uint32_t bit = 0;
      if (!readBit(bit)) return false;
      value = (value << 1) | bit;
    }
    return true;
  }

  bool readUnsignedExpGolomb(uint32_t &value) {
    uint8_t zeroCount = 0;
    uint32_t bit = 0;
    do {
      if (!readBit(bit) || zeroCount >= 31) return false;
      if (!bit) ++zeroCount;
    } while (!bit);
    uint32_t suffix = 0;
    if (!readBits(zeroCount, suffix)) return false;
    value = ((1UL << zeroCount) - 1UL) + suffix;
    return true;
  }

  bool readSignedExpGolomb(int32_t &value) {
    uint32_t codeNum = 0;
    if (!readUnsignedExpGolomb(codeNum) || codeNum > UINT32_MAX - 1) return false;
    value = (codeNum & 1U) ? (int32_t)((codeNum + 1U) >> 1) : -(int32_t)(codeNum >> 1);
    return true;
  }

private:
  bool nextByte(uint8_t &value) {
    while (_offset < _byteCount) {
      value = _bytes[_offset++];
      if (_zeroBytes >= 2 && value == 3) {
        _zeroBytes = 0;
        continue;
      }
      _zeroBytes = value == 0 ? _zeroBytes + 1 : 0;
      return true;
    }
    return false;
  }

  const uint8_t *_bytes;
  uint32_t _byteCount;
  uint32_t _offset;
  uint8_t _bit;
  uint8_t _current;
  uint8_t _zeroBytes;
};

bool skipScalingList(SpsBitReader &reader, uint8_t entries) {
  int32_t lastScale = 8;
  int32_t nextScale = 8;
  for (uint8_t index = 0; index < entries; ++index) {
    if (nextScale != 0) {
      int32_t deltaScale = 0;
      if (!reader.readSignedExpGolomb(deltaScale)) return false;
      nextScale = (lastScale + deltaScale + 256) & 255;
    }
    if (nextScale != 0) lastScale = nextScale;
  }
  return true;
}

uint32_t requiredReferenceFrames(const uint8_t *sequenceParameterSet, uint32_t bytes) {
  // AVC sample entries carry a complete SPS NAL.  This reads only the
  // max_num_ref_frames field needed to size the decoder-owned frame pool.
  if (!sequenceParameterSet || bytes < 4 || (sequenceParameterSet[0] & 0x1f) != 7) return 1;
  SpsBitReader reader(sequenceParameterSet + 1, bytes - 1);
  uint32_t profile = 0;
  uint32_t unused = 0;
  uint32_t sequenceId = 0;
  if (!reader.readBits(8, profile) || !reader.readBits(8, unused) ||
      !reader.readBits(8, unused) || !reader.readUnsignedExpGolomb(sequenceId)) return 1;

  const bool extendedProfile = profile == 100 || profile == 110 || profile == 122 || profile == 244 ||
                               profile == 44 || profile == 83 || profile == 86 || profile == 118 ||
                               profile == 128 || profile == 138 || profile == 139 || profile == 134 ||
                               profile == 135;
  if (extendedProfile) {
    uint32_t chromaFormat = 0;
    if (!reader.readUnsignedExpGolomb(chromaFormat)) return 1;
    if (chromaFormat == 3 && !reader.readBit(unused)) return 1;
    if (!reader.readUnsignedExpGolomb(unused) || !reader.readUnsignedExpGolomb(unused) ||
        !reader.readBit(unused)) return 1;
    uint32_t scalingPresent = 0;
    if (!reader.readBit(scalingPresent)) return 1;
    if (scalingPresent) {
      const uint8_t listCount = chromaFormat == 3 ? 12 : 8;
      for (uint8_t index = 0; index < listCount; ++index) {
        uint32_t listPresent = 0;
        if (!reader.readBit(listPresent)) return 1;
        if (listPresent && !skipScalingList(reader, index < 6 ? 16 : 64)) return 1;
      }
    }
  }

  uint32_t picOrderCountType = 0;
  if (!reader.readUnsignedExpGolomb(unused) || !reader.readUnsignedExpGolomb(picOrderCountType)) return 1;
  if (picOrderCountType == 0) {
    if (!reader.readUnsignedExpGolomb(unused)) return 1;
  } else if (picOrderCountType == 1) {
    int32_t signedValue = 0;
    uint32_t cycle = 0;
    if (!reader.readBit(unused) || !reader.readSignedExpGolomb(signedValue) ||
        !reader.readSignedExpGolomb(signedValue) || !reader.readUnsignedExpGolomb(cycle) || cycle > 255) return 1;
    for (uint32_t index = 0; index < cycle; ++index)
      if (!reader.readSignedExpGolomb(signedValue)) return 1;
  } else if (picOrderCountType > 2) {
    return 1;
  }

  uint32_t referenceFrames = 0;
  if (!reader.readUnsignedExpGolomb(referenceFrames)) return 1;
  return referenceFrames > 0 && referenceFrames <= 16 ? referenceFrames : 1;
}

struct DecoderState {
  iv_obj_t *codec;
  iv_mem_rec_t *records;
  uint32_t recordCount;
  uint32_t allocatedRecordCount;
  uint32_t referenceFrames;
  uint32_t reorderFrames;
  bool initialized;
  bool bufferManagersReady;
  uint8_t *output[3];
  uint32_t outputBytes[3];
};

void *allocateAlignedPsram(uint32_t bytes, uint32_t alignment) {
  if (!bytes) return nullptr;
  if (alignment < sizeof(void *)) alignment = sizeof(void *);
  const size_t overhead = (size_t)alignment - 1 + sizeof(void *);
  if (bytes > SIZE_MAX - overhead) return nullptr;
  uint8_t *raw = static_cast<uint8_t *>(ps_malloc((size_t)bytes + overhead));
  if (!raw) return nullptr;
  uintptr_t address = reinterpret_cast<uintptr_t>(raw + sizeof(void *));
  address = (address + alignment - 1) & ~(uintptr_t)(alignment - 1);
  void *aligned = reinterpret_cast<void *>(address);
  reinterpret_cast<void **>(aligned)[-1] = raw;
  return aligned;
}

void freeAlignedPsram(void *aligned) {
  if (aligned) free(reinterpret_cast<void **>(aligned)[-1]);
}

bool invoke(iv_obj_t *codec, void *input, void *output) {
  return ih264d_api_function(codec, input, output) == IV_SUCCESS;
}

void releaseState(DecoderState *state) {
  if (!state) return;
  // lib264's retrieve command destroys buffer-manager mutexes.  Those
  // managers are created only after it accepts picture headers; invoking the
  // command immediately after IV_CMD_INIT dereferences its null managers.
  if (state->initialized && state->bufferManagersReady && state->codec && state->records) {
    iv_retrieve_mem_rec_ip_t input = {};
    iv_retrieve_mem_rec_op_t output = {};
    input.u4_size = sizeof(input);
    input.e_cmd = IV_CMD_RETRIEVE_MEMREC;
    input.pv_mem_rec_location = state->records;
    output.u4_size = sizeof(output);
    ih264d_api_function(state->codec, &input, &output);
  }
  for (uint8_t index = 0; index < 3; ++index) free(state->output[index]);
  if (state->records) {
    // A failed fill/allocation can leave lib264-owned descriptions in slots
    // we did not allocate.  Only aligned blocks recorded below are ours.
    const uint32_t allocatedCount = state->allocatedRecordCount < state->recordCount
                                        ? state->allocatedRecordCount
                                        : state->recordCount;
    for (uint32_t index = 0; index < allocatedCount; ++index)
      freeAlignedPsram(state->records[index].pv_base);
  }
  free(state->records);
  free(state);
}

bool createState(uint16_t maximumWidth, uint16_t maximumHeight, uint32_t yBytes,
                 uint32_t referenceFrames, uint32_t reorderFrames,
                 DecoderState *&state, String &error, bool &psramExhausted) {
  state = nullptr;
  psramExhausted = false;
  DecoderState *candidate = static_cast<DecoderState *>(calloc(1, sizeof(DecoderState)));
  if (!candidate) {
    error = F("H.264 state allocation failed");
    return false;
  }

  iv_num_mem_rec_ip_t countInput = {};
  iv_num_mem_rec_op_t countOutput = {};
  countInput.u4_size = sizeof(countInput);
  countInput.e_cmd = IV_CMD_GET_NUM_MEM_REC;
  countOutput.u4_size = sizeof(countOutput);
  if (!invoke(nullptr, &countInput, &countOutput) || !countOutput.u4_num_mem_rec) {
    error = F("H.264 memory query failed");
    releaseState(candidate);
    return false;
  }
  candidate->recordCount = countOutput.u4_num_mem_rec;
  candidate->records = static_cast<iv_mem_rec_t *>(calloc(candidate->recordCount, sizeof(iv_mem_rec_t)));
  if (!candidate->records) {
    error = F("H.264 memory table failed");
    releaseState(candidate);
    return false;
  }
  for (uint32_t index = 0; index < candidate->recordCount; ++index)
    candidate->records[index].u4_size = sizeof(iv_mem_rec_t);

  ih264d_fill_mem_rec_ip_t fillInput = {};
  ih264d_fill_mem_rec_op_t fillOutput = {};
  fillInput.s_ivd_fill_mem_rec_ip_t.u4_size = sizeof(fillInput);
  fillInput.s_ivd_fill_mem_rec_ip_t.e_cmd = IV_CMD_FILL_NUM_MEM_REC;
  fillInput.s_ivd_fill_mem_rec_ip_t.pv_mem_rec_location = candidate->records;
  fillInput.s_ivd_fill_mem_rec_ip_t.u4_max_frm_wd = maximumWidth;
  fillInput.s_ivd_fill_mem_rec_ip_t.u4_max_frm_ht = maximumHeight;
  fillInput.i4_level = 64;
  fillInput.u4_num_ref_frames = referenceFrames;
  fillInput.u4_num_reorder_frames = reorderFrames;
  fillInput.u4_share_disp_buf = 0;
  fillInput.e_output_format = IV_YUV_420P;
  fillInput.u4_num_extra_disp_buf = 0;
  fillOutput.s_ivd_fill_mem_rec_op_t.u4_size = sizeof(fillOutput);
  if (!invoke(nullptr, &fillInput, &fillOutput) ||
      !fillOutput.s_ivd_fill_mem_rec_op_t.u4_num_mem_rec_filled ||
      fillOutput.s_ivd_fill_mem_rec_op_t.u4_num_mem_rec_filled > candidate->recordCount) {
    error = F("H.264 memory layout failed");
    releaseState(candidate);
    return false;
  }
  candidate->recordCount = fillOutput.s_ivd_fill_mem_rec_op_t.u4_num_mem_rec_filled;
  for (uint32_t index = 0; index < candidate->recordCount; ++index) {
    candidate->records[index].pv_base = allocateAlignedPsram(candidate->records[index].u4_mem_size,
                                                              candidate->records[index].u4_mem_alignment);
    if (!candidate->records[index].pv_base) {
      error = F("H.264 PSRAM exhausted");
      psramExhausted = true;
      releaseState(candidate);
      return false;
    }
    ++candidate->allocatedRecordCount;
  }

  ih264d_init_ip_t initInput = {};
  ih264d_init_op_t initOutput = {};
  initInput.s_ivd_init_ip_t.u4_size = sizeof(initInput);
  initInput.s_ivd_init_ip_t.e_cmd = (IVD_API_COMMAND_TYPE_T)IV_CMD_INIT;
  initInput.s_ivd_init_ip_t.u4_num_mem_rec = candidate->recordCount;
  initInput.s_ivd_init_ip_t.u4_frm_max_wd = maximumWidth;
  initInput.s_ivd_init_ip_t.u4_frm_max_ht = maximumHeight;
  initInput.s_ivd_init_ip_t.e_output_format = IV_YUV_420P;
  initInput.s_ivd_init_ip_t.pv_mem_rec_location = candidate->records;
  initInput.i4_level = 64;
  initInput.u4_num_ref_frames = referenceFrames;
  initInput.u4_num_reorder_frames = reorderFrames;
  initInput.u4_share_disp_buf = 0;
  initInput.u4_num_extra_disp_buf = 0;
  initOutput.s_ivd_init_op_t.u4_size = sizeof(initOutput);
  candidate->codec = static_cast<iv_obj_t *>(candidate->records[0].pv_base);
  candidate->codec->u4_size = sizeof(iv_obj_t);
  candidate->codec->pv_fxns = reinterpret_cast<void *>(&ih264d_api_function);
  if (!invoke(candidate->codec, &initInput, &initOutput)) {
    error = F("H.264 decoder init failed");
    releaseState(candidate);
    return false;
  }
  candidate->initialized = true;

  ih264d_ctl_set_num_cores_ip_t coresInput = {};
  ih264d_ctl_set_num_cores_op_t coresOutput = {};
  coresInput.u4_size = sizeof(coresInput);
  coresInput.e_cmd = IVD_CMD_VIDEO_CTL;
  coresInput.e_sub_cmd = (IVD_CONTROL_API_COMMAND_TYPE_T)IH264D_CMD_CTL_SET_NUM_CORES;
  coresInput.u4_num_cores = 1;
  coresOutput.u4_size = sizeof(coresOutput);
  if (!invoke(candidate->codec, &coresInput, &coresOutput)) {
    error = F("H.264 single-core setup failed");
    releaseState(candidate);
    return false;
  }

  ih264d_ctl_set_processor_ip_t processorInput = {};
  ih264d_ctl_set_processor_op_t processorOutput = {};
  processorInput.u4_size = sizeof(processorInput);
  processorInput.e_cmd = IVD_CMD_VIDEO_CTL;
  processorInput.e_sub_cmd = (IVD_CONTROL_API_COMMAND_TYPE_T)IH264D_CMD_CTL_SET_PROCESSOR;
  processorInput.u4_arch = ARCH_X86_GENERIC;
  processorInput.u4_soc = SOC_GENERIC;
  processorInput.u4_num_cores = 1;
  processorOutput.u4_size = sizeof(processorOutput);
  if (!invoke(candidate->codec, &processorInput, &processorOutput)) {
    error = F("H.264 generic setup failed");
    releaseState(candidate);
    return false;
  }

  ivd_ctl_set_config_ip_t configInput = {};
  ivd_ctl_set_config_op_t configOutput = {};
  configInput.u4_size = sizeof(configInput);
  configInput.e_cmd = IVD_CMD_VIDEO_CTL;
  configInput.e_sub_cmd = IVD_CMD_CTL_SETPARAMS;
  configInput.e_vid_dec_mode = IVD_DECODE_FRAME;
  configInput.u4_disp_wd = maximumWidth;
  configInput.e_frm_skip_mode = IVD_SKIP_NONE;
  configInput.e_frm_out_mode = IVD_DISPLAY_FRAME_OUT;
  configOutput.u4_size = sizeof(configOutput);
  if (!invoke(candidate->codec, &configInput, &configOutput)) {
    error = F("H.264 output setup failed");
    releaseState(candidate);
    return false;
  }

  candidate->outputBytes[0] = yBytes;
  candidate->outputBytes[1] = yBytes / 4;
  candidate->outputBytes[2] = yBytes / 4;
  for (uint8_t index = 0; index < 3; ++index) {
    candidate->output[index] = static_cast<uint8_t *>(ps_malloc(candidate->outputBytes[index]));
    if (!candidate->output[index]) {
      error = F("H.264 output PSRAM exhausted");
      psramExhausted = true;
      releaseState(candidate);
      return false;
    }
  }
  candidate->referenceFrames = referenceFrames;
  candidate->reorderFrames = reorderFrames;
  state = candidate;
  return true;
}
}

VideoH264::VideoH264() : _state(nullptr) {
}

VideoH264::~VideoH264() {
  end();
}

bool VideoH264::begin(uint16_t maximumWidth, uint16_t maximumHeight,
                      const uint8_t *sequenceParameterSet, uint32_t sequenceParameterSetBytes) {
  end();
  if (!psramFound() || !maximumWidth || !maximumHeight) {
    _error = F("H.264 needs PSRAM");
    return false;
  }
  const uint64_t alignedWidth = ((uint64_t)maximumWidth + 15U) & ~15ULL;
  const uint64_t alignedHeight = ((uint64_t)maximumHeight + 15U) & ~15ULL;
  const uint64_t yBytes64 = alignedWidth * alignedHeight;
  if (!yBytes64 || yBytes64 > UINT32_MAX) {
    _error = F("H.264 frame dimensions overflow");
    return false;
  }

  String error;
  const uint32_t minimumReferences = requiredReferenceFrames(sequenceParameterSet, sequenceParameterSetBytes);
  Serial.printf("[Video] H.264 SPS minimum refs=%u\n", (unsigned)minimumReferences);
  for (uint8_t level = 0; level < sizeof(ReservationLevels); ++level) {
    const uint32_t reservation = ReservationLevels[level];
    if (reservation < minimumReferences) break;
    DecoderState *state = nullptr;
    bool psramExhausted = false;
    if (createState(maximumWidth, maximumHeight, (uint32_t)yBytes64, reservation, reservation,
                    state, error, psramExhausted)) {
      _state = state;
      Serial.printf("[Video] H.264 reservation refs=%u reorder=%u\n",
                    (unsigned)reservation, (unsigned)reservation);
      _error = "";
      return true;
    }
    if (!psramExhausted) {
      _error = error;
      return false;
    }

    // Preserve the references demanded by the source, but progressively
    // trade only display-reorder capacity for PSRAM after the equal power-of-
    // two reservation fails. A 4-reference stream, for example, can still
    // run from a 4/2 or 4/1 pool when 4/4 does not fit.
    if (reservation == minimumReferences ||
        (level + 1 < sizeof(ReservationLevels) && ReservationLevels[level + 1] < minimumReferences)) {
      for (uint8_t lower = level + 1; lower < sizeof(ReservationLevels); ++lower) {
        const uint32_t reorder = ReservationLevels[lower];
        DecoderState *reducedState = nullptr;
        bool reducedPsramExhausted = false;
        if (createState(maximumWidth, maximumHeight, (uint32_t)yBytes64, reservation, reorder,
                        reducedState, error, reducedPsramExhausted)) {
          _state = reducedState;
          Serial.printf("[Video] H.264 reservation refs=%u reorder=%u\n",
                        (unsigned)reservation, (unsigned)reorder);
          _error = "";
          return true;
        }
        if (!reducedPsramExhausted) {
          _error = error;
          return false;
        }
      }
      DecoderState *lowDelayState = nullptr;
      bool lowDelayPsramExhausted = false;
      if (createState(maximumWidth, maximumHeight, (uint32_t)yBytes64, reservation, 0,
                      lowDelayState, error, lowDelayPsramExhausted)) {
        _state = lowDelayState;
        Serial.printf("[Video] H.264 reservation refs=%u reorder=0\n", (unsigned)reservation);
        _error = "";
        return true;
      }
      if (!lowDelayPsramExhausted) {
        _error = error;
        return false;
      }
      break;
    }
  }
  _error = error;
  return false;
}

void VideoH264::end() {
  DecoderState *state = static_cast<DecoderState *>(_state);
  if (!state) return;
  releaseState(state);
  _state = nullptr;
}

bool VideoH264::setError(uint32_t errorCode, const __FlashStringHelper *fallback) {
  const uint8_t detail = (uint8_t)(errorCode & 0xff);
  if (detail == H264UnavailablePictureBuffer || detail == H264UnavailableMotionVectorBuffer) {
    DecoderState *state = static_cast<DecoderState *>(_state);
    _error = F("H.264 frame pool exhausted");
    if (state) {
      _error += String(F(" (refs=")) + state->referenceFrames +
                String(F(", reorder=")) + state->reorderFrames + ')';
    }
  } else if (errorCode & (1UL << IVD_UNSUPPORTEDINPUT)) _error = F("Unsupported H.264 feature");
  else if (errorCode & (1UL << IVD_UNSUPPORTEDPARAM)) _error = F("Unsupported H.264 parameters");
  else if (errorCode & (1UL << IVD_CORRUPTEDHEADER)) _error = F("Invalid H.264 headers");
  else if (errorCode & (1UL << IVD_CORRUPTEDDATA)) _error = F("Corrupt H.264 stream");
  else if (errorCode & (1UL << IVD_INSUFFICIENTDATA)) _error = F("Truncated H.264 stream");
  else _error = fallback;
  if (errorCode) _error += String(F(" (0x")) + String(errorCode, HEX) + ')';
  return false;
}

bool VideoH264::emitOutput(const void *operation, FrameCallback callback, void *context) {
  const ivd_video_decode_op_t &output = *static_cast<const ivd_video_decode_op_t *>(operation);
  if (!output.u4_output_present) return true;
  DecoderState *state = static_cast<DecoderState *>(_state);
  if (state) state->bufferManagersReady = true;
  if (!output.u4_progressive_frame_flag) {
    _error = F("Interlaced H.264 is unsupported");
    return false;
  }
  const iv_yuv_buf_t &planes = output.s_disp_frm_buf;
  if (!planes.pv_y_buf || !planes.pv_u_buf || !planes.pv_v_buf || !output.u4_pic_wd || !output.u4_pic_ht ||
      output.u4_pic_wd > 0xffff || output.u4_pic_ht > 0xffff || planes.u4_y_strd > 0xffff ||
      planes.u4_u_strd > 0xffff || planes.u4_v_strd > 0xffff) {
    _error = F("Invalid H.264 picture output");
    return false;
  }
  Frame frame = {static_cast<const uint8_t *>(planes.pv_y_buf), static_cast<const uint8_t *>(planes.pv_u_buf),
                 static_cast<const uint8_t *>(planes.pv_v_buf), (uint16_t)output.u4_pic_wd,
                 (uint16_t)output.u4_pic_ht, (uint16_t)planes.u4_y_strd, (uint16_t)planes.u4_u_strd,
                 (uint16_t)planes.u4_v_strd};
  if (!callback || !callback(frame, context)) {
    _error = F("H.264 picture rejected");
    return false;
  }
  return true;
}

bool VideoH264::decode(const uint8_t *annexB, uint32_t bytes, uint32_t timestamp,
                       FrameCallback callback, void *context) {
  DecoderState *state = static_cast<DecoderState *>(_state);
  if (!state || !annexB || !bytes) return false;
  uint32_t remaining = bytes;
  const uint8_t *inputBytes = annexB;
  while (remaining) {
    ivd_video_decode_ip_t input = {};
    ivd_video_decode_op_t output = {};
    input.u4_size = sizeof(input);
    input.e_cmd = IVD_CMD_VIDEO_DECODE;
    input.u4_ts = timestamp;
    input.pv_stream_buffer = const_cast<uint8_t *>(inputBytes);
    input.u4_num_Bytes = remaining;
    input.s_out_buffer.u4_num_bufs = 3;
    for (uint8_t index = 0; index < 3; index++) {
      input.s_out_buffer.pu1_bufs[index] = state->output[index];
      input.s_out_buffer.u4_min_out_buf_size[index] = state->outputBytes[index];
    }
    output.u4_size = sizeof(output);
    if (!invoke(state->codec, &input, &output)) return setError(output.u4_error_code, F("H.264 decode failed"));
    if (!emitOutput(&output, callback, context)) return false;
    if (!output.u4_num_bytes_consumed || output.u4_num_bytes_consumed > remaining) {
      _error = F("Invalid H.264 access unit");
      return false;
    }
    inputBytes += output.u4_num_bytes_consumed;
    remaining -= output.u4_num_bytes_consumed;
  }
  return true;
}

bool VideoH264::flush(FrameCallback callback, void *context) {
  DecoderState *state = static_cast<DecoderState *>(_state);
  if (!state) return false;
  for (uint32_t pass = 0; pass <= state->reorderFrames; pass++) {
    ivd_ctl_flush_ip_t flushInput = {};
    ivd_ctl_flush_op_t flushOutput = {};
    flushInput.u4_size = sizeof(flushInput);
    flushInput.e_cmd = IVD_CMD_VIDEO_CTL;
    flushInput.e_sub_cmd = IVD_CMD_CTL_FLUSH;
    flushOutput.u4_size = sizeof(flushOutput);
    if (!invoke(state->codec, &flushInput, &flushOutput)) return setError(flushOutput.u4_error_code, F("H.264 flush failed"));

    ivd_video_decode_ip_t input = {};
    ivd_video_decode_op_t output = {};
    input.u4_size = sizeof(input);
    input.e_cmd = IVD_CMD_VIDEO_DECODE;
    input.s_out_buffer.u4_num_bufs = 3;
    for (uint8_t index = 0; index < 3; index++) {
      input.s_out_buffer.pu1_bufs[index] = state->output[index];
      input.s_out_buffer.u4_min_out_buf_size[index] = state->outputBytes[index];
    }
    output.u4_size = sizeof(output);
    if (!invoke(state->codec, &input, &output)) return setError(output.u4_error_code, F("H.264 flush failed"));
    if (!output.u4_output_present) return true;
    if (!emitOutput(&output, callback, context)) return false;
  }
  _error = F("H.264 reorder queue overflow");
  return false;
}

const String &VideoH264::lastError() const {
  return _error;
}
