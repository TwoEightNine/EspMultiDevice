#define MINIMP4_IMPLEMENTATION
#include "third_party/minimp4/minimp4.h"

#define MINIMP3_ONLY_MP3
#include "third_party/minimp3/minimp3.h"

extern "C" {
#include "third_party/libhelix/libhelix-aac/aacdec.h"
#include "third_party/opencore-amr/amrnb/interf_dec.h"
}

#include "VideoPlayerScreen.h"

#include "src/media/AnimationCache.h"
#include "src/app/Navigator.h"
#include "src/core/SpiBusLock.h"
#include "src/ui/UiKit.h"

#include <SD.h>
#include <esp32-hal-psram.h>
#include <stdlib.h>
#include <string.h>

namespace {
// ISO/IEC 14496-3 sampling-frequency-index table.  Keep the exact Hz values:
// 44.1 kHz and 22.05 kHz are common in phone-shot MP4/3GP clips.
const uint32_t AacSampleRates[] = {
  96000, 88200, 64000, 48000, 44100, 32000, 24000,
  22050, 16000, 12000, 11025, 8000, 7350
};

int readMp4(int64_t offset, void *buffer, size_t bytes, void *token) {
  StorageFile *file = static_cast<StorageFile *>(token);
  if (!file || offset < 0 || offset > 0xffffffffLL || !SpiBusLock::lock(pdMS_TO_TICKS(100))) return 1;
  bool ok = file->seek((uint32_t)offset) && file->read((uint8_t *)buffer, bytes) == bytes;
  SpiBusLock::unlock();
  return ok ? 0 : 1;
}

uint8_t clampChannel(int value) {
  return value < 0 ? 0 : (value > 255 ? 255 : (uint8_t)value);
}

const __FlashStringHelper *avcProfileName(unsigned profile) {
  switch (profile) {
    case 66: return F("Baseline");
    case 77: return F("Main");
    case 88: return F("Extended");
    case 100: return F("High");
    case 110: return F("High 10");
    case 122: return F("High 4:2:2");
    case 244: return F("High 4:4:4");
    default: return F("profile");
  }
}

String objectTypeLabel(unsigned objectType) {
  if (!objectType) return F("unknown video sample entry");
  if (objectType == MP4_OBJECT_TYPE_H263) return F("H.263");
  if (objectType == MP4_OBJECT_TYPE_AMR_NB) return F("AMR-NB");
  if (objectType == MP4_OBJECT_TYPE_HEVC) return F("HEVC/H.265");
  if (objectType == 0x20) return F("MPEG-4 Part 2");
  if (objectType == 0x6a) return F("MPEG-1 video");
  return String(F("object 0x")) + String(objectType, HEX);
}

String sampleEntryLabel(unsigned sampleEntry) {
  if (!sampleEntry) return F("unknown entry");
  String label;
  for (int shift = 24; shift >= 0; shift -= 8) {
    char value = (char)((sampleEntry >> shift) & 0xff);
    label += value >= 0x20 && value <= 0x7e ? value : '?';
  }
  return label;
}

int comparePresentationPoints(const void *left, const void *right) {
  const VideoPresentationPoint &a = *static_cast<const VideoPresentationPoint *>(left);
  const VideoPresentationPoint &b = *static_cast<const VideoPresentationPoint *>(right);
  return a.timestamp < b.timestamp ? -1 : (a.timestamp > b.timestamp ? 1 : 0);
}
}

VideoPlayerScreen::VideoPlayerScreen(Navigator &navigator, TouchController &touch,
                                     DisplayController &display, SdCardController &sdCard,
                                     SpeakerController &speaker, SecretStorage &secretStorage)
  : Screen(navigator, touch, display), _sdCard(sdCard), _speaker(speaker), _secretStorage(secretStorage),
    _secretAccess(false), _demux(nullptr),
    _state(State::Ready), _audioCodec(AudioCodec::None), _videoCodec(VideoCodec::None), _sdMounted(false),
    _lastTouchDown(false), _sourceSize(0), _sourceWriteTime(0), _sourceWidth(0),
    _sourceHeight(0), _drawWidth(0), _drawHeight(0), _drawLeft(0), _drawTop(0),
    _videoTrack(0), _audioTrack(0), _videoSamples(0), _audioSamples(0), _videoSample(0),
    _audioSample(0), _videoTimeScale(0), _audioTimeScale(0), _buildFrames(0),
    _buildLastTimestamp(0), _buildLastDelay(0), _presentationTimeline(nullptr), _presentationCount(0),
    _presentationIndex(0), _buildFlushed(false), _playFrame(0),
    _playFrames(0), _playDelay(0), _playStartedAt(0), _nextFrameAt(0), _pauseElapsedMs(0),
    _pauseFrameRemainingMs(0), _durationMs(0),
    _lastDrawMs(0), _audioSampleRate(0), _audioFramesWritten(0), _audioTailStartedAt(0),
    _audioChannels(0), _sampleBuffer(nullptr),
    _sampleBufferBytes(0), _annexBuffer(nullptr), _annexBufferBytes(0), _pendingFrame(nullptr),
    _hasPendingFrame(false), _playFrameBuffer(nullptr), _audioScratch(nullptr), _aac(nullptr), _mp3(nullptr),
    _amr(nullptr) {
}

VideoPlayerScreen::~VideoPlayerScreen() {
  reset();
}

void VideoPlayerScreen::onEnter(const ScreenData &data) {
  reset();
  _lastTouchDown = false;
  _secretAccess = data.fileAccess == FileAccessMode::Secret;
  _sdMounted = _sdCard.isMounted();
  _path = data.kind == ScreenDataKind::VideoPath ? data.text : "";
  _fileName = displayName(_path);

  // The MP4 inspection/cache probe can touch a slow card.  Render a disabled
  // player first so the transition never leaves the Files screen frozen.
  _state = State::Building;
  draw();
  yield();
  _state = State::Ready;
  if (data.kind != ScreenDataKind::VideoPath || data.text.length() == 0) {
    fail(F("No video selected"));
  } else if (!_sdMounted) {
    fail(F("SD card unavailable"));
  } else {
    if (openSource() && inspectSource()) {
      _cachePath = cachePathForSource();
      _cacheTempPath = _cachePath + F(".tmp");
      if (loadCache()) {
        draw();
        startPlayback();
        return;
      }
      else if (!beginBuild()) draw();
    }
  }
  draw();
}

void VideoPlayerScreen::onExit() {
  reset();
}

void VideoPlayerScreen::handleTouch(const TouchPoint &point) {
  if (!point.touched) {
    _lastTouchDown = false;
    return;
  }
  if (_lastTouchDown) return;
  _lastTouchDown = true;
  if (UiKit::navigationIconContains(point.x, point.y)) {
    _navigator.pop();
    return;
  }
  if (!controlContains(point.x, point.y) || _state == State::Building || _state == State::Error) return;
  if (_state == State::Playing) pausePlayback();
  else if (_state == State::Ready || _state == State::Paused || _state == State::Finished) startPlayback();
}

void VideoPlayerScreen::tick(uint32_t nowMs) {
  handleSdCardEvent(_sdCard.takeEvent());
  if (_state == State::Building) {
    if (!buildStep()) {
      if (_state != State::Error) {
        const String &decoderError = _videoCodec == VideoCodec::H264 ? _h264.lastError() : _legacy.lastError();
        fail(decoderError.length() ? decoderError : String(F("Video conversion failed")));
      }
      draw();
    }
    return;
  }
  if (_state == State::Playing) tickPlayback(nowMs);
}

void VideoPlayerScreen::reset() {
  stopSpeaker();
  _h264.end();
  _legacy.end();
  releasePresentationTimeline();
  releaseAudioDecoder();
  releaseDemux();
  if (_source) _source.close();
  if (_cache) _cache.close();
  if (_cacheRead) _cacheRead.close();
  if (_state == State::Building && _cacheTempPath.length() && SpiBusLock::lock(pdMS_TO_TICKS(100))) {
    _secretStorage.remove(_cacheTempPath);
    SpiBusLock::unlock();
  }
  if (_sampleBuffer) free(_sampleBuffer);
  if (_annexBuffer) free(_annexBuffer);
  if (_pendingFrame) free(_pendingFrame);
  if (_playFrameBuffer) free(_playFrameBuffer);
  if (_audioScratch) free(_audioScratch);
  _sampleBuffer = _annexBuffer = nullptr;
  _pendingFrame = _playFrameBuffer = nullptr;
  _audioScratch = nullptr;
  _sampleBufferBytes = _annexBufferBytes = 0;
  _path = _fileName = _cachePath = _cacheTempPath = _error = _audioStatus = _audioDiagnostic = "";
  _state = State::Ready;
  _audioCodec = AudioCodec::None;
  _videoCodec = VideoCodec::None;
  _sourceSize = _sourceWriteTime = 0;
  _sourceWidth = _sourceHeight = _drawWidth = _drawHeight = 0;
  _drawLeft = _drawTop = 0;
  _videoTrack = _audioTrack = _videoSamples = _audioSamples = 0;
  _videoSample = _audioSample = _videoTimeScale = _audioTimeScale = 0;
  _buildFrames = _buildLastTimestamp = _buildLastDelay = 0;
  _presentationCount = _presentationIndex = 0;
  _buildFlushed = false;
  _playFrame = _playFrames = _playDelay = _playStartedAt = _nextFrameAt = _pauseElapsedMs =
    _pauseFrameRemainingMs = _durationMs = _lastDrawMs = 0;
  _audioSampleRate = _audioFramesWritten = _audioTailStartedAt = _audioChannels = 0;
  _hasPendingFrame = false;
}

void VideoPlayerScreen::releaseDemux() {
  if (_demux) {
    MP4D_close(_demux);
    delete _demux;
    _demux = nullptr;
  }
}

void VideoPlayerScreen::releaseAudioDecoder() {
  if (_aac) AACFreeDecoder((HAACDecoder)_aac);
  if (_mp3) free(_mp3);
  if (_amr) Decoder_Interface_exit(_amr);
  _aac = _mp3 = _amr = nullptr;
}

bool VideoPlayerScreen::openSource() {
  _source = _secretStorage.open(_path, FILE_READ, _secretAccess);
  if (!_source) return fail(F("Unable to open video"));
  _sourceSize = (uint32_t)_source.size();
  _sourceWriteTime = (uint32_t)_source.getLastWrite();
  if (_sourceSize < 12) return fail(F("Invalid MP4/3GP file"));
  return true;
}

bool VideoPlayerScreen::inspectSource() {
  releaseDemux();
  _demux = new MP4D_demux_t();
  if (!_demux || !MP4D_open(_demux, readMp4, &_source, _sourceSize)) return fail(F("Invalid MP4/3GP container"));
  bool videoFound = false;
  String unsupportedVideo;
  for (unsigned index = 0; index < _demux->track_count; index++) {
    const MP4D_track_t &track = _demux->track[index];
    if (!videoFound && track.handler_type == MP4D_HANDLER_TYPE_VIDE) {
      VideoCodec codec = VideoCodec::None;
      if (track.object_type_indication == MP4_OBJECT_TYPE_AVC) {
        codec = VideoCodec::H264;
      } else if (track.object_type_indication == MP4_OBJECT_TYPE_H263) {
        codec = VideoCodec::H263;
      } else if (track.object_type_indication == 0x20) {
        codec = VideoCodec::Mpeg4Part2;
      } else {
        unsupportedVideo = objectTypeLabel(track.object_type_indication);
        continue;
      }
      if (codec == VideoCodec::H264) {
        if (!track.dsi_bytes || track.avc_nal_length_size < 1 || track.avc_nal_length_size > 4) {
          return fail(F("Invalid H.264 config\nMissing AVC parameters"));
        }
        if (track.avc_profile != 66 && track.avc_profile != 77 && track.avc_profile != 100) {
          return fail(String(F("H.264 ")) + avcProfileName(track.avc_profile) + F(" (") +
                      String(track.avc_profile) + F(")\nNeed Baseline, Main, or High"));
        }
      } else if (codec == VideoCodec::H263 && (!track.dsi || track.dsi_bytes < 7)) {
        return fail(F("Malformed H.263 config"));
      }
      _videoCodec = codec;
      _videoTrack = index;
      _videoSamples = track.sample_count;
      _videoTimeScale = track.timescale;
      _sourceWidth = (uint16_t)track.SampleDescription.video.width;
      _sourceHeight = (uint16_t)track.SampleDescription.video.height;
      videoFound = _videoSamples > 0 && _videoTimeScale > 0;
      if (track.duration_lo) _durationMs = timestampMs(track.duration_lo, _videoTimeScale);
    } else if (_audioCodec != AudioCodec::Aac && _audioCodec != AudioCodec::Mp3 && _audioCodec != AudioCodec::AmrNb &&
               track.handler_type == MP4D_HANDLER_TYPE_SOUN) {
      _audioTrack = index;
      _audioSamples = track.sample_count;
      _audioTimeScale = track.timescale;
      _audioChannels = (uint16_t)track.SampleDescription.audio.channelcount;
      if (track.object_type_indication == MP4_OBJECT_TYPE_AUDIO_ISO_IEC_14496_3 && parseAacConfig()) {
        _audioCodec = AudioCodec::Aac;
        _audioDiagnostic = F("AAC-LC supported");
      } else if (track.object_type_indication == 0x6b) {
        _audioCodec = AudioCodec::Mp3;
        _audioDiagnostic = F("MPEG Layer III supported");
      } else if (track.object_type_indication == MP4_OBJECT_TYPE_AMR_NB) {
        _audioCodec = AudioCodec::AmrNb;
        _audioSampleRate = 8000;
        _audioChannels = 1;
        _audioDiagnostic = F("AMR-NB supported (8 kHz mono)");
      } else {
        _audioCodec = AudioCodec::Unsupported;
        String header = String(F("Unsupported ")) + sampleEntryLabel(track.sample_entry) +
                        F(" / ") + objectTypeLabel(track.object_type_indication);
        _audioDiagnostic = _audioDiagnostic.length() ? header + F(": ") + _audioDiagnostic : header;
      }
      logAudioTrack();
    }
  }
  if (!videoFound || !_sourceWidth || !_sourceHeight) {
    return fail(unsupportedVideo.length() ? String(F("Unsupported video\n")) + unsupportedVideo
                                          : String(F("No video track found")));
  }
  if (_sourceWidth > MaxVideoWidth || _sourceHeight > MaxVideoHeight) {
    return fail(String(F("Video ")) + String(_sourceWidth) + F("x") + String(_sourceHeight) +
                F(" exceeds\n320x240 maximum"));
  }
  _drawWidth = _sourceWidth;
  _drawHeight = _sourceHeight;
  _drawLeft = (_display.width() - _drawWidth) / 2;
  _drawTop = UiKit::AppBarHeight + 4 + (MaxVideoHeight - _drawHeight) / 2;
  if (_audioCodec == AudioCodec::Unsupported && _demux && _audioTrack < _demux->track_count) {
    const MP4D_track_t &track = _demux->track[_audioTrack];
    _audioStatus = String(F("Audio ")) + sampleEntryLabel(track.sample_entry) + F(" 0x") +
                   String(track.object_type_indication, HEX) + F(" unsupported");
  }
  return true;
}

bool VideoPlayerScreen::parseAacConfig() {
  if (!_demux || _audioTrack >= _demux->track_count) {
    _audioDiagnostic = F("AAC track is unavailable");
    return false;
  }
  const MP4D_track_t &track = _demux->track[_audioTrack];
  if (!track.dsi || track.dsi_bytes < 2) {
    _audioDiagnostic = F("AAC AudioSpecificConfig is missing");
    return false;
  }
  uint8_t objectType = track.dsi[0] >> 3;
  uint8_t rateIndex = (uint8_t)(((track.dsi[0] & 0x07) << 1) | (track.dsi[1] >> 7));
  uint8_t channels = (track.dsi[1] >> 3) & 0x0f;
  if (objectType != 2) {
    _audioDiagnostic = String(F("AAC object type ")) + String(objectType) + F(" (need LC 2)");
    return false;
  }
  if (rateIndex >= sizeof(AacSampleRates)) {
    _audioDiagnostic = String(F("AAC sample-rate index ")) + String(rateIndex) + F(" unsupported");
    return false;
  }
  if (channels == 0 || channels > 2) {
    _audioDiagnostic = String(F("AAC channel config ")) + String(channels) + F(" unsupported");
    return false;
  }
  _audioSampleRate = AacSampleRates[rateIndex];
  _audioChannels = channels;
  return true;
}

void VideoPlayerScreen::logAudioTrack() const {
  if (!_demux || _audioTrack >= _demux->track_count) return;
  const MP4D_track_t &track = _demux->track[_audioTrack];
  Serial.print(F("Video audio track: index="));
  Serial.print(_audioTrack);
  Serial.print(F(" entry="));
  Serial.print(sampleEntryLabel(track.sample_entry));
  Serial.print(F(" object=0x"));
  Serial.print(track.object_type_indication, HEX);
  Serial.print(F(" config="));
  Serial.print(track.dsi_bytes);
  Serial.print(F("B channels="));
  Serial.print(_audioChannels);
  Serial.print(F(" rate="));
  Serial.print(_audioSampleRate ? _audioSampleRate : track.SampleDescription.audio.samplerate_hz);
  Serial.print(F("Hz samples="));
  Serial.print(_audioSamples);
  Serial.print(F(" timescale="));
  Serial.print(_audioTimeScale);
  Serial.print(F(" result="));
  Serial.println(_audioDiagnostic);
  if (track.dsi && track.dsi_bytes) {
    Serial.print(F("Video audio config:"));
    unsigned bytes = track.dsi_bytes > 16 ? 16 : track.dsi_bytes;
    for (unsigned index = 0; index < bytes; index++) {
      Serial.print(' ');
      if (track.dsi[index] < 16) Serial.print('0');
      Serial.print(track.dsi[index], HEX);
    }
    if (bytes < track.dsi_bytes) Serial.print(F(" ..."));
    Serial.println();
  }
}

String VideoPlayerScreen::cachePathForSource() const {
  int slash = _path.lastIndexOf('/');
  String directory = slash <= 0 ? F("/") : _path.substring(0, slash);
  String name = slash < 0 ? _path : _path.substring(slash + 1);
  return directory + (directory == F("/") ? F("") : F("/")) + F(".cache/") + name + F(".emga");
}

bool VideoPlayerScreen::readCacheHeader(uint32_t &frames, uint32_t &duration) {
  frames = duration = 0;
  if (!_cacheRead || _cacheRead.size() < AnimationCache::HeaderBytes) return false;
  uint8_t header[AnimationCache::HeaderBytes] = {};
  if (!_cacheRead.seek(0) || _cacheRead.read(header, sizeof(header)) != sizeof(header) ||
      AnimationCache::readLe32(header) != AnimationCache::Magic || header[4] != AnimationCache::Version ||
      header[5] != 2 || AnimationCache::readLe16(header + 8) != AnimationCache::CanvasWidth ||
      AnimationCache::readLe16(header + 10) != AnimationCache::CanvasHeight ||
      AnimationCache::readLe32(header + 12) != _sourceSize ||
      AnimationCache::readLe32(header + 16) != _sourceWriteTime ||
      AnimationCache::readLe32(header + 20) != _sourceWidth ||
      AnimationCache::readLe32(header + 24) != _sourceHeight ||
      AnimationCache::readLe16(header + 36) != _drawWidth ||
      AnimationCache::readLe16(header + 38) != _drawHeight) return false;
  frames = AnimationCache::readLe32(header + 28);
  if (!frames) return false;
  uint64_t expected = AnimationCache::HeaderBytes + (uint64_t)frames * AnimationCache::frameBytes(_drawWidth, _drawHeight);
  if (expected != _cacheRead.size()) return false;
  for (uint32_t index = 0; index < frames; index++) {
    uint64_t offset = AnimationCache::HeaderBytes + (uint64_t)index * AnimationCache::frameBytes(_drawWidth, _drawHeight);
    uint8_t delay[4];
    if (!_cacheRead.seek((uint32_t)offset) || _cacheRead.read(delay, sizeof(delay)) != sizeof(delay)) return false;
    duration += AnimationCache::readLe32(delay);
  }
  return duration > 0;
}

bool VideoPlayerScreen::loadCache() {
  if (!psramFound() || !_secretStorage.exists(_cachePath)) return false;
  _cacheRead = _secretStorage.open(_cachePath, FILE_READ, _secretAccess);
  uint32_t frames = 0, duration = 0;
  if (!_cacheRead || !readCacheHeader(frames, duration)) {
    if (_cacheRead) _cacheRead.close();
    return false;
  }
  const size_t pixels = (size_t)_drawWidth * _drawHeight;
  _playFrameBuffer = (uint16_t *)ps_malloc(pixels * sizeof(uint16_t));
  if (!_playFrameBuffer) {
    _cacheRead.close();
    return false;
  }
  _playFrames = frames;
  _durationMs = duration;
  return true;
}

bool VideoPlayerScreen::beginBuild() {
  if (!psramFound()) return fail(F("Video needs PSRAM"));
  if (!preparePresentationTimeline()) return fail(F("Cannot build video timeline"));
  int slash = _cachePath.lastIndexOf('/');
  String directory = slash <= 0 ? F("/") : _cachePath.substring(0, slash);
  if (!_secretStorage.mkdir(directory)) return fail(F("Cannot create video cache"));
  if (_secretStorage.exists(_cacheTempPath) && !_secretStorage.remove(_cacheTempPath)) {
    return fail(F("Cannot replace incomplete cache"));
  }
  _cache = _secretStorage.open(_cacheTempPath, FILE_WRITE, _secretAccess);
  if (!_cache || !AnimationCache::writeHeader(_cache, _sourceSize, _sourceWriteTime, _sourceWidth,
                                               _sourceHeight, 0, -1, _drawWidth, _drawHeight)) {
    if (_cache) _cache.close();
    _secretStorage.remove(_cacheTempPath);
    return fail(F("Cannot start video cache"));
  }
  _pendingFrame = (uint16_t *)ps_malloc((size_t)_drawWidth * _drawHeight * sizeof(uint16_t));
  if (!_pendingFrame) return fail(F("Video needs too much memory"));
  if (_videoCodec == VideoCodec::H264) {
    if (!_h264.begin(_sourceWidth, _sourceHeight)) return fail(_h264.lastError());
  } else {
    if (!_demux || _videoTrack >= _demux->track_count) return fail(F("Video track unavailable"));
    const MP4D_track_t &track = _demux->track[_videoTrack];
    VideoLegacy::Codec codec = _videoCodec == VideoCodec::H263 ? VideoLegacy::Codec::H263
                                                                 : VideoLegacy::Codec::Mpeg4Part2;
    // The MPEG-4 Part 2 decoder requires the esds configuration record. H.263
    // samples carry their picture headers directly; d263 is descriptive only.
    const uint8_t *configuration = _videoCodec == VideoCodec::Mpeg4Part2 ? track.dsi : nullptr;
    uint32_t configurationBytes = configuration ? track.dsi_bytes : 0;
    if (!_legacy.begin(codec, _sourceWidth, _sourceHeight, configuration, configurationBytes)) {
      return fail(_legacy.lastError());
    }
  }
  _state = State::Building;
  _videoSample = _buildFrames = _buildLastTimestamp = _buildLastDelay = 0;
  _presentationIndex = 0;
  _buildFlushed = false;
  _hasPendingFrame = false;
  return true;
}

bool VideoPlayerScreen::preparePresentationTimeline() {
  releasePresentationTimeline();
  if (!_demux || _videoTrack >= _demux->track_count || !_videoSamples || !_videoTimeScale) return false;
  size_t bytes = (size_t)_videoSamples * sizeof(VideoPresentationPoint);
  if (bytes / sizeof(VideoPresentationPoint) != _videoSamples) return false;
  _presentationTimeline = static_cast<VideoPresentationPoint *>(ps_malloc(bytes));
  if (!_presentationTimeline) return false;
  for (uint32_t sample = 0; sample < _videoSamples; sample++) {
    int64_t timestamp = MP4D_frame_presentation_timestamp(_demux, _videoTrack, sample);
    if (timestamp == INT64_MIN) {
      releasePresentationTimeline();
      return false;
    }
    _presentationTimeline[sample].timestamp = timestamp;
  }
  qsort(_presentationTimeline, _videoSamples, sizeof(VideoPresentationPoint), comparePresentationPoints);
  // MP4 edit lists commonly shift the first decode timestamp below zero.
  // Cache time is relative, so normalize the earliest presentation picture to
  // zero while retaining all ctts-derived spacing and B-frame ordering.
  int64_t origin = _presentationTimeline[0].timestamp;
  for (uint32_t sample = 0; sample < _videoSamples; sample++)
    _presentationTimeline[sample].timestamp -= origin;
  _presentationCount = _videoSamples;
  return true;
}

void VideoPlayerScreen::releasePresentationTimeline() {
  if (_presentationTimeline) free(_presentationTimeline);
  _presentationTimeline = nullptr;
  _presentationCount = _presentationIndex = 0;
}

bool VideoPlayerScreen::ensureSampleBuffer(uint32_t bytes) {
  if (bytes <= _sampleBufferBytes) return true;
  uint8_t *replacement = (uint8_t *)(psramFound() ? ps_malloc(bytes) : malloc(bytes));
  if (!replacement) return false;
  if (_sampleBuffer) free(_sampleBuffer);
  _sampleBuffer = replacement;
  _sampleBufferBytes = bytes;
  return true;
}

bool VideoPlayerScreen::ensureAnnexBuffer(uint32_t bytes) {
  if (bytes <= _annexBufferBytes) return true;
  uint8_t *replacement = (uint8_t *)(psramFound() ? ps_malloc(bytes) : malloc(bytes));
  if (!replacement) return false;
  if (_annexBuffer) free(_annexBuffer);
  _annexBuffer = replacement;
  _annexBufferBytes = bytes;
  return true;
}

bool VideoPlayerScreen::readSample(uint32_t track, uint32_t sample, uint32_t &bytes,
                                    uint32_t &timestamp, uint32_t &duration) {
  bytes = timestamp = duration = 0;
  if (!_demux || track >= _demux->track_count || sample >= _demux->track[track].sample_count) return false;
  unsigned sampleBytes = 0, sampleTimestamp = 0, sampleDuration = 0;
  MP4D_file_offset_t offset = MP4D_frame_offset(_demux, track, sample, &sampleBytes, &sampleTimestamp, &sampleDuration);
  if (!sampleBytes || offset > 0xffffffffULL || !ensureSampleBuffer(sampleBytes) ||
      !SpiBusLock::lock(pdMS_TO_TICKS(100))) return false;
  bool ok = _source.seek((uint32_t)offset) && _source.read(_sampleBuffer, sampleBytes) == sampleBytes;
  SpiBusLock::unlock();
  if (!ok) return false;
  bytes = sampleBytes;
  timestamp = sampleTimestamp;
  duration = sampleDuration;
  return true;
}

bool VideoPlayerScreen::buildAnnexB(const uint8_t *sample, uint32_t bytes, bool includeParameters,
                                    uint32_t &outputBytes) {
  outputBytes = 0;
  if (!_demux || !sample || _videoTrack >= _demux->track_count) return false;
  const MP4D_track_t &track = _demux->track[_videoTrack];
  uint8_t lengthBytes = (uint8_t)track.avc_nal_length_size;
  if (lengthBytes < 1 || lengthBytes > 4 || !ensureAnnexBuffer(bytes + 8192)) return false;
  auto appendNal = [&](const uint8_t *nal, uint32_t nalBytes) -> bool {
    if (!nal || nalBytes == 0 || outputBytes > _annexBufferBytes - nalBytes - 4) return false;
    _annexBuffer[outputBytes++] = 0;
    _annexBuffer[outputBytes++] = 0;
    _annexBuffer[outputBytes++] = 0;
    _annexBuffer[outputBytes++] = 1;
    memcpy(_annexBuffer + outputBytes, nal, nalBytes);
    outputBytes += nalBytes;
    return true;
  };
  if (includeParameters) {
    for (int index = 0; ; index++) {
      int size = 0;
      const uint8_t *sps = (const uint8_t *)MP4D_read_sps(_demux, _videoTrack, index, &size);
      if (!sps) break;
      if (!appendNal(sps, (uint32_t)size)) return false;
    }
    for (int index = 0; ; index++) {
      int size = 0;
      const uint8_t *pps = (const uint8_t *)MP4D_read_pps(_demux, _videoTrack, index, &size);
      if (!pps) break;
      if (!appendNal(pps, (uint32_t)size)) return false;
    }
  }
  uint32_t offset = 0;
  while (offset + lengthBytes <= bytes) {
    uint32_t nalBytes = 0;
    for (uint8_t i = 0; i < lengthBytes; i++) nalBytes = (nalBytes << 8) | sample[offset + i];
    offset += lengthBytes;
    if (nalBytes == 0 || nalBytes > bytes - offset || !appendNal(sample + offset, nalBytes)) return false;
    offset += nalBytes;
  }
  return offset == bytes && outputBytes > 0;
}

bool VideoPlayerScreen::decodedFrame(const VideoH264::Frame &frame, void *context) {
  return context && static_cast<VideoPlayerScreen *>(context)->captureDecodedFrame(frame);
}

bool VideoPlayerScreen::captureDecodedFrame(const VideoH264::Frame &frame) {
  if (!_presentationTimeline || _presentationIndex >= _presentationCount || !_pendingFrame ||
      !frame.y || !frame.u || !frame.v || frame.width != _sourceWidth || frame.height != _sourceHeight ||
      frame.yStride < frame.width || frame.uStride < frame.width / 2 || frame.vStride < frame.width / 2) return false;
  uint32_t timestamp = presentationTimestampMs(_presentationTimeline[_presentationIndex++].timestamp, _videoTimeScale);
  bool keep = !_hasPendingFrame || timestamp - _buildLastTimestamp >= MinFrameDelayMs;
  if (keep && _hasPendingFrame && !appendPendingFrame(timestamp - _buildLastTimestamp)) return false;
  if (!keep) return true;
  for (uint16_t y = 0; y < _drawHeight; y++) {
    uint16_t sourceY = _drawHeight > 1 ? (uint32_t)y * (frame.height - 1) / (_drawHeight - 1) : 0;
    for (uint16_t x = 0; x < _drawWidth; x++) {
      uint16_t sourceX = _drawWidth > 1 ? (uint32_t)x * (frame.width - 1) / (_drawWidth - 1) : 0;
      int c = frame.y[(uint32_t)sourceY * frame.yStride + sourceX] - 16;
      int d = frame.u[(uint32_t)(sourceY / 2) * frame.uStride + sourceX / 2] - 128;
      int e = frame.v[(uint32_t)(sourceY / 2) * frame.vStride + sourceX / 2] - 128;
      uint8_t r = clampChannel((298 * c + 409 * e + 128) >> 8);
      uint8_t g = clampChannel((298 * c - 100 * d - 208 * e + 128) >> 8);
      uint8_t b = clampChannel((298 * c + 516 * d + 128) >> 8);
      _pendingFrame[(uint32_t)y * _drawWidth + x] = ((uint16_t)(r & 0xf8) << 8) |
                                                   ((uint16_t)(g & 0xfc) << 3) | (b >> 3);
    }
  }
  _hasPendingFrame = true;
  _buildLastTimestamp = timestamp;
  return true;
}

bool VideoPlayerScreen::writeFrameRgb444(const uint16_t *pixels, uint32_t delayMs) {
  if (!_cache || !pixels) return false;
  uint8_t delay[4];
  AnimationCache::writeLe32(delay, delayMs);
  if (!SpiBusLock::lock(pdMS_TO_TICKS(100))) return false;
  bool delayWritten = _cache.write(delay, sizeof(delay)) == sizeof(delay);
  SpiBusLock::unlock();
  if (!delayWritten) return false;
  const uint32_t total = AnimationCache::rgb444Bytes(_drawWidth, _drawHeight);
  for (uint32_t base = 0; base < total; ) {
    uint16_t count = total - base > IoChunkBytes ? IoChunkBytes : (uint16_t)(total - base);
    for (uint16_t i = 0; i < count; i++) {
      uint32_t position = base + i;
      uint32_t pair = position / 3;
      uint32_t firstIndex = pair * 2;
      uint16_t first = pixels[firstIndex];
      uint16_t second = firstIndex + 1 < (uint32_t)_drawWidth * _drawHeight ? pixels[firstIndex + 1] : 0;
      uint16_t a = (uint16_t)((first & 0xf000) >> 4) | (uint16_t)((first & 0x0780) >> 3) | (uint16_t)((first & 0x001e) >> 1);
      uint16_t b = (uint16_t)((second & 0xf000) >> 4) | (uint16_t)((second & 0x0780) >> 3) | (uint16_t)((second & 0x001e) >> 1);
      _cacheChunk[i] = position % 3 == 0 ? (uint8_t)(a >> 4) :
                       (position % 3 == 1 ? (uint8_t)((a << 4) | (b >> 8)) : (uint8_t)b);
    }
    if (!SpiBusLock::lock(pdMS_TO_TICKS(100))) return false;
    bool written = _cache.write(_cacheChunk, count) == count;
    SpiBusLock::unlock();
    if (!written) return false;
    base += count;
    taskYIELD();
  }
  return true;
}

bool VideoPlayerScreen::appendPendingFrame(uint32_t delayMs) {
  if (!_hasPendingFrame) return true;
  if (delayMs < MinFrameDelayMs) delayMs = MinFrameDelayMs;
  if (!writeFrameRgb444(_pendingFrame, delayMs)) return false;
  _buildFrames++;
  _buildLastDelay = delayMs;
  _hasPendingFrame = false;
  return true;
}

bool VideoPlayerScreen::buildStep() {
  if (_videoSample >= _videoSamples) {
    if (!_buildFlushed) {
      _buildFlushed = true;
      if (_videoCodec == VideoCodec::H264 ? !_h264.flush(decodedFrame, this)
                                          : !_legacy.flush(decodedFrame, this)) return false;
    }
    if (_presentationIndex != _presentationCount) return fail(F("Video output is incomplete"));
    return finishBuild();
  }
  uint32_t bytes, timestamp, duration;
  if (!readSample(_videoTrack, _videoSample, bytes, timestamp, duration)) return false;
  if (_videoCodec == VideoCodec::H264) {
    uint32_t annexBytes = 0;
    if (!buildAnnexB(_sampleBuffer, bytes, _videoSample == 0, annexBytes) ||
        !_h264.decode(_annexBuffer, annexBytes, _videoSample, decodedFrame, this)) return false;
  } else if (!_legacy.decode(_sampleBuffer, bytes, _videoSample, decodedFrame, this)) {
    return false;
  }
  _videoSample++;
  if (_videoSamples) {
    _lastDrawMs = millis();
    draw(false);
  }
  return true;
}

bool VideoPlayerScreen::finishBuild() {
  uint32_t finalDelay = _durationMs > _buildLastTimestamp ? _durationMs - _buildLastTimestamp : _buildLastDelay;
  if (!appendPendingFrame(finalDelay) || !_buildFrames || !_cache.seek(0) ||
      !AnimationCache::writeHeader(_cache, _sourceSize, _sourceWriteTime, _sourceWidth, _sourceHeight,
                                   _buildFrames, -1, _drawWidth, _drawHeight)) return false;
  _cache.close();
  if (_secretStorage.exists(_cachePath)) _secretStorage.remove(_cachePath);
  if (!_secretStorage.rename(_cacheTempPath, _cachePath)) return false;
  _h264.end();
  _legacy.end();
  if (_source) _source.close();
  releaseDemux();
  _cacheRead = _secretStorage.open(_cachePath, FILE_READ, _secretAccess);
  uint32_t frames = 0, duration = 0;
  if (!_cacheRead || !readCacheHeader(frames, duration)) return false;
  _playFrameBuffer = (uint16_t *)ps_malloc((size_t)_drawWidth * _drawHeight * sizeof(uint16_t));
  if (!_playFrameBuffer) return false;
  _playFrames = frames;
  _durationMs = duration;
  if (!openSource() || !inspectSource()) return false;
  _state = State::Ready;
  draw();
  return startPlayback();
}

bool VideoPlayerScreen::loadPlaybackFrame(uint32_t frame) {
  if (!_cacheRead || !_playFrameBuffer || frame >= _playFrames) return false;
  uint64_t offset = AnimationCache::HeaderBytes + (uint64_t)frame * AnimationCache::frameBytes(_drawWidth, _drawHeight);
  if (offset > 0xffffffffULL || !_cacheRead.seek((uint32_t)offset)) return false;
  uint8_t delay[4];
  if (_cacheRead.read(delay, sizeof(delay)) != sizeof(delay)) return false;
  _playDelay = AnimationCache::readLe32(delay);
  uint32_t total = AnimationCache::rgb444Bytes(_drawWidth, _drawHeight);
  uint8_t packed[3];
  uint8_t packedCount = 0;
  uint32_t pixel = 0;
  for (uint32_t base = 0; base < total; ) {
    uint16_t count = total - base > IoChunkBytes ? IoChunkBytes : (uint16_t)(total - base);
    if (_cacheRead.read(_cacheChunk, count) != count) return false;
    for (uint16_t i = 0; i < count; i++) {
      packed[packedCount++] = _cacheChunk[i];
      if (packedCount != 3) continue;
      uint16_t colors[] = { (uint16_t)((packed[0] << 4) | (packed[1] >> 4)),
                            (uint16_t)(((packed[1] & 0x0f) << 8) | packed[2]) };
      for (uint8_t color = 0; color < 2 && pixel < (uint32_t)_drawWidth * _drawHeight; color++, pixel++) {
        uint16_t rgb = colors[color];
        _playFrameBuffer[pixel] = (uint16_t)(((rgb & 0x0f00) << 4) | ((rgb & 0x00f0) << 3) | ((rgb & 0x000f) << 1));
      }
      packedCount = 0;
    }
    base += count;
    taskYIELD();
  }
  if (packedCount == 2 && pixel < (uint32_t)_drawWidth * _drawHeight) {
    uint16_t rgb = (uint16_t)((packed[0] << 4) | (packed[1] >> 4));
    _playFrameBuffer[pixel] = (uint16_t)(((rgb & 0x0f00) << 4) | ((rgb & 0x00f0) << 3) | ((rgb & 0x000f) << 1));
  }
  _display.tft().pushImage(_drawLeft, _drawTop, _drawWidth, _drawHeight, _playFrameBuffer);
  return true;
}

bool VideoPlayerScreen::startPlayback() {
  if (!_cacheRead || _state == State::Error || !_sdMounted) return false;
  if (_state == State::Paused) {
    if (_aac || _mp3 || _amr) {
      if (!_speaker.start(_audioSampleRate)) {
        _audioStatus = F("Speaker unavailable");
      }
    }
    _playStartedAt = millis() - _pauseElapsedMs;
    _nextFrameAt = _pauseElapsedMs + _pauseFrameRemainingMs;
    if (_audioTailStartedAt) _audioTailStartedAt = millis();
    _state = State::Playing;
    drawPlayButton();
    return true;
  }
  if (_state == State::Finished) {
    _playFrame = _audioSample = 0;
    _audioFramesWritten = 0;
    releaseAudioDecoder();
  }
  if (_playFrame >= _playFrames) _playFrame = 0;
  if (!loadPlaybackFrame(_playFrame)) return fail(F("Video cache read failed"));
  if (_audioCodec == AudioCodec::Aac) {
    _aac = AACInitDecoder();
    if (_aac) {
      AACFrameInfo info{};
      info.nChans = _audioChannels;
      info.sampRateCore = _audioSampleRate;
      info.profile = AAC_PROFILE_LC;
      if (AACSetRawBlockParams((HAACDecoder)_aac, 0, &info) != ERR_AAC_NONE) {
        releaseAudioDecoder();
        _audioStatus = F("Audio unavailable");
      }
    }
  } else if (_audioCodec == AudioCodec::Mp3) {
    _mp3 = malloc(sizeof(mp3dec_t));
    if (_mp3) mp3dec_init((mp3dec_t *)_mp3);
  } else if (_audioCodec == AudioCodec::AmrNb) {
    _amr = Decoder_Interface_init();
    if (!_amr) _audioStatus = F("AMR-NB decoder unavailable");
  }
  if ((_audioCodec == AudioCodec::Aac && _aac) || (_audioCodec == AudioCodec::Mp3 && _mp3) ||
      (_audioCodec == AudioCodec::AmrNb && _amr)) {
    if (!_speaker.start(_audioSampleRate)) _audioStatus = F("Speaker unavailable");
  }
  _playStartedAt = millis();
  _nextFrameAt = _playDelay;
  _pauseElapsedMs = 0;
  _pauseFrameRemainingMs = _playDelay;
  _state = State::Playing;
  drawPlayButton();
  drawProgress();
  return true;
}

void VideoPlayerScreen::pausePlayback() {
  const uint32_t now = millis();
  _pauseElapsedMs = playbackElapsedMs(now);
  _pauseFrameRemainingMs = (int32_t)(_nextFrameAt - _pauseElapsedMs) > 0
    ? _nextFrameAt - _pauseElapsedMs : 0;
  stopSpeaker();
  _state = State::Paused;
  drawPlayButton();
}

void VideoPlayerScreen::finishPlayback() {
  stopSpeaker();
  _playFrame = _playFrames;
  _state = State::Finished;
  drawPlayButton();
  drawProgress();
}

void VideoPlayerScreen::tickPlayback(uint32_t nowMs) {
  uint32_t elapsed = playbackElapsedMs(nowMs);
  pumpAudio(elapsed);
  elapsed = playbackElapsedMs(nowMs);
  if ((int32_t)(elapsed - _nextFrameAt) >= 0) {
    _playFrame++;
    if (_playFrame >= _playFrames) {
      finishPlayback();
      return;
    }
    if (!loadPlaybackFrame(_playFrame)) {
      fail(F("Video cache read failed"));
      draw();
      return;
    }
    _nextFrameAt += _playDelay;
  }
  if (nowMs - _lastDrawMs >= 200) {
    _lastDrawMs = nowMs;
    drawProgress();
    drawStatus();
  }
}

void VideoPlayerScreen::pumpAudio(uint32_t elapsedMs) {
  if ((_audioCodec != AudioCodec::Aac && _audioCodec != AudioCodec::Mp3 && _audioCodec != AudioCodec::AmrNb) ||
      (!_aac && !_mp3 && !_amr) || _audioSample >= _audioSamples || !_speaker.isReady()) return;
  if (!_demux || _audioTrack >= _demux->track_count) {
    _audioStatus = F("Audio unavailable");
    releaseAudioDecoder();
    return;
  }
  unsigned bytes = 0, timestamp = 0, duration = 0;
  MP4D_frame_offset(_demux, _audioTrack, _audioSample, &bytes, &timestamp, &duration);
  if (!bytes) {
    _audioStatus = F("Audio unavailable");
    releaseAudioDecoder();
    return;
  }
  if (timestampMs(timestamp, _audioTimeScale) > elapsedMs + 80) return;
  if (!decodeAudioSample()) {
    _audioStatus = _audioCodec == AudioCodec::AmrNb ? F("AMR-NB decode failed") : F("Audio unavailable");
    releaseAudioDecoder();
  }
  _audioSample++;
  if (_audioSample >= _audioSamples) _audioTailStartedAt = millis();
}

bool VideoPlayerScreen::decodeAudioSample() {
  uint32_t bytes, timestamp, duration;
  if (!readSample(_audioTrack, _audioSample, bytes, timestamp, duration)) return false;
  if (_audioCodec == AudioCodec::Aac) return decodeAacSample(_sampleBuffer, bytes);
  if (_audioCodec == AudioCodec::Mp3) return decodeMp3Sample(_sampleBuffer, bytes);
  return _audioCodec == AudioCodec::AmrNb && decodeAmrNbSample(_sampleBuffer, bytes);
}

bool VideoPlayerScreen::decodeAacSample(const uint8_t *bytes, uint32_t length) {
  if (!_aac || !bytes || !length) return false;
  if (!_audioScratch) _audioScratch = (int16_t *)ps_malloc(2048 * sizeof(int16_t));
  if (!_audioScratch) return false;
  unsigned char *input = const_cast<unsigned char *>(bytes);
  int remaining = (int)length;
  if (AACDecode((HAACDecoder)_aac, &input, &remaining, _audioScratch) != ERR_AAC_NONE) return false;
  AACFrameInfo info{};
  AACGetLastFrameInfo((HAACDecoder)_aac, &info);
  if (info.nChans < 1 || info.nChans > 2 || info.outputSamps < info.nChans) return false;
  if (!_audioSampleRate) _audioSampleRate = info.sampRateOut;
  if (info.sampRateOut && _speaker.sampleRate() != (uint32_t)info.sampRateOut &&
      !_speaker.setSampleRate((uint32_t)info.sampRateOut)) return false;
  uint32_t frames = info.outputSamps / info.nChans;
  for (uint32_t start = 0; start < frames; ) {
    uint16_t count = frames - start > PcmFrames ? PcmFrames : (uint16_t)(frames - start);
    for (uint16_t i = 0; i < count; i++) {
      int32_t value = _audioScratch[(start + i) * info.nChans];
      if (info.nChans == 2) value = (value + _audioScratch[(start + i) * 2 + 1]) / 2;
      _pcm[i] = (int16_t)value;
    }
    if (_speaker.writePcm16(_pcm, count, 100) != count) return false;
    _audioFramesWritten += count;
    start += count;
  }
  return true;
}

bool VideoPlayerScreen::decodeMp3Sample(const uint8_t *bytes, uint32_t length) {
  if (!_mp3 || !bytes || !length) return false;
  if (!_audioScratch) _audioScratch = (int16_t *)ps_malloc(MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(int16_t));
  if (!_audioScratch) return false;
  mp3dec_frame_info_t info{};
  int frames = mp3dec_decode_frame((mp3dec_t *)_mp3, bytes, (int)length, _audioScratch, &info);
  if (frames <= 0 || info.channels < 1 || info.channels > 2 || !info.hz) return false;
  if (!_audioSampleRate) _audioSampleRate = info.hz;
  if (_speaker.sampleRate() != (uint32_t)info.hz && !_speaker.setSampleRate((uint32_t)info.hz)) return false;
  for (int start = 0; start < frames; ) {
    uint16_t count = frames - start > PcmFrames ? PcmFrames : (uint16_t)(frames - start);
    for (uint16_t i = 0; i < count; i++) {
      int32_t value = _audioScratch[(start + i) * info.channels];
      if (info.channels == 2) value = (value + _audioScratch[(start + i) * 2 + 1]) / 2;
      _pcm[i] = (int16_t)value;
    }
    if (_speaker.writePcm16(_pcm, count, 100) != count) return false;
    _audioFramesWritten += count;
    start += count;
  }
  return true;
}

bool VideoPlayerScreen::decodeAmrNbSample(const uint8_t *bytes, uint32_t length) {
  // RFC 3267 octet-aligned AMR-NB: one ToC byte plus the packed payload.
  // 3GP `samr` samples contain exactly one such frame.
  static const uint8_t FrameBytes[] = { 13, 14, 16, 18, 20, 21, 27, 32, 6 };
  if (!_amr || !bytes || !length) return false;
  uint8_t frameType = (bytes[0] >> 3) & 0x0f;
  if (frameType <= 8) {
    if (length != FrameBytes[frameType]) return false;
  } else if (frameType == 15) {
    if (length != 1) return false; // no-data frame
  } else {
    return false; // AMR-WB and reserved AMR-NB frame types are unsupported
  }
  if (!_audioScratch) _audioScratch = (int16_t *)ps_malloc(160 * sizeof(int16_t));
  if (!_audioScratch) return false;
  Decoder_Interface_Decode(_amr, bytes, _audioScratch, 0);
  if (_speaker.writePcm16(_audioScratch, 160, 100) != 160) return false;
  _audioFramesWritten += 160;
  return true;
}

uint32_t VideoPlayerScreen::timestampMs(uint32_t timestamp, uint32_t timeScale) const {
  return timeScale ? (uint32_t)(((uint64_t)timestamp * 1000UL) / timeScale) : 0;
}

uint32_t VideoPlayerScreen::presentationTimestampMs(int64_t timestamp, uint32_t timeScale) const {
  if (timestamp < 0 || !timeScale) return 0;
  uint64_t milliseconds = ((uint64_t)timestamp * 1000UL) / timeScale;
  return milliseconds > 0xffffffffULL ? 0xffffffffUL : (uint32_t)milliseconds;
}

uint32_t VideoPlayerScreen::playbackElapsedMs(uint32_t nowMs) const {
  if ((_aac || _mp3 || _amr) && _audioSampleRate && _audioFramesWritten) {
    uint32_t elapsed = (uint32_t)(((uint64_t)_audioFramesWritten * 1000UL) / _audioSampleRate);
    // Once all source packets are queued, let the final DMA tail advance so
    // a slightly-shorter audio track cannot hold the last video frame forever.
    return _audioTailStartedAt ? elapsed + (nowMs - _audioTailStartedAt) : elapsed;
  }
  return nowMs - _playStartedAt;
}

bool VideoPlayerScreen::fail(const String &message) {
  stopSpeaker();
  if (_cache) _cache.close();
  if (_cacheTempPath.length() && _secretStorage.exists(_cacheTempPath)) _secretStorage.remove(_cacheTempPath);
  _error = message;
  _state = State::Error;
  return false;
}

bool VideoPlayerScreen::fail(const __FlashStringHelper *message) {
  return fail(String(message));
}

void VideoPlayerScreen::handleSdCardEvent(SdCardEvent event) {
  if (event == SdCardEvent::None) return;
  if (event == SdCardEvent::Removed || event == SdCardEvent::MountFailed) {
    _sdMounted = false;
    fail(event == SdCardEvent::Removed ? F("SD card removed") : F("SD card unavailable"));
    draw();
  } else if (event == SdCardEvent::Mounted) {
    _sdMounted = true;
  }
}

void VideoPlayerScreen::stopSpeaker() {
  _speaker.stop();
}

String VideoPlayerScreen::displayName(const String &path) const {
  int slash = path.lastIndexOf('/');
  return slash < 0 ? path : path.substring(slash + 1);
}

void VideoPlayerScreen::draw(bool full) {
  if (!_display.isReady()) return;
  TFT_eSPI &tft = _display.tft();
  if (full) {
    tft.fillScreen(TFT_BLACK);
    drawTopBar();
  }
  drawPlayButton();
  drawProgress();
  drawStatus();
  if (_state == State::Error) drawError();
}

void VideoPlayerScreen::drawTopBar() {
  UiKit::drawTopAppBar(_display.tft(), _display.width(), F("Video"));
}

void VideoPlayerScreen::drawPlayButton() {
  TFT_eSPI &tft = _display.tft();
  const int16_t cx = _display.width() - 30;
  const int16_t cy = UiKit::AppBarHeight + 18;
  const bool enabled = _state != State::Building && _state != State::Error && _sdMounted;
  tft.fillCircle(cx, cy, 15, enabled ? TFT_DARKGREY : TFT_BLACK);
  tft.drawCircle(cx, cy, 15, enabled ? TFT_WHITE : TFT_DARKGREY);
  if (!enabled) return;
  if (_state == State::Playing) {
    tft.fillRect(cx - 6, cy - 7, 4, 14, TFT_WHITE);
    tft.fillRect(cx + 2, cy - 7, 4, 14, TFT_WHITE);
  } else {
    tft.fillTriangle(cx - 4, cy - 8, cx - 4, cy + 8, cx + 8, cy, TFT_WHITE);
  }
}

void VideoPlayerScreen::drawProgress() {
  TFT_eSPI &tft = _display.tft();
  const int16_t x = 16, width = _display.width() - 32, y = _display.height() - 16;
  uint32_t elapsed = _state == State::Building && _videoSamples
    ? (uint32_t)(((uint64_t)_durationMs * _videoSample) / _videoSamples)
    : (_state == State::Playing ? playbackElapsedMs(millis())
       : (_state == State::Paused ? _pauseElapsedMs
          : (_state == State::Finished ? _durationMs : 0)));
  if (elapsed > _durationMs) elapsed = _durationMs;
  uint32_t filled = _durationMs ? (uint32_t)(((uint64_t)width * elapsed) / _durationMs) : 0;
  tft.fillRect(x, y, width, 6, TFT_DARKGREY);
  if (filled) tft.fillRect(x, y, filled, 6, TFT_CYAN);
}

void VideoPlayerScreen::drawStatus() {
  TFT_eSPI &tft = _display.tft();
  tft.fillRect(48, UiKit::AppBarHeight + 3, _display.width() - 96, 24, TFT_BLACK);
  String text;
  if (_state == State::Building) text = "Caching video " + String(_videoSample) + "/" + String(_videoSamples);
  else if (_audioStatus.length()) text = _audioStatus;
  else text = _fileName;
  UiText::draw(tft, UiKit::ellipsize(text, _display.width() - 96, UiText::Size::Compact),
               _display.width() / 2, UiKit::AppBarHeight + 14, TFT_LIGHTGREY, TFT_BLACK,
               UiText::Size::Compact, MC_DATUM);
}

void VideoPlayerScreen::drawError() {
  TFT_eSPI &tft = _display.tft();
  int newline = _error.indexOf('\n');
  String first = newline < 0 ? _error : _error.substring(0, newline);
  String second = newline < 0 ? String() : _error.substring(newline + 1);
  const int16_t centerX = _display.width() / 2;
  const int16_t centerY = _display.height() / 2;
  UiText::draw(tft, UiKit::ellipsize(first, _display.width() - 32, UiText::Size::Compact), centerX,
               second.length() ? centerY - 12 : centerY, TFT_RED, TFT_BLACK,
               UiText::Size::Compact, MC_DATUM);
  if (second.length()) {
    UiText::draw(tft, UiKit::ellipsize(second, _display.width() - 32, UiText::Size::Compact),
                 centerX, centerY + 12, TFT_LIGHTGREY, TFT_BLACK,
                 UiText::Size::Compact, MC_DATUM);
  }
}

bool VideoPlayerScreen::controlContains(int16_t x, int16_t y) const {
  int16_t cx = _display.width() - 30;
  int16_t cy = UiKit::AppBarHeight + 18;
  return x >= cx - 18 && x <= cx + 18 && y >= cy - 18 && y <= cy + 18;
}
