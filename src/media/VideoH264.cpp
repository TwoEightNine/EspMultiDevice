#include "VideoH264.h"

extern "C" {
#include "../../third_party/lib264/decoder/ih264_typedefs.h"
#include "../../third_party/lib264/decoder/ih264d.h"
}

#include <esp32-hal-psram.h>
#include <stdlib.h>
#include <string.h>

namespace {
const uint32_t MaxReferenceFrames = 16;
const uint32_t MaxReorderFrames = 16;

struct DecoderState {
  iv_obj_t *codec;
  iv_mem_rec_t *records;
  void **rawAllocations;
  uint32_t recordCount;
  uint8_t *output[3];
  uint32_t outputBytes[3];
};

void *allocateAlignedPsram(uint32_t bytes, uint32_t alignment) {
  if (!bytes) return nullptr;
  if (alignment < sizeof(void *)) alignment = sizeof(void *);
  uint8_t *raw = static_cast<uint8_t *>(ps_malloc((size_t)bytes + alignment - 1 + sizeof(void *)));
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
}

VideoH264::VideoH264() : _state(nullptr) {
}

VideoH264::~VideoH264() {
  end();
}

bool VideoH264::begin(uint16_t maximumWidth, uint16_t maximumHeight) {
  end();
  if (!psramFound() || !maximumWidth || !maximumHeight) {
    _error = F("H.264 needs PSRAM");
    return false;
  }

  DecoderState *state = static_cast<DecoderState *>(calloc(1, sizeof(DecoderState)));
  if (!state) {
    _error = F("H.264 state allocation failed");
    return false;
  }

  iv_num_mem_rec_ip_t countInput = {};
  iv_num_mem_rec_op_t countOutput = {};
  countInput.u4_size = sizeof(countInput);
  countInput.e_cmd = IV_CMD_GET_NUM_MEM_REC;
  countOutput.u4_size = sizeof(countOutput);
  if (!invoke(nullptr, &countInput, &countOutput) || !countOutput.u4_num_mem_rec) {
    free(state);
    _error = F("H.264 memory query failed");
    return false;
  }

  state->recordCount = countOutput.u4_num_mem_rec;
  state->records = static_cast<iv_mem_rec_t *>(calloc(state->recordCount, sizeof(iv_mem_rec_t)));
  state->rawAllocations = static_cast<void **>(calloc(state->recordCount, sizeof(void *)));
  if (!state->records || !state->rawAllocations) {
    end();
    free(state->records);
    free(state->rawAllocations);
    free(state);
    _error = F("H.264 memory table failed");
    return false;
  }
  for (uint32_t index = 0; index < state->recordCount; index++) state->records[index].u4_size = sizeof(iv_mem_rec_t);

  ih264d_fill_mem_rec_ip_t fillInput = {};
  ih264d_fill_mem_rec_op_t fillOutput = {};
  fillInput.s_ivd_fill_mem_rec_ip_t.u4_size = sizeof(fillInput);
  fillInput.s_ivd_fill_mem_rec_ip_t.e_cmd = IV_CMD_FILL_NUM_MEM_REC;
  fillInput.s_ivd_fill_mem_rec_ip_t.pv_mem_rec_location = state->records;
  fillInput.s_ivd_fill_mem_rec_ip_t.u4_max_frm_wd = maximumWidth;
  fillInput.s_ivd_fill_mem_rec_ip_t.u4_max_frm_ht = maximumHeight;
  fillInput.i4_level = 64;
  fillInput.u4_num_ref_frames = MaxReferenceFrames;
  fillInput.u4_num_reorder_frames = MaxReorderFrames;
  fillInput.u4_share_disp_buf = 0;
  fillInput.e_output_format = IV_YUV_420P;
  fillInput.u4_num_extra_disp_buf = 0;
  fillOutput.s_ivd_fill_mem_rec_op_t.u4_size = sizeof(fillOutput);
  if (!invoke(nullptr, &fillInput, &fillOutput) ||
      !fillOutput.s_ivd_fill_mem_rec_op_t.u4_num_mem_rec_filled ||
      fillOutput.s_ivd_fill_mem_rec_op_t.u4_num_mem_rec_filled > state->recordCount) {
    _error = F("H.264 memory layout failed");
    for (uint32_t index = 0; index < state->recordCount; index++) freeAlignedPsram(state->records[index].pv_base);
    free(state->records);
    free(state->rawAllocations);
    free(state);
    return false;
  }
  state->recordCount = fillOutput.s_ivd_fill_mem_rec_op_t.u4_num_mem_rec_filled;
  for (uint32_t index = 0; index < state->recordCount; index++) {
    state->records[index].pv_base = allocateAlignedPsram(state->records[index].u4_mem_size,
                                                         state->records[index].u4_mem_alignment);
    if (!state->records[index].pv_base) {
      _error = F("H.264 PSRAM exhausted");
      for (uint32_t allocated = 0; allocated < index; allocated++) freeAlignedPsram(state->records[allocated].pv_base);
      free(state->records);
      free(state->rawAllocations);
      free(state);
      return false;
    }
  }

  ih264d_init_ip_t initInput = {};
  ih264d_init_op_t initOutput = {};
  initInput.s_ivd_init_ip_t.u4_size = sizeof(initInput);
  initInput.s_ivd_init_ip_t.e_cmd = (IVD_API_COMMAND_TYPE_T)IV_CMD_INIT;
  initInput.s_ivd_init_ip_t.u4_num_mem_rec = state->recordCount;
  initInput.s_ivd_init_ip_t.u4_frm_max_wd = maximumWidth;
  initInput.s_ivd_init_ip_t.u4_frm_max_ht = maximumHeight;
  initInput.s_ivd_init_ip_t.e_output_format = IV_YUV_420P;
  initInput.s_ivd_init_ip_t.pv_mem_rec_location = state->records;
  initInput.i4_level = 64;
  initInput.u4_num_ref_frames = MaxReferenceFrames;
  initInput.u4_num_reorder_frames = MaxReorderFrames;
  initInput.u4_share_disp_buf = 0;
  initInput.u4_num_extra_disp_buf = 0;
  initOutput.s_ivd_init_op_t.u4_size = sizeof(initOutput);
  state->codec = static_cast<iv_obj_t *>(state->records[0].pv_base);
  state->codec->u4_size = sizeof(iv_obj_t);
  state->codec->pv_fxns = reinterpret_cast<void *>(&ih264d_api_function);
  if (!invoke(state->codec, &initInput, &initOutput)) {
    _error = F("H.264 decoder init failed");
    for (uint32_t index = 0; index < state->recordCount; index++) freeAlignedPsram(state->records[index].pv_base);
    free(state->records);
    free(state->rawAllocations);
    free(state);
    return false;
  }

  ih264d_ctl_set_num_cores_ip_t coresInput = {};
  ih264d_ctl_set_num_cores_op_t coresOutput = {};
  coresInput.u4_size = sizeof(coresInput);
  coresInput.e_cmd = IVD_CMD_VIDEO_CTL;
  coresInput.e_sub_cmd = (IVD_CONTROL_API_COMMAND_TYPE_T)IH264D_CMD_CTL_SET_NUM_CORES;
  coresInput.u4_num_cores = 1;
  coresOutput.u4_size = sizeof(coresOutput);
  if (!invoke(state->codec, &coresInput, &coresOutput)) {
    _error = F("H.264 single-core setup failed");
    _state = state;
    end();
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
  if (!invoke(state->codec, &processorInput, &processorOutput)) {
    _error = F("H.264 generic setup failed");
    _state = state;
    end();
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
  if (!invoke(state->codec, &configInput, &configOutput)) {
    _error = F("H.264 output setup failed");
    _state = state;
    end();
    return false;
  }

  uint32_t yBytes = (uint32_t)((maximumWidth + 15) & ~15U) * ((maximumHeight + 15) & ~15U);
  state->outputBytes[0] = yBytes;
  state->outputBytes[1] = yBytes / 4;
  state->outputBytes[2] = yBytes / 4;
  for (uint8_t index = 0; index < 3; index++) {
    state->output[index] = static_cast<uint8_t *>(ps_malloc(state->outputBytes[index]));
    if (!state->output[index]) {
      _error = F("H.264 output PSRAM exhausted");
      _state = state;
      end();
      return false;
    }
  }
  _state = state;
  _error = "";
  return true;
}

void VideoH264::end() {
  DecoderState *state = static_cast<DecoderState *>(_state);
  if (!state) return;
  if (state->codec && state->records) {
    iv_retrieve_mem_rec_ip_t input = {};
    iv_retrieve_mem_rec_op_t output = {};
    input.u4_size = sizeof(input);
    input.e_cmd = IV_CMD_RETRIEVE_MEMREC;
    input.pv_mem_rec_location = state->records;
    output.u4_size = sizeof(output);
    ih264d_api_function(state->codec, &input, &output);
  }
  for (uint8_t index = 0; index < 3; index++) if (state->output[index]) free(state->output[index]);
  for (uint32_t index = 0; index < state->recordCount; index++) freeAlignedPsram(state->records[index].pv_base);
  free(state->records);
  free(state->rawAllocations);
  free(state);
  _state = nullptr;
}

bool VideoH264::setError(uint32_t errorCode, const __FlashStringHelper *fallback) {
  if (errorCode & (1UL << IVD_UNSUPPORTEDINPUT)) _error = F("Unsupported H.264 feature");
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
  for (uint8_t pass = 0; pass <= MaxReorderFrames; pass++) {
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
