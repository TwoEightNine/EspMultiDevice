#include "VideoLegacy.h"

extern "C" {
#include "third_party/ffmpeg/libavcodec/avcodec.h"
#include "third_party/ffmpeg/libavutil/error.h"
#include "third_party/ffmpeg/libavutil/mem.h"
#include "third_party/ffmpeg/libavutil/pixfmt.h"
}

#include <esp32-hal-psram.h>
#include <limits.h>
#include <string.h>

namespace {
const uint32_t InputPadding = AV_INPUT_BUFFER_PADDING_SIZE;
}

VideoLegacy::VideoLegacy()
  : _context(nullptr), _frame(nullptr), _packet(nullptr), _maximumWidth(0), _maximumHeight(0) {
}

VideoLegacy::~VideoLegacy() {
  end();
}

bool VideoLegacy::begin(Codec codec, uint16_t maximumWidth, uint16_t maximumHeight,
                        const uint8_t *configuration, uint32_t configurationBytes) {
  end();
  if (!psramFound() || !maximumWidth || !maximumHeight ||
      (configurationBytes && !configuration)) {
    _error = F("Legacy video needs PSRAM");
    return false;
  }
  enum AVCodecID id = codec == Codec::H263 ? AV_CODEC_ID_H263 : AV_CODEC_ID_MPEG4;
  const AVCodec *decoder = avcodec_find_decoder(id);
  AVCodecContext *context = decoder ? avcodec_alloc_context3(decoder) : nullptr;
  AVFrame *frame = context ? av_frame_alloc() : nullptr;
  AVPacket *packet = frame ? av_packet_alloc() : nullptr;
  if (!context || !frame || !packet) {
    if (packet) av_packet_free(&packet);
    if (frame) av_frame_free(&frame);
    if (context) avcodec_free_context(&context);
    _error = F("Legacy decoder allocation failed");
    return false;
  }
  context->thread_count = 1;
  context->thread_type = 0;
  if (configurationBytes) {
    context->extradata = (uint8_t *)av_mallocz((size_t)configurationBytes + InputPadding);
    if (!context->extradata) {
      av_packet_free(&packet);
      av_frame_free(&frame);
      avcodec_free_context(&context);
      _error = F("Legacy video config allocation failed");
      return false;
    }
    memcpy(context->extradata, configuration, configurationBytes);
    context->extradata_size = (int)configurationBytes;
  }
  int result = avcodec_open2(context, decoder, nullptr);
  if (result < 0) {
    av_packet_free(&packet);
    av_frame_free(&frame);
    avcodec_free_context(&context);
    return setError(result, F("Legacy video config rejected"));
  }
  _context = context;
  _frame = frame;
  _packet = packet;
  _maximumWidth = maximumWidth;
  _maximumHeight = maximumHeight;
  _error = "";
  return true;
}

void VideoLegacy::end() {
  AVPacket *packet = static_cast<AVPacket *>(_packet);
  AVFrame *frame = static_cast<AVFrame *>(_frame);
  AVCodecContext *context = static_cast<AVCodecContext *>(_context);
  if (packet) av_packet_free(&packet);
  if (frame) av_frame_free(&frame);
  if (context) avcodec_free_context(&context);
  _packet = _frame = _context = nullptr;
  _maximumWidth = _maximumHeight = 0;
}

bool VideoLegacy::setError(int code, const __FlashStringHelper *fallback) {
  char detail[80] = {};
  if (code < 0 && av_strerror(code, detail, sizeof(detail)) == 0 && detail[0]) {
    _error = String(fallback) + F("\n") + detail;
  } else {
    _error = fallback;
  }
  return false;
}

bool VideoLegacy::emitFrame(FrameCallback callback, void *context) {
  AVFrame *frame = static_cast<AVFrame *>(_frame);
  if (!frame || !callback) {
    _error = F("Legacy decoder output unavailable");
    return false;
  }
  if (frame->format != AV_PIX_FMT_YUV420P) {
    _error = F("Unsupported legacy pixel format");
    return false;
  }
  if (frame->width < 1 || frame->height < 1) {
    _error = F("Malformed legacy video frame");
    return false;
  }
  if (frame->width > _maximumWidth || frame->height > _maximumHeight) {
    _error = F("Decoded video exceeds 320x240");
    return false;
  }
  if (!frame->data[0] || !frame->data[1] || !frame->data[2] || frame->linesize[0] < frame->width ||
      frame->linesize[1] < (frame->width + 1) / 2 || frame->linesize[2] < (frame->width + 1) / 2 ||
      frame->linesize[0] > 0xffff || frame->linesize[1] > 0xffff || frame->linesize[2] > 0xffff) {
    _error = F("Malformed legacy frame planes");
    return false;
  }
  Frame output = {
    frame->data[0], frame->data[1], frame->data[2],
    (uint16_t)frame->width, (uint16_t)frame->height,
    (uint16_t)frame->linesize[0], (uint16_t)frame->linesize[1], (uint16_t)frame->linesize[2]
  };
  if (!callback(output, context)) {
    _error = F("Legacy video frame rejected");
    return false;
  }
  return true;
}

bool VideoLegacy::receiveFrames(FrameCallback callback, void *context, bool draining) {
  AVCodecContext *decoder = static_cast<AVCodecContext *>(_context);
  AVFrame *frame = static_cast<AVFrame *>(_frame);
  if (!decoder || !frame) return false;
  for (;;) {
    int result = avcodec_receive_frame(decoder, frame);
    if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) return true;
    if (result < 0) return setError(result, draining ? F("Legacy video flush failed") : F("Legacy video decode failed"));
    if (!emitFrame(callback, context)) return false;
    av_frame_unref(frame);
  }
}

bool VideoLegacy::decode(const uint8_t *sample, uint32_t bytes, uint32_t timestamp,
                         FrameCallback callback, void *context) {
  AVCodecContext *decoder = static_cast<AVCodecContext *>(_context);
  AVPacket *packet = static_cast<AVPacket *>(_packet);
  if (!decoder || !packet || !sample || !bytes || !callback || bytes > INT_MAX) {
    _error = F("Malformed legacy video sample");
    return false;
  }
  av_packet_unref(packet);
  int result = av_new_packet(packet, (int)bytes);
  if (result < 0) return setError(result, F("Legacy video packet allocation failed"));
  memcpy(packet->data, sample, bytes);
  packet->pts = timestamp;
  result = avcodec_send_packet(decoder, packet);
  if (result < 0) return setError(result, F("Legacy video decode failed"));
  return receiveFrames(callback, context, false);
}

bool VideoLegacy::flush(FrameCallback callback, void *context) {
  AVCodecContext *decoder = static_cast<AVCodecContext *>(_context);
  if (!decoder || !callback) return false;
  int result = avcodec_send_packet(decoder, nullptr);
  if (result < 0 && result != AVERROR_EOF) return setError(result, F("Legacy video flush failed"));
  return receiveFrames(callback, context, true);
}

const String &VideoLegacy::lastError() const {
  return _error;
}
