#define MINIMP4_IMPLEMENTATION
#include "../../../third_party/minimp4/minimp4.h"

#define MINIMP3_ONLY_MP3
#include "../../../third_party/minimp3/minimp3.h"

extern "C" {
#include "../../../third_party/libhelix/libhelix-aac/aacdec.h"
#include "../../../third_party/opencore-amr/amrnb/interf_dec.h"
}

#include "VideoPlayerScreen.h"

#include "../../app/Navigator.h"
#include "../../core/SpiBusLock.h"
#include "../../media/VideoH264.h"
#include "../../media/VideoLegacy.h"

#include <esp32-hal-psram.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <stdlib.h>
#include <string.h>

namespace {
constexpr uint16_t MaxVideoWidth = 320;
constexpr uint16_t MaxVideoHeight = 480;
constexpr uint8_t FrameSlots = 2;
constexpr uint16_t IoChunkBytes = 8192;
constexpr uint16_t PcmFrames = 512;
constexpr uint32_t AudioRingFrames = 16384;
constexpr uint32_t AudioStartMs = 80;
constexpr uint32_t AudioLowMs = 80;
constexpr uint32_t AudioTargetMs = 160;
constexpr uint16_t MaxI2sFramesPerPass = 256;
constexpr uint32_t WorkQuantumUs = 8000;
constexpr uint32_t LateFrameMs = 250;
constexpr uint32_t ForcedFrameMs = 250;
constexpr uint32_t SnapshotIntervalMs = 100;
constexpr uint32_t VideoWorkerStackBytes = 24576;
constexpr uint8_t MaxAudioPacketsPerQuantum = 4;

// Video workers are screen-scoped, but the state gate is process-lifetime.
// A decoder stack fault must never turn a corrupted per-worker semaphore
// pointer into a Core 1 LoadProhibited panic during UI polling.
SemaphoreHandle_t VideoStreamStateMutex = nullptr;

SemaphoreHandle_t videoStreamStateMutex() {
  return VideoStreamStateMutex;
}

const uint32_t AacSampleRates[] = {
  96000, 88200, 64000, 48000, 44100, 32000, 24000,
  22050, 16000, 12000, 11025, 8000, 7350
};

class StateLock {
public:
  explicit StateLock(SemaphoreHandle_t mutex)
      : _mutex(mutex), _locked(mutex && xSemaphoreTake(mutex, portMAX_DELAY) == pdTRUE) {}
  ~StateLock() { if (_locked) xSemaphoreGive(_mutex); }
  bool locked() const { return _locked; }
private:
  SemaphoreHandle_t _mutex;
  bool _locked;
};

int readMp4(int64_t offset, void *buffer, size_t bytes, void *token) {
  StorageFile *file = static_cast<StorageFile *>(token);
  if (!file || offset < 0 || offset > 0xffffffffLL || !SpiBusLock::lock(pdMS_TO_TICKS(20))) return 1;
  const bool ok = file->seek((uint32_t)offset) && file->read(buffer, bytes) == (int)bytes;
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
    case 100: return F("High");
    default: return F("profile");
  }
}

String objectTypeLabel(unsigned objectType) {
  if (!objectType) return F("unknown video sample entry");
  if (objectType == MP4_OBJECT_TYPE_H263) return F("H.263");
  if (objectType == MP4_OBJECT_TYPE_AMR_NB) return F("AMR-NB");
  if (objectType == MP4_OBJECT_TYPE_HEVC) return F("HEVC/H.265");
  if (objectType == 0x20) return F("MPEG-4 Part 2");
  if (objectType >= 0x60 && objectType <= 0x65) return F("MPEG-2 video");
  if (objectType == 0x6a) return F("MPEG-1 video");
  if (objectType == 0x6c) return F("Motion JPEG");
  return String(F("object 0x")) + String(objectType, HEX);
}

String fourCcLabel(unsigned value) {
  if (!value) return F("unknown");
  char text[5] = {
      (char)(value >> 24), (char)(value >> 16), (char)(value >> 8), (char)value, '\0'};
  for (uint8_t index = 0; index < 4; ++index) {
    if (text[index] < 0x20 || text[index] > 0x7e) return String(F("0x")) + String(value, HEX);
  }
  return String(text);
}

String describeVideoTrack(const MP4D_track_t &track) {
  String details = String(F("Codec: ")) + objectTypeLabel(track.object_type_indication);
  if (track.sample_entry) details += String(F(" (")) + fourCcLabel(track.sample_entry) + ')';
  if (track.object_type_indication) {
    details += String(F("\nObject type: 0x")) + String(track.object_type_indication, HEX);
  }
  const uint16_t width = (uint16_t)track.SampleDescription.video.width;
  const uint16_t height = (uint16_t)track.SampleDescription.video.height;
  if (width && height) details += String(F("\nVideo: ")) + width + 'x' + height;
  return details;
}

String unsupportedVideoError(const MP4D_track_t &track) {
  return String(F("Unsupported video codec\n")) + describeVideoTrack(track) +
         F("\nAdd a decoder for this codec");
}

constexpr uint32_t fourCc(char a, char b, char c, char d) {
  return ((uint32_t)(uint8_t)a << 24) | ((uint32_t)(uint8_t)b << 16) |
         ((uint32_t)(uint8_t)c << 8) | (uint8_t)d;
}

uint32_t readBigEndian32(const uint8_t *bytes) {
  return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) |
         ((uint32_t)bytes[2] << 8) | bytes[3];
}

// This deliberately tiny probe is separate from minimp4. It does not demux
// samples or accept a file for playback; it only finds the selected video
// sample entry when minimp4 rejected the container before exposing tracks.
class IsoBmffProbe {
public:
  struct Result {
    uint32_t majorBrand = 0;
    uint32_t videoEntry = 0;
    uint16_t width = 0;
    uint16_t height = 0;
  };

  IsoBmffProbe(StorageFile &source, uint32_t sourceSize) : _source(source), _sourceSize(sourceSize), _result{} {}

  Result inspect() {
    if (_sourceSize < 12) return _result;
    uint8_t header[12] = {};
    if (read(0, header, sizeof(header)) && readBigEndian32(header + 4) == fourCc('f', 't', 'y', 'p')) {
      _result.majorBrand = readBigEndian32(header + 8);
    }
    scanBoxes(0, _sourceSize, 0, nullptr);
    return _result;
  }

private:
  struct Box { uint32_t type = 0; uint32_t dataOffset = 0; uint32_t endOffset = 0; };
  struct Track { uint32_t handler = 0; uint32_t entry = 0; uint16_t width = 0; uint16_t height = 0; };
  static constexpr uint8_t MaxDepth = 8;
  static constexpr uint16_t MaxBoxes = 128;

  StorageFile &_source;
  uint32_t _sourceSize;
  Result _result;

  bool read(uint32_t offset, void *destination, uint32_t bytes) const {
    return bytes <= _sourceSize && offset <= _sourceSize - bytes &&
           readMp4(offset, destination, bytes, &_source) == 0;
  }

  bool readBox(uint32_t offset, uint32_t rangeEnd, Box &box) const {
    uint8_t header[16] = {};
    if (offset > rangeEnd || rangeEnd - offset < 8 || !read(offset, header, 8)) return false;
    uint64_t size = readBigEndian32(header);
    const uint32_t type = readBigEndian32(header + 4);
    uint32_t headerBytes = 8;
    if (size == 1) {
      if (rangeEnd - offset < 16 || !read(offset + 8, header + 8, 8)) return false;
      size = ((uint64_t)readBigEndian32(header + 8) << 32) | readBigEndian32(header + 12);
      headerBytes = 16;
    } else if (size == 0) {
      size = rangeEnd - offset;
    }
    if (size < headerBytes || size > rangeEnd - offset || size > UINT32_MAX) return false;
    box = {type, offset + headerBytes, offset + (uint32_t)size};
    return true;
  }

  static bool isContainer(uint32_t type) {
    return type == fourCc('m', 'o', 'o', 'v') || type == fourCc('m', 'd', 'i', 'a') ||
           type == fourCc('m', 'i', 'n', 'f') || type == fourCc('s', 't', 'b', 'l') ||
           type == fourCc('e', 'd', 't', 's') || type == fourCc('d', 'i', 'n', 'f');
  }

  void readHandler(const Box &box, Track &track) const {
    uint8_t bytes[4] = {};
    if (box.endOffset - box.dataOffset >= 12 && read(box.dataOffset + 8, bytes, sizeof(bytes))) {
      track.handler = readBigEndian32(bytes);
    }
  }

  void readSampleEntry(const Box &box, Track &track) const {
    uint8_t bytes[36] = {};
    // stsd: FullBox (4), entry count (4), then a sample entry size and FourCC.
    if (box.endOffset - box.dataOffset < 16 || !read(box.dataOffset + 8, bytes, 8)) return;
    const uint32_t entryBytes = readBigEndian32(bytes);
    if (entryBytes < 8 || entryBytes > box.endOffset - box.dataOffset - 8) return;
    track.entry = readBigEndian32(bytes + 4);
    // VisualSampleEntry width/height start 32 bytes after its box header.
    if (entryBytes >= 36 && read(box.dataOffset + 8, bytes, sizeof(bytes))) {
      track.width = (uint16_t)((bytes[32] << 8) | bytes[33]);
      track.height = (uint16_t)((bytes[34] << 8) | bytes[35]);
    }
  }

  void acceptTrack(const Track &track) {
    if (track.handler != MP4D_HANDLER_TYPE_VIDE || !track.entry || _result.videoEntry) return;
    _result.videoEntry = track.entry;
    _result.width = track.width;
    _result.height = track.height;
  }

  void scanBoxes(uint32_t offset, uint32_t rangeEnd, uint8_t depth, Track *track) {
    if (depth > MaxDepth) return;
    uint16_t boxes = 0;
    while (offset < rangeEnd && boxes++ < MaxBoxes) {
      Box box;
      if (!readBox(offset, rangeEnd, box) || box.endOffset <= offset) return;
      if (box.type == fourCc('t', 'r', 'a', 'k')) {
        Track child;
        scanBoxes(box.dataOffset, box.endOffset, depth + 1, &child);
        acceptTrack(child);
      } else if (track && box.type == fourCc('h', 'd', 'l', 'r')) {
        readHandler(box, *track);
      } else if (track && box.type == fourCc('s', 't', 's', 'd')) {
        readSampleEntry(box, *track);
      } else if (isContainer(box.type)) {
        scanBoxes(box.dataOffset, box.endOffset, depth + 1, track);
      }
      offset = box.endOffset;
    }
  }
};

String probeCodecLabel(uint32_t entry) {
  if (entry == fourCc('a', 'v', 'c', '1') || entry == fourCc('a', 'v', 'c', '3')) return F("H.264");
  if (entry == fourCc('h', 'v', 'c', '1') || entry == fourCc('h', 'e', 'v', '1')) return F("HEVC/H.265");
  if (entry == fourCc('v', 'p', '0', '9')) return F("VP9");
  if (entry == fourCc('a', 'v', '0', '1')) return F("AV1");
  if (entry == fourCc('m', 'p', '4', 'v')) return F("MPEG-4 Part 2");
  if (entry == fourCc('s', '2', '6', '3')) return F("H.263");
  if (entry == fourCc('m', 'j', 'p', '2') || entry == fourCc('j', 'p', 'e', 'g')) return F("Motion JPEG");
  return entry ? String(F("unknown")) : String(F("not found"));
}

bool probeCodecIsSupported(uint32_t entry) {
  return entry == fourCc('a', 'v', 'c', '1') || entry == fourCc('a', 'v', 'c', '3') ||
         entry == fourCc('m', 'p', '4', 'v') || entry == fourCc('s', '2', '6', '3');
}

String invalidContainerError(StorageFile &source, uint32_t sourceSize) {
  const IsoBmffProbe::Result result = IsoBmffProbe(source, sourceSize).inspect();
  String error = F("MP4/3GP parser rejected file");
  error += String(F("\nContainer: ")) + (result.majorBrand ? String(F("ISO-BMFF (")) + fourCcLabel(result.majorBrand) + ')'
                                                               : String(F("ftyp not found")));
  if (result.videoEntry) {
    error += String(F("\nCodec: ")) + probeCodecLabel(result.videoEntry) +
             String(F(" (")) + fourCcLabel(result.videoEntry) + ')';
    if (result.width && result.height) error += String(F("\nVideo: ")) + result.width + 'x' + result.height;
    error += probeCodecIsSupported(result.videoEntry)
                 ? F("\nCodec is supported; container/layout is not")
                 : F("\nAdd a decoder for this codec");
  } else {
    error += F("\nVideo sample entry not found");
  }
  return error;
}

int comparePresentationPoints(const void *left, const void *right) {
  const int64_t a = *static_cast<const int64_t *>(left);
  const int64_t b = *static_cast<const int64_t *>(right);
  return a < b ? -1 : (a > b ? 1 : 0);
}
}  // namespace

class VideoStreamWorker {
public:
  enum class Phase : uint8_t { Idle, Opening, Buffered, Playing, Paused, Finished, Error, Stopping };
  struct Snapshot {
    Phase phase = Phase::Idle;
    String status;
    String error;
    uint32_t durationMs = 0;
    uint32_t elapsedMs = 0;
    uint32_t droppedFrames = 0;
    uint16_t sourceWidth = 0;
    uint16_t sourceHeight = 0;
    bool hasAudio = false;
  };
  struct Frame {
    uint8_t slot = 0xff;
    const uint16_t *pixels = nullptr;
    uint32_t timestampMs = 0;
    uint16_t x = 0;
    uint16_t y = 0;
    uint16_t width = 0;
    uint16_t height = 0;
  };

  VideoStreamWorker(SpeakerController &speaker, SecretStorage &storage,
                    uint16_t viewportWidth, uint16_t viewportHeight)
      : _speaker(speaker), _storage(storage), _viewportWidth(viewportWidth), _viewportHeight(viewportHeight),
        _requestedViewportWidth(viewportWidth), _requestedViewportHeight(viewportHeight),
        _viewportReconfigurePending(false), _pauseAfterViewportReconfigure(false),
        _pauseAfterViewportFrame(false),
        _task(nullptr), _command(Command::None), _generation(0), _commandGeneration(0),
        _stopGeneration(0), _stopAcknowledgedGeneration(0), _secretAccess(false),
        _sourceSize(0), _demux(nullptr), _videoCodec(VideoCodec::None), _audioCodec(AudioCodec::None),
        _videoTrack(0), _audioTrack(0), _videoSamples(0), _audioSamples(0), _videoSample(0), _audioSample(0),
        _videoTimeScale(0), _audioTimeScale(0), _presentation(nullptr), _presentationCount(0),
        _presentationIndex(0), _videoFlushed(false), _videoFinished(false), _firstVideoTimestamp(0),
        _firstVideoTimestampSet(false), _sampleBuffer(nullptr), _sampleBufferBytes(0), _annexBuffer(nullptr),
        _annexBufferBytes(0), _audioBuffer(nullptr), _audioBufferBytes(0), _readWindow(nullptr),
        _readWindowStart(0), _readWindowBytes(0), _aac(nullptr), _mp3(nullptr), _amr(nullptr), _audioScratch(nullptr),
        _audioSampleRate(0), _audioChannels(0), _audioFramesSubmitted(0), _startedAt(0), _pausedElapsedMs(0),
        _playbackStarted(false), _pcmRing(nullptr), _pcmRead(0), _pcmWrite(0), _pcmCount(0),
        _sourceWidth(0), _sourceHeight(0), _drawX(0), _drawY(0), _drawWidth(0), _drawHeight(0),
        _phase(Phase::Idle), _durationMs(0), _droppedFrames(0), _presentationClockMs(0), _lastSnapshotMs(0),
        _lastProgressLogMs(0), _lastPresentedAtMs(0), _decodedSinceLog(0), _presentedSinceLog(0),
        _droppedSinceLog(0), _i2sZeroWrites(0), _sdReadCount(0), _sdReadBytes(0),
        _videoDecodeUs(0), _videoDecodeCount(0), _scaleUs(0), _scaleCount(0), _audioUnderrun(false),
        _ended(false), _slots{}, _snapshot{} {
    for (uint8_t index = 0; index < FrameSlots; ++index) _slots[index] = {};
  }

  ~VideoStreamWorker() { requestStop(); }

  bool begin() {
    if (_ended) return false;
    if (_task) return true;
    if (!VideoStreamStateMutex) VideoStreamStateMutex = xSemaphoreCreateMutex();
    if (!VideoStreamStateMutex) return false;
#if defined(CONFIG_FREERTOS_UNICORE) && CONFIG_FREERTOS_UNICORE
    const BaseType_t workerCore = 0;
#else
    const BaseType_t workerCore = ARDUINO_RUNNING_CORE == 0 ? 1 : 0;
#endif
    if (xTaskCreatePinnedToCore(taskMain, "video-stream", VideoWorkerStackBytes, this, tskIDLE_PRIORITY + 1,
                                &_task, workerCore) != pdPASS) {
      return false;
    }
    return true;
  }

  bool end() {
    if (_ended) return true;
    SemaphoreHandle_t mutex = videoStreamStateMutex();
    if (!mutex) return true;
    {
      StateLock lock(mutex);
      if (!lock.locked()) return false;
      if (!_stopGeneration) queueCommandLocked(Command::Stop);
      // A task state is not an acknowledgement: a notified task can still
      // report eBlocked until the scheduler runs it. The worker publishes this
      // exact command generation only after cleanup, immediately before it
      // deletes itself.
      if (!_stopGeneration || _stopAcknowledgedGeneration != _stopGeneration) return false;
      _ended = true;
      _task = nullptr;
    }
    return true;
  }

  bool prepare(const String &path, bool secretAccess) {
    if (_ended) return false;
    if (!begin()) return false;
    StateLock lock(videoStreamStateMutex());
    if (!lock.locked()) return false;
    ++_generation;
    if (_generation == 0) ++_generation;
    _requestedPath = path;
    _secretAccess = secretAccess;
    _command = Command::Prepare;
    _snapshot = {};
    _snapshot.phase = Phase::Opening;
    xTaskNotifyGive(_task);
    return true;
  }

  void play() { command(Command::Play); }
  void pause() { command(Command::Pause); }
  void replay() { command(Command::Replay); }
  void requestStop() { command(Command::Stop); }

  // The UI changes presentation geometry without disturbing source, decoder,
  // or audio-clock ownership. The worker applies the latest request only
  // between frame presentations, when no slot is borrowed by the UI.
  bool configureViewport(uint16_t width, uint16_t height, bool pauseAfterFrame = false) {
    if (_ended || !_task || !width || !height) return false;
    StateLock lock(videoStreamStateMutex());
    if (!lock.locked() || _stopGeneration) return false;
    _requestedViewportWidth = width;
    _requestedViewportHeight = height;
    _viewportReconfigurePending = true;
    _pauseAfterViewportReconfigure = _pauseAfterViewportReconfigure || pauseAfterFrame;
    xTaskNotifyGive(_task);
    return true;
  }

  Snapshot snapshot() const {
    Snapshot copy;
    if (_ended) return copy;
    StateLock lock(videoStreamStateMutex());
    if (lock.locked()) copy = _snapshot;
    return copy;
  }

  bool stopped() const {
    if (_ended || !_task) return true;
    StateLock lock(videoStreamStateMutex());
    return lock.locked() && _stopGeneration && _stopAcknowledgedGeneration == _stopGeneration;
  }

  bool takeFrame(Frame &frame) {
    if (_ended) return false;
    StateLock lock(videoStreamStateMutex());
    if (!lock.locked()) return false;
    // Hold old-geometry frames until the worker has replaced the packed slots;
    // the UI keeps its current canvas visible until a replacement arrives.
    if (_viewportReconfigurePending) return false;
    const uint32_t presentationClockMs = __atomic_load_n(&_presentationClockMs, __ATOMIC_ACQUIRE);
    uint8_t selected = 0xff;
    for (uint8_t index = 0; index < FrameSlots; ++index) {
      // A full-screen tap pauses only after a newly resized embedded frame is
      // ready. A paused clock no longer advances to that timestamp, so it is
      // deliberately eligible for this final presentation.
      if (_slots[index].state != SlotState::Ready ||
          (_phase != Phase::Paused && _slots[index].timestampMs > presentationClockMs)) continue;
      if (selected == 0xff || _slots[index].timestampMs < _slots[selected].timestampMs) selected = index;
    }
    if (selected == 0xff) return false;
    _slots[selected].state = SlotState::Presenting;
    frame = {selected, _slots[selected].pixels, _slots[selected].timestampMs,
             _drawX, _drawY, _drawWidth, _drawHeight};
    return true;
  }

  void releaseFrame(uint8_t slot, bool dropped) {
    if (_ended) return;
    StateLock lock(videoStreamStateMutex());
    if (!lock.locked() || slot >= FrameSlots || _slots[slot].state != SlotState::Presenting) return;
    _slots[slot].state = SlotState::Free;
    if (dropped) {
      __atomic_add_fetch(&_droppedFrames, 1, __ATOMIC_RELAXED);
      __atomic_add_fetch(&_droppedSinceLog, 1, __ATOMIC_RELAXED);
    } else {
      __atomic_store_n(&_lastPresentedAtMs, millis(), __ATOMIC_RELEASE);
      __atomic_add_fetch(&_presentedSinceLog, 1, __ATOMIC_RELAXED);
    }
  }

private:
  enum class Command : uint8_t { None, Prepare, Play, Pause, Replay, Stop };
  enum class VideoCodec : uint8_t { None, H264, H263, Mpeg4Part2 };
  enum class AudioCodec : uint8_t { None, Aac, Mp3, AmrNb, Unsupported };
  enum class SlotState : uint8_t { Free, Ready, Presenting };
  struct Slot { uint16_t *pixels = nullptr; uint32_t timestampMs = 0; SlotState state = SlotState::Free; };

  SpeakerController &_speaker;
  SecretStorage &_storage;
  uint16_t _viewportWidth;
  uint16_t _viewportHeight;
  uint16_t _requestedViewportWidth;
  uint16_t _requestedViewportHeight;
  bool _viewportReconfigurePending;
  bool _pauseAfterViewportReconfigure;
  bool _pauseAfterViewportFrame;
  TaskHandle_t _task;
  Command _command;
  uint32_t _generation;
  uint32_t _commandGeneration;
  uint32_t _stopGeneration;
  uint32_t _stopAcknowledgedGeneration;
  String _requestedPath;
  String _path;
  bool _secretAccess;
  uint32_t _sourceSize;
  StorageFile _source;
  MP4D_demux_t *_demux;
  VideoH264 _h264;
  VideoLegacy _legacy;
  VideoCodec _videoCodec;
  AudioCodec _audioCodec;
  uint32_t _videoTrack, _audioTrack, _videoSamples, _audioSamples, _videoSample, _audioSample;
  uint32_t _videoTimeScale, _audioTimeScale;
  int64_t *_presentation;
  uint32_t _presentationCount, _presentationIndex;
  bool _videoFlushed, _videoFinished, _firstVideoTimestampSet;
  uint32_t _firstVideoTimestamp;
  uint8_t *_sampleBuffer, *_annexBuffer;
  uint32_t _sampleBufferBytes, _annexBufferBytes;
  uint8_t *_audioBuffer;
  uint32_t _audioBufferBytes;
  uint8_t *_readWindow;
  uint32_t _readWindowStart, _readWindowBytes;
  void *_aac, *_mp3, *_amr;
  int16_t *_audioScratch;
  uint32_t _audioSampleRate, _audioChannels, _audioFramesSubmitted, _startedAt, _pausedElapsedMs;
  bool _playbackStarted;
  int16_t *_pcmRing;
  uint32_t _pcmRead, _pcmWrite, _pcmCount;
  uint16_t _sourceWidth, _sourceHeight, _drawX, _drawY, _drawWidth, _drawHeight;
  Phase _phase;
  uint32_t _durationMs, _droppedFrames, _presentationClockMs, _lastSnapshotMs, _lastProgressLogMs,
           _lastPresentedAtMs;
  uint32_t _decodedSinceLog, _presentedSinceLog, _droppedSinceLog, _i2sZeroWrites;
  uint32_t _sdReadCount, _sdReadBytes;
  uint64_t _videoDecodeUs, _scaleUs;
  uint32_t _videoDecodeCount, _scaleCount;
  bool _audioUnderrun;
  volatile bool _ended;
  Slot _slots[FrameSlots];
  Snapshot _snapshot;

  const char *videoCodecName() const {
    switch (_videoCodec) {
      case VideoCodec::H264: return "H.264";
      case VideoCodec::H263: return "H.263";
      case VideoCodec::Mpeg4Part2: return "MPEG-4 Part 2";
      default: return "none";
    }
  }

  const char *audioCodecName() const {
    switch (_audioCodec) {
      case AudioCodec::Aac: return "AAC-LC";
      case AudioCodec::Mp3: return "MP3";
      case AudioCodec::AmrNb: return "AMR-NB";
      default: return "none";
    }
  }

  static void taskMain(void *context) {
    static_cast<VideoStreamWorker *>(context)->run();
  }

  void command(Command commandValue) {
    if (_ended || !_task) return;
    StateLock lock(videoStreamStateMutex());
    if (!lock.locked()) return;
    queueCommandLocked(commandValue);
  }

  void queueCommandLocked(Command commandValue) {
    if (_command == Command::Stop && commandValue != Command::Stop) return;
    if (commandValue == Command::Stop && _stopGeneration) return;
    _command = commandValue;
    ++_commandGeneration;
    if (_commandGeneration == 0) ++_commandGeneration;
    if (commandValue == Command::Stop) _stopGeneration = _commandGeneration;
    Serial.printf("[Video] command=%u generation=%lu\n", static_cast<unsigned>(commandValue),
                  static_cast<unsigned long>(_commandGeneration));
    if (_task) xTaskNotifyGive(_task);
  }

  Command takeCommand(uint32_t &generation) {
    StateLock lock(videoStreamStateMutex());
    if (!lock.locked()) { generation = 0; return Command::None; }
    const Command result = _command;
    generation = _commandGeneration;
    _command = Command::None;
    return result;
  }

  bool stopPending() const {
    StateLock lock(videoStreamStateMutex());
    return lock.locked() && _command == Command::Stop;
  }

  void setPhase(Phase phase, const String &status = "") {
    if (_phase != phase) {
      Serial.printf("[Video] phase=%u status=%s\n", static_cast<unsigned>(phase), status.c_str());
    }
    _phase = phase;
    publishSnapshot(true, status);
  }

  void setError(const String &error) {
    Serial.print(F("Video stream failure: "));
    Serial.println(error);
    _speaker.stop();
    _phase = Phase::Error;
    StateLock lock(videoStreamStateMutex());
    if (!lock.locked()) return;
    _snapshot.phase = Phase::Error;
    _snapshot.error = error;
    _snapshot.status = F("Playback stopped");
  }

  void publishSnapshot(bool force = false, const String &status = "") {
    const uint32_t now = millis();
    if (!force && now - _lastSnapshotMs < SnapshotIntervalMs) return;
    _lastSnapshotMs = now;
    StateLock lock(videoStreamStateMutex());
    if (!lock.locked()) return;
    _snapshot.phase = _phase;
    _snapshot.durationMs = _durationMs;
    _snapshot.elapsedMs = _phase == Phase::Paused ? _pausedElapsedMs : playbackElapsedMs(false);
    _snapshot.droppedFrames = __atomic_load_n(&_droppedFrames, __ATOMIC_RELAXED);
    _snapshot.sourceWidth = _sourceWidth;
    _snapshot.sourceHeight = _sourceHeight;
    _snapshot.hasAudio = _audioCodec != AudioCodec::None && _audioSamples != 0;
    if (status.length()) _snapshot.status = status;
  }

  void run() {
    for (;;) {
      uint32_t generation = 0;
      const Command pending = takeCommand(generation);
      if (pending == Command::Stop) {
        stopAndExit(generation);
        return;
      }
      if (pending != Command::None) handleCommand(pending);
      reconfigureViewportIfReady();
      if (_phase == Phase::Playing) {
        pump();
      } else {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));
      }
    }
  }

  void handleCommand(Command commandValue) {
    switch (commandValue) {
      case Command::Prepare:
      case Command::Replay:
        cleanup();
        prepareSource();
        break;
      case Command::Play:
        if (_phase == Phase::Buffered || _phase == Phase::Paused) beginPlayback();
        break;
      case Command::Pause:
        pausePlayback();
        break;
      case Command::Stop:
        // Stop is handled in run() so the task can acknowledge it and exit.
        break;
      case Command::None:
        break;
    }
  }

  void pausePlayback() {
    if (_phase != Phase::Playing) return;
    _pausedElapsedMs = playbackElapsedMs(false);
    if (_audioCodec != AudioCodec::None && !_speaker.pauseOutput()) {
      Serial.printf("[Video] audio pause failed: %s\n", _speaker.lastError().c_str());
      disableAudio(F("Audio unavailable"));
    }
    Serial.printf("[Video] paused at=%lu\n", static_cast<unsigned long>(_pausedElapsedMs));
    setPhase(Phase::Paused, F("Paused"));
  }

  void releaseFrameSlots() {
    for (uint8_t index = 0; index < FrameSlots; ++index) {
      free(_slots[index].pixels);
      _slots[index] = {};
    }
  }

  void reconfigureViewportIfReady() {
    uint16_t width = 0;
    uint16_t height = 0;
    bool pauseAfterFrame = false;
    {
      StateLock lock(videoStreamStateMutex());
      if (!lock.locked() || !_viewportReconfigurePending || !_sourceWidth || !_sourceHeight) return;
      for (uint8_t index = 0; index < FrameSlots; ++index) {
        if (_slots[index].state == SlotState::Presenting) return;
      }
      width = _requestedViewportWidth;
      height = _requestedViewportHeight;
      _viewportReconfigurePending = false;
      pauseAfterFrame = _pauseAfterViewportReconfigure;
      _pauseAfterViewportReconfigure = false;
      if (width == _viewportWidth && height == _viewportHeight) {
        _pauseAfterViewportFrame = _pauseAfterViewportFrame || pauseAfterFrame;
        return;
      }
      // Invalidate ready frames while holding the state lock so the UI cannot
      // borrow an old-geometry buffer after the canvas has changed.
      for (uint8_t index = 0; index < FrameSlots; ++index) _slots[index].state = SlotState::Free;
    }
    _viewportWidth = width;
    _viewportHeight = height;
    releaseFrameSlots();
    if (!allocateSlots()) return;
    _pauseAfterViewportFrame = pauseAfterFrame;
    Serial.printf("[Video] viewport=%ux%u\n", _viewportWidth, _viewportHeight);
  }

  void stopAndExit(uint32_t generation) {
    setPhase(Phase::Stopping, F("Closing"));
    cleanup();
    {
      StateLock lock(videoStreamStateMutex());
      if (lock.locked()) {
        _snapshot.phase = Phase::Idle;
        _stopAcknowledgedGeneration = generation;
      }
    }
    // Do not access worker state after the acknowledgement: the UI may now
    // release the worker and its mutex. This task owns no further resources.
    vTaskDelete(nullptr);
  }

  bool prepareSource() {
    setPhase(Phase::Opening, F("Opening video"));
    if (!psramFound()) { setError(F("Video needs PSRAM")); return false; }
    bool havePath = false;
    {
      StateLock lock(videoStreamStateMutex());
      if (lock.locked() && _requestedPath.length()) {
        _path = _requestedPath;
        havePath = true;
      }
    }
    if (!havePath) { setError(F("No video selected")); return false; }
    _source = _storage.open(_path, FILE_READ, _secretAccess);
    _sourceSize = _source ? (uint32_t)_source.size() : 0;
    if (!_source || _sourceSize < 12) { setError(F("Unable to open video")); return false; }
    _demux = new MP4D_demux_t();
    if (!_demux || !MP4D_open(_demux, readMp4, &_source, _sourceSize)) {
      setError(invalidContainerError(_source, _sourceSize)); return false;
    }
    if (!inspectTracks() || !preparePresentationTimeline() || !allocateSlots() || !beginVideoDecoder() ||
        !initAudioDecoder() || !primeAudio()) return false;
    // Decode until both slots are ready before exposing the play action. This
    // is deliberately source-direct: no cache is read or written.
    while (readySlots() < FrameSlots && !_videoFinished) {
      if (!decodeOneVideo()) return false;
    }
    if (!readySlots()) { setError(F("Video contains no displayable frames")); return false; }
    Serial.printf("[Video] buffered frames=%u audioPrebuffer=%lu\n", readySlots(),
                  static_cast<unsigned long>(_pcmCount));
    setPhase(Phase::Buffered, _audioCodec == AudioCodec::None ? F("Ready") : F("Buffered"));
    return true;
  }

  bool inspectTracks() {
    bool videoFound = false;
    String unsupported;
    for (unsigned index = 0; index < _demux->track_count; ++index) {
      const MP4D_track_t &track = _demux->track[index];
      if (!videoFound && track.handler_type == MP4D_HANDLER_TYPE_VIDE) {
        VideoCodec codec = VideoCodec::None;
        if (track.object_type_indication == MP4_OBJECT_TYPE_AVC) codec = VideoCodec::H264;
        else if (track.object_type_indication == MP4_OBJECT_TYPE_H263) codec = VideoCodec::H263;
        else if (track.object_type_indication == 0x20) codec = VideoCodec::Mpeg4Part2;
        else { unsupported = unsupportedVideoError(track); continue; }
        if (codec == VideoCodec::H264 &&
            (!track.dsi_bytes || track.avc_nal_length_size < 1 || track.avc_nal_length_size > 4 ||
             (track.avc_profile != 66 && track.avc_profile != 77 && track.avc_profile != 100))) {
          String error = String(F("Unsupported H.264 video\n")) + describeVideoTrack(track);
          if (track.avc_profile) error += String(F("\nProfile: ")) + avcProfileName(track.avc_profile);
          else error += F("\nMissing or invalid AVC config");
          error += F("\nDecoder is present; this profile/config is not");
          setError(error);
          return false;
        }
        if (codec == VideoCodec::H263 && (!track.dsi || track.dsi_bytes < 7)) {
          setError(F("Malformed H.263 config")); return false;
        }
        const uint16_t width = (uint16_t)track.SampleDescription.video.width;
        const uint16_t height = (uint16_t)track.SampleDescription.video.height;
        if (!width || !height || width > MaxVideoWidth || height > MaxVideoHeight) {
          setError(String(F("Video ")) + width + 'x' + height + F(" exceeds\n320x480 maximum"));
          return false;
        }
        _videoCodec = codec;
        _videoTrack = index;
        _videoSamples = track.sample_count;
        _videoTimeScale = track.timescale;
        videoFound = _videoSamples && _videoTimeScale;
        _sourceWidth = width;
        _sourceHeight = height;
        _durationMs = track.duration_lo ? timestampMs(track.duration_lo, _videoTimeScale) : 0;
      } else if (_audioCodec == AudioCodec::None && track.handler_type == MP4D_HANDLER_TYPE_SOUN) {
        _audioTrack = index;
        _audioSamples = track.sample_count;
        _audioTimeScale = track.timescale;
        _audioChannels = track.SampleDescription.audio.channelcount;
        if (track.object_type_indication == MP4_OBJECT_TYPE_AUDIO_ISO_IEC_14496_3 && parseAacConfig()) {
          _audioCodec = AudioCodec::Aac;
        } else if (track.object_type_indication == 0x6b) {
          _audioCodec = AudioCodec::Mp3;
        } else if (track.object_type_indication == MP4_OBJECT_TYPE_AMR_NB) {
          _audioCodec = AudioCodec::AmrNb;
          _audioSampleRate = 8000;
          _audioChannels = 1;
        } else {
          _audioCodec = AudioCodec::Unsupported;
        }
      }
    }
    if (!videoFound) { setError(unsupported.length() ? unsupported
                                                       : String(F("No video track found"))); return false; }
    if (_audioCodec == AudioCodec::Unsupported) _audioCodec = AudioCodec::None;
    Serial.printf("[Video] metadata video=%s %ux%u samples=%lu timescale=%lu audio=%s samples=%lu timescale=%lu rate=%lu channels=%lu duration=%lu\n",
                  videoCodecName(), _sourceWidth, _sourceHeight,
                  static_cast<unsigned long>(_videoSamples), static_cast<unsigned long>(_videoTimeScale),
                  audioCodecName(), static_cast<unsigned long>(_audioSamples), static_cast<unsigned long>(_audioTimeScale),
                  static_cast<unsigned long>(_audioSampleRate), static_cast<unsigned long>(_audioChannels),
                  static_cast<unsigned long>(_durationMs));
    publishSnapshot(true);
    return true;
  }

  bool parseAacConfig() {
    if (!_demux || _audioTrack >= _demux->track_count) return false;
    const MP4D_track_t &track = _demux->track[_audioTrack];
    if (!track.dsi || track.dsi_bytes < 2) return false;
    const uint8_t objectType = track.dsi[0] >> 3;
    const uint8_t rateIndex = (uint8_t)(((track.dsi[0] & 0x07) << 1) | (track.dsi[1] >> 7));
    const uint8_t channels = (track.dsi[1] >> 3) & 0x0f;
    if (objectType != 2 || rateIndex >= sizeof(AacSampleRates) / sizeof(AacSampleRates[0]) ||
        channels < 1 || channels > 2) return false;
    _audioSampleRate = AacSampleRates[rateIndex];
    _audioChannels = channels;
    return true;
  }

  bool preparePresentationTimeline() {
    if (!_demux || !_videoSamples || !_videoTimeScale) return false;
    const size_t bytes = (size_t)_videoSamples * sizeof(int64_t);
    if (bytes / sizeof(int64_t) != _videoSamples) { setError(F("Video timeline too large")); return false; }
    _presentation = static_cast<int64_t *>(ps_malloc(bytes));
    if (!_presentation) { setError(F("Video needs more PSRAM")); return false; }
    for (uint32_t sample = 0; sample < _videoSamples; ++sample)
      _presentation[sample] = MP4D_frame_presentation_timestamp(_demux, _videoTrack, sample);
    qsort(_presentation, _videoSamples, sizeof(int64_t), comparePresentationPoints);
    _presentationCount = _videoSamples;
    _presentationIndex = 0;
    return true;
  }

  bool allocateSlots() {
    _drawWidth = _viewportWidth;
    _drawHeight = _viewportHeight;
    _drawX = _drawY = 0;
    if ((uint32_t)_sourceWidth * _viewportHeight > (uint32_t)_sourceHeight * _viewportWidth) {
      _drawHeight = max<uint32_t>(1, ((uint32_t)_sourceHeight * _viewportWidth) / _sourceWidth);
      _drawY = (_viewportHeight - _drawHeight) / 2;
    } else {
      _drawWidth = max<uint32_t>(1, ((uint32_t)_sourceWidth * _viewportHeight) / _sourceHeight);
      _drawX = (_viewportWidth - _drawWidth) / 2;
    }
    const size_t bytes = (size_t)_drawWidth * _drawHeight * sizeof(uint16_t);
    for (uint8_t index = 0; index < FrameSlots; ++index) {
      _slots[index].pixels = static_cast<uint16_t *>(ps_malloc(bytes));
      if (!_slots[index].pixels) { setError(F("Video frame buffers need more PSRAM")); return false; }
      _slots[index].state = SlotState::Free;
    }
    Serial.printf("[Video] presentation rect=%u,%u %ux%u slotBytes=%lu\n", _drawX, _drawY,
                  _drawWidth, _drawHeight, static_cast<unsigned long>(bytes));
    return true;
  }

  bool beginVideoDecoder() {
    if (_videoCodec == VideoCodec::H264) {
      if (!_h264.begin(MaxVideoWidth, MaxVideoHeight)) { setError(_h264.lastError()); return false; }
    } else {
      const MP4D_track_t &track = _demux->track[_videoTrack];
      const VideoLegacy::Codec codec = _videoCodec == VideoCodec::H263 ? VideoLegacy::Codec::H263
                                                                          : VideoLegacy::Codec::Mpeg4Part2;
      const uint8_t *configuration = _videoCodec == VideoCodec::Mpeg4Part2 ? track.dsi : nullptr;
      const uint32_t configurationBytes = _videoCodec == VideoCodec::Mpeg4Part2 ? track.dsi_bytes : 0;
      if (!_legacy.begin(codec, MaxVideoWidth, MaxVideoHeight, configuration, configurationBytes)) {
        setError(_legacy.lastError()); return false;
      }
    }
    return true;
  }

  bool beginPlayback() {
    const bool resuming = _playbackStarted;
    if (_audioCodec != AudioCodec::None) {
      const bool audioReady = resuming ? _speaker.resumeOutput() : _speaker.start(_audioSampleRate);
      if (!audioReady) {
        Serial.printf("[Video] audio %s failed: %s\n", resuming ? "resume" : "start",
                      _speaker.lastError().c_str());
        disableAudio(F("Audio unavailable"));
      }
    }
    if (resuming) {
      _startedAt = millis() - _pausedElapsedMs;
    } else {
      _startedAt = millis();
      _audioFramesSubmitted = 0;
      _playbackStarted = true;
    }
    Serial.printf("[Video] play resume=%s audio=%s rate=%lu prebuffer=%lu\n", resuming ? "yes" : "no", audioCodecName(),
                  static_cast<unsigned long>(_audioSampleRate), static_cast<unsigned long>(_pcmCount));
    setPhase(Phase::Playing, F("Playing"));
    return true;
  }

  bool initAudioDecoder() {
    releaseAudioDecoder();
    if (_audioCodec == AudioCodec::None) return true;
    if (_audioCodec == AudioCodec::Aac) {
      _aac = AACInitDecoder();
      if (_aac) {
        AACFrameInfo info{};
        info.nChans = _audioChannels;
        info.sampRateCore = _audioSampleRate;
        info.profile = AAC_PROFILE_LC;
        if (AACSetRawBlockParams((HAACDecoder)_aac, 0, &info) != ERR_AAC_NONE) _aac = nullptr;
      }
    } else if (_audioCodec == AudioCodec::Mp3) {
      _mp3 = malloc(sizeof(mp3dec_t));
      if (_mp3) mp3dec_init(static_cast<mp3dec_t *>(_mp3));
    } else if (_audioCodec == AudioCodec::AmrNb) {
      _amr = Decoder_Interface_init();
    }
    if ((_audioCodec == AudioCodec::Aac && !_aac) || (_audioCodec == AudioCodec::Mp3 && !_mp3) ||
        (_audioCodec == AudioCodec::AmrNb && !_amr)) {
      setError(F("Audio decoder unavailable"));
      return false;
    }
    return true;
  }

  bool primeAudio() {
    if (_audioCodec == AudioCodec::None || !_audioSamples) return true;
    _pcmRing = static_cast<int16_t *>(ps_malloc(AudioRingFrames * sizeof(int16_t)));
    if (!_pcmRing) {
      setError(F("Audio buffer needs more PSRAM"));
      return false;
    }
    _pcmRead = _pcmWrite = _pcmCount = 0;
    while (_audioSample < _audioSamples && (!_audioSampleRate ||
           ringFillMs() < AudioStartMs)) {
      const uint32_t before = _audioSample;
      if (!decodeNextAudioPacket() || _audioSample == before) break;
    }
    if (!_pcmCount) disableAudio(F("Audio unavailable"));
    return true;
  }

  void pump() {
    const uint32_t quantumStartedUs = micros();
    uint32_t generation = 0;
    const Command pending = takeCommand(generation);
    if (pending == Command::Stop) { stopAndExit(generation); return; }
    if (pending != Command::None) { handleCommand(pending); return; }
    reconfigureViewportIfReady();
    const uint32_t elapsed = playbackElapsedMs(false);
    __atomic_store_n(&_presentationClockMs, elapsed, __ATOMIC_RELEASE);
    if (_audioCodec != AudioCodec::None) {
      drainPcmToI2s();
      refillAudio(elapsed);
    }
    if (!_videoFinished && freeSlots()) {
      const uint32_t decodeStartedUs = micros();
      if (!decodeOneVideo()) return;
      _videoDecodeUs += micros() - decodeStartedUs;
      ++_videoDecodeCount;
      ++_decodedSinceLog;
    }
    if (_audioCodec != AudioCodec::None && (uint32_t)(micros() - quantumStartedUs) < WorkQuantumUs) {
      drainPcmToI2s();
    }
    updateUnderrunStatus();
    publishSnapshot();
    logPerformance(elapsed);
    const bool audioFinished = _audioCodec == AudioCodec::None ||
                               (_audioSample >= _audioSamples && _pcmCount == 0 &&
                                playbackElapsedMs(false) >= submittedAudioMs());
    if (_videoFinished && audioFinished) {
      _speaker.stop();
      setPhase(Phase::Finished, F("Finished"));
      return;
    }
    // A one-tick block also protects IDLE0 when the TFT has both slots and I2S
    // is applying backpressure; taskYIELD alone cannot run a lower-priority
    // idle task and previously allowed the watchdog to fire.
    vTaskDelay(1);
  }

  bool decodeOneVideo() {
    if (_videoSample >= _videoSamples) {
      if (!_videoFlushed) {
        _videoFlushed = true;
        const bool flushed = _videoCodec == VideoCodec::H264 ? _h264.flush(decodedFrame, this)
                                                              : _legacy.flush(decodedFrame, this);
        if (!flushed) { setError(_videoCodec == VideoCodec::H264 ? _h264.lastError() : _legacy.lastError()); return false; }
      }
      _videoFinished = true;
      return true;
    }
    uint32_t bytes = 0, timestamp = 0, duration = 0;
    if (!readSample(_videoTrack, _videoSample, bytes, timestamp, duration)) {
      setError(F("Video sample read failed")); return false;
    }
    bool decoded = false;
    if (_videoCodec == VideoCodec::H264) {
      uint32_t annexBytes = 0;
      decoded = buildAnnexB(_sampleBuffer, bytes, _videoSample == 0, annexBytes) &&
                _h264.decode(_annexBuffer, annexBytes, _videoSample, decodedFrame, this);
    } else {
      decoded = _legacy.decode(_sampleBuffer, bytes, _videoSample, decodedFrame, this);
    }
    if (!decoded) { setError(_videoCodec == VideoCodec::H264 ? _h264.lastError() : _legacy.lastError()); return false; }
    ++_videoSample;
    return true;
  }

  static bool decodedFrame(const VideoH264::Frame &frame, void *context) {
    return context && static_cast<VideoStreamWorker *>(context)->captureFrame(frame);
  }

  bool captureFrame(const VideoH264::Frame &frame) {
    if (!_presentation || _presentationIndex >= _presentationCount || !frame.y || !frame.u || !frame.v ||
        frame.width != _sourceWidth || frame.height != _sourceHeight ||
        frame.yStride < frame.width || frame.uStride < (frame.width + 1) / 2 || frame.vStride < (frame.width + 1) / 2) {
      return false;
    }
    const int64_t rawTimestamp = _presentation[_presentationIndex++];
    const uint32_t timestamp = presentationTimestampMs(rawTimestamp, _videoTimeScale);
    if (!_firstVideoTimestampSet) { _firstVideoTimestampSet = true; _firstVideoTimestamp = timestamp; }
    const uint32_t relativeTimestamp = timestamp - _firstVideoTimestamp;
    // Decode reference frames to keep the codec healthy, but skip their
    // expensive RGB conversion when they are already behind the audio clock.
    // This lets a slow software decoder catch up instead of converting frames
    // that the UI would immediately discard.
    const uint32_t now = millis();
    const bool overdueForPresentation =
      now - __atomic_load_n(&_lastPresentedAtMs, __ATOMIC_ACQUIRE) >= ForcedFrameMs;
    if (_phase == Phase::Playing && !overdueForPresentation &&
        (int32_t)(playbackElapsedMs(false) - relativeTimestamp) > (int32_t)LateFrameMs) {
      __atomic_add_fetch(&_droppedFrames, 1, __ATOMIC_RELAXED);
      __atomic_add_fetch(&_droppedSinceLog, 1, __ATOMIC_RELAXED);
      return true;
    }
    uint8_t slot = 0xff;
    for (;;) {
      {
        StateLock lock(videoStreamStateMutex());
        if (lock.locked()) {
          for (uint8_t index = 0; index < FrameSlots; ++index) {
            if (_slots[index].state == SlotState::Free) { slot = index; _slots[index].state = SlotState::Presenting; break; }
          }
        }
      }
      if (slot != 0xff) break;
      if (stopPending()) return false;
      vTaskDelay(1);
    }
    const uint32_t scaleStartedUs = micros();
    scaleFrame(frame, _slots[slot].pixels);
    _scaleUs += micros() - scaleStartedUs;
    ++_scaleCount;
    bool pauseAfterFrame = false;
    {
      StateLock lock(videoStreamStateMutex());
      if (!lock.locked()) return false;
      _slots[slot].timestampMs = relativeTimestamp;
      _slots[slot].state = SlotState::Ready;
      pauseAfterFrame = _pauseAfterViewportFrame;
      _pauseAfterViewportFrame = false;
    }
    if (pauseAfterFrame) pausePlayback();
    return true;
  }

  void scaleFrame(const VideoH264::Frame &frame, uint16_t *destination) {
    const uint32_t sourceW = frame.width, sourceH = frame.height;
    const uint32_t xStep = (sourceW << 16) / _drawWidth;
    const uint32_t yStep = (sourceH << 16) / _drawHeight;
    uint32_t sourceYFixed = 0;
    for (uint32_t y = 0; y < _drawHeight; ++y) {
      // 16.16 nearest-neighbour mapping keeps aspect ratio without division
      // in the pixel loop. This is materially faster on ESP32-S3.
      const uint32_t sourceY = min<uint32_t>(sourceH - 1, sourceYFixed >> 16);
      uint32_t sourceXFixed = 0;
      for (uint32_t x = 0; x < _drawWidth; ++x) {
        const uint32_t sourceX = min<uint32_t>(sourceW - 1, sourceXFixed >> 16);
        const int c = frame.y[sourceY * frame.yStride + sourceX] - 16;
        const int d = frame.u[(sourceY / 2) * frame.uStride + sourceX / 2] - 128;
        const int e = frame.v[(sourceY / 2) * frame.vStride + sourceX / 2] - 128;
        const uint8_t r = clampChannel((298 * c + 409 * e + 128) >> 8);
        const uint8_t g = clampChannel((298 * c - 100 * d - 208 * e + 128) >> 8);
        const uint8_t b = clampChannel((298 * c + 516 * d + 128) >> 8);
        destination[y * _drawWidth + x] = ((uint16_t)(r & 0xf8) << 8) |
                                          ((uint16_t)(g & 0xfc) << 3) | (b >> 3);
        sourceXFixed += xStep;
      }
      sourceYFixed += yStep;
    }
  }

  bool decodeNextAudioPacket() {
    if (_audioSample >= _audioSamples || !_demux) return true;
    uint32_t bytes = 0, timestamp = 0, duration = 0;
    if (!readAudioSample(bytes, timestamp, duration)) {
      Serial.printf("[Video] audio read failed sample=%lu codec=%s error=%s\n",
                    static_cast<unsigned long>(_audioSample), audioCodecName(), _speaker.lastError().c_str());
      disableAudio(F("Audio unavailable"));
      return false;
    }
    if (!decodeAudio(_audioBuffer, bytes)) {
      // Compressed samples are independently framed. A malformed packet is
      // skipped, while I2S backpressure never reaches this decoder path.
      Serial.printf("[Video] audio decode skipped sample=%lu codec=%s bytes=%lu toc=0x%02x error=%s\n",
                    static_cast<unsigned long>(_audioSample), audioCodecName(), static_cast<unsigned long>(bytes),
                    bytes ? _audioBuffer[0] : 0, _speaker.lastError().c_str());
      ++_audioSample;
      return true;
    }
    ++_audioSample;
    return true;
  }

  void refillAudio(uint32_t elapsedMs) {
    if (!_pcmRing || !_audioSampleRate || ringFillMs() >= AudioLowMs) return;
    for (uint8_t packet = 0; packet < MaxAudioPacketsPerQuantum &&
                              _audioCodec != AudioCodec::None && _audioSample < _audioSamples; ++packet) {
      const uint32_t bufferedEndMs = submittedAudioMs() + ringFillMs();
      if (ringFillMs() >= AudioTargetMs && bufferedEndMs >= elapsedMs + AudioTargetMs) break;
      const uint32_t before = _audioSample;
      if (!decodeNextAudioPacket() || _audioSample == before) break;
    }
  }

  void drainPcmToI2s() {
    if (!_pcmRing || !_pcmCount || !_speaker.isReady()) return;
    uint32_t count = min<uint32_t>(_pcmCount, AudioRingFrames - _pcmRead);
    count = min<uint32_t>(count, MaxI2sFramesPerPass);
    const size_t written = _speaker.tryWritePcm16(_pcmRing + _pcmRead, count);
    if (!written) {
      ++_i2sZeroWrites;
      return;
    }
    _pcmRead = (_pcmRead + written) % AudioRingFrames;
    _pcmCount -= written;
    _audioFramesSubmitted += written;
  }

  bool decodeAudio(const uint8_t *bytes, uint32_t length) {
    if (_audioCodec == AudioCodec::Aac) return decodeAac(bytes, length);
    if (_audioCodec == AudioCodec::Mp3) return decodeMp3(bytes, length);
    return _audioCodec == AudioCodec::AmrNb && decodeAmr(bytes, length);
  }

  bool writeMono(const int16_t *samples, uint32_t frames, uint8_t channels) {
    if (!samples || !frames || channels < 1 || channels > 2 || !_pcmRing ||
        frames > AudioRingFrames - _pcmCount) return false;
    int16_t pcm[PcmFrames];
    for (uint32_t start = 0; start < frames; ) {
      const uint16_t count = frames - start > PcmFrames ? PcmFrames : (uint16_t)(frames - start);
      for (uint16_t i = 0; i < count; ++i) {
        int32_t value = samples[(start + i) * channels];
        if (channels == 2) value = (value + samples[(start + i) * 2 + 1]) / 2;
        pcm[i] = (int16_t)value;
      }
      const uint32_t first = min<uint32_t>(count, AudioRingFrames - _pcmWrite);
      memcpy(_pcmRing + _pcmWrite, pcm, first * sizeof(int16_t));
      if (first < count) memcpy(_pcmRing, pcm + first, (count - first) * sizeof(int16_t));
      _pcmWrite = (_pcmWrite + count) % AudioRingFrames;
      _pcmCount += count;
      start += count;
    }
    return true;
  }

  bool decodeAac(const uint8_t *bytes, uint32_t length) {
    if (!_aac || !bytes || !length) return false;
    if (!_audioScratch) _audioScratch = static_cast<int16_t *>(ps_malloc(2048 * sizeof(int16_t)));
    if (!_audioScratch) return false;
    unsigned char *input = const_cast<unsigned char *>(bytes);
    int remaining = (int)length;
    if (AACDecode((HAACDecoder)_aac, &input, &remaining, _audioScratch) != ERR_AAC_NONE) return false;
    AACFrameInfo info{};
    AACGetLastFrameInfo((HAACDecoder)_aac, &info);
    if (info.nChans < 1 || info.nChans > 2 || info.outputSamps < info.nChans) return false;
    if (_audioSampleRate != (uint32_t)info.sampRateOut) {
      _audioSampleRate = info.sampRateOut;
      if (_playbackStarted && !_speaker.setSampleRate(_audioSampleRate)) return false;
    }
    return writeMono(_audioScratch, info.outputSamps / info.nChans, info.nChans);
  }

  bool decodeMp3(const uint8_t *bytes, uint32_t length) {
    if (!_mp3 || !bytes || !length) return false;
    if (!_audioScratch) _audioScratch = static_cast<int16_t *>(ps_malloc(MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(int16_t)));
    if (!_audioScratch) return false;
    mp3dec_frame_info_t info{};
    const int frames = mp3dec_decode_frame(static_cast<mp3dec_t *>(_mp3), bytes, (int)length, _audioScratch, &info);
    if (frames <= 0 || info.channels < 1 || info.channels > 2 || !info.hz) return false;
    if (_audioSampleRate != (uint32_t)info.hz) {
      _audioSampleRate = info.hz;
      if (_playbackStarted && !_speaker.setSampleRate(_audioSampleRate)) return false;
    }
    return writeMono(_audioScratch, frames, info.channels);
  }

  bool decodeAmr(const uint8_t *bytes, uint32_t length) {
    static const uint8_t FrameBytes[] = {13, 14, 16, 18, 20, 21, 27, 32, 6};
    if (!_amr || !bytes || !length) return false;
    if (!_audioScratch) _audioScratch = static_cast<int16_t *>(ps_malloc(160 * sizeof(int16_t)));
    if (!_audioScratch) return false;
    uint32_t offset = 0;
    while (offset < length) {
      const uint8_t type = (bytes[offset] >> 3) & 0x0f;
      if (type <= 8) {
        const uint8_t frameBytes = FrameBytes[type];
        if (frameBytes > length - offset) {
          Serial.printf("[Video] AMR truncation sample=%lu type=%u bytes=%lu offset=%lu\n",
                        static_cast<unsigned long>(_audioSample), type, static_cast<unsigned long>(length),
                        static_cast<unsigned long>(offset));
          return false;
        }
        Decoder_Interface_Decode(_amr, bytes + offset, _audioScratch, 0);
        if (!writeMono(_audioScratch, 160, 1)) return false;
        offset += frameBytes;
        continue;
      }
      // RFC 3267 no-data and reserved modes carry no usable speech for the
      // narrowband decoder. Preserve the audio timeline with one silence
      // frame rather than disabling the complete track.
      if (type >= 9 && type <= 15) {
        memset(_audioScratch, 0, 160 * sizeof(*_audioScratch));
        if (!writeMono(_audioScratch, 160, 1)) return false;
        ++offset;
        continue;
      }
      return false;
    }
    return true;
  }

  bool readSample(uint32_t track, uint32_t sample, uint32_t &bytes, uint32_t &timestamp, uint32_t &duration) {
    bytes = timestamp = duration = 0;
    if (!_demux || track >= _demux->track_count) return false;
    if (sample >= _demux->track[track].sample_count) return false;
    unsigned sampleBytes = 0, sampleTimestamp = 0, sampleDuration = 0;
    const MP4D_file_offset_t offset = MP4D_frame_offset(_demux, track, sample, &sampleBytes,
                                                         &sampleTimestamp, &sampleDuration);
    if (!sampleBytes || offset > 0xffffffffULL || !ensureSampleBuffer(sampleBytes)) return false;
    const bool ok = readSource((uint32_t)offset, _sampleBuffer, sampleBytes);
    bytes = sampleBytes;
    timestamp = sampleTimestamp;
    duration = sampleDuration;
    return ok;
  }

  bool ensureSampleBuffer(uint32_t bytes) {
    if (bytes <= _sampleBufferBytes) return true;
    uint8_t *replacement = static_cast<uint8_t *>(ps_malloc(bytes));
    if (!replacement) return false;
    free(_sampleBuffer);
    _sampleBuffer = replacement;
    _sampleBufferBytes = bytes;
    return true;
  }

  bool ensureAnnexBuffer(uint32_t bytes) {
    if (bytes <= _annexBufferBytes) return true;
    uint8_t *replacement = static_cast<uint8_t *>(ps_malloc(bytes));
    if (!replacement) return false;
    free(_annexBuffer);
    _annexBuffer = replacement;
    _annexBufferBytes = bytes;
    return true;
  }

  bool readAudioSample(uint32_t &bytes, uint32_t &timestamp, uint32_t &duration) {
    bytes = timestamp = duration = 0;
    if (!_demux || _audioTrack >= _demux->track_count || _audioSample >= _demux->track[_audioTrack].sample_count) return false;
    unsigned sampleBytes = 0, sampleTimestamp = 0, sampleDuration = 0;
    const MP4D_file_offset_t offset = MP4D_frame_offset(_demux, _audioTrack, _audioSample, &sampleBytes,
                                                         &sampleTimestamp, &sampleDuration);
    if (!sampleBytes || offset > 0xffffffffULL || !ensureAudioBuffer(sampleBytes)) return false;
    const bool ok = readSource((uint32_t)offset, _audioBuffer, sampleBytes);
    bytes = sampleBytes;
    timestamp = sampleTimestamp;
    duration = sampleDuration;
    return ok;
  }

  bool ensureAudioBuffer(uint32_t bytes) {
    if (bytes <= _audioBufferBytes) return true;
    uint8_t *replacement = static_cast<uint8_t *>(ps_malloc(bytes));
    if (!replacement) return false;
    free(_audioBuffer);
    _audioBuffer = replacement;
    _audioBufferBytes = bytes;
    return true;
  }

  bool readSource(uint32_t offset, uint8_t *destination, uint32_t bytes) {
    if (!destination || !bytes || offset > _sourceSize || bytes > _sourceSize - offset) return false;
    if (_readWindow && offset >= _readWindowStart &&
        offset + bytes <= _readWindowStart + _readWindowBytes) {
      memcpy(destination, _readWindow + (offset - _readWindowStart), bytes);
      return true;
    }
    if (bytes <= IoChunkBytes) {
      if (!_readWindow) _readWindow = static_cast<uint8_t *>(ps_malloc(IoChunkBytes));
      if (!_readWindow) return false;
      const uint32_t refillBytes = min<uint32_t>(IoChunkBytes, _sourceSize - offset);
      if (!SpiBusLock::lock(pdMS_TO_TICKS(20))) return false;
      const bool ok = _source.seek(offset) && _source.read(_readWindow, refillBytes) == (int)refillBytes;
      SpiBusLock::unlock();
      if (!ok) { _readWindowBytes = 0; return false; }
      _readWindowStart = offset;
      _readWindowBytes = refillBytes;
      ++_sdReadCount;
      _sdReadBytes += refillBytes;
      memcpy(destination, _readWindow, bytes);
      return true;
    }
    _readWindowBytes = 0;
    uint32_t copied = 0;
    while (copied < bytes) {
      const uint32_t chunk = min<uint32_t>(IoChunkBytes, bytes - copied);
      if (!SpiBusLock::lock(pdMS_TO_TICKS(20))) return false;
      const bool ok = _source.seek(offset + copied) &&
                      _source.read(destination + copied, chunk) == (int)chunk;
      SpiBusLock::unlock();
      if (!ok) return false;
      ++_sdReadCount;
      _sdReadBytes += chunk;
      copied += chunk;
      taskYIELD();
    }
    return true;
  }

  bool buildAnnexB(const uint8_t *sample, uint32_t bytes, bool includeParameters, uint32_t &outputBytes) {
    outputBytes = 0;
    if (!_demux || !sample || _videoTrack >= _demux->track_count) return false;
    const MP4D_track_t &track = _demux->track[_videoTrack];
    const uint8_t lengthBytes = track.avc_nal_length_size;
    uint32_t required = 0;
    if (includeParameters) {
      for (unsigned index = 0; ; ++index) {
        int size = 0; const uint8_t *sps = static_cast<const uint8_t *>(MP4D_read_sps(_demux, _videoTrack, index, &size));
        if (!sps || size <= 0) break; required += 4 + size;
      }
      for (unsigned index = 0; ; ++index) {
        int size = 0; const uint8_t *pps = static_cast<const uint8_t *>(MP4D_read_pps(_demux, _videoTrack, index, &size));
        if (!pps || size <= 0) break; required += 4 + size;
      }
    }
    for (uint32_t offset = 0; offset + lengthBytes <= bytes; ) {
      uint32_t size = 0;
      for (uint8_t index = 0; index < lengthBytes; ++index) size = (size << 8) | sample[offset + index];
      offset += lengthBytes;
      if (size > bytes - offset) return false;
      required += 4 + size;
      offset += size;
    }
    if (!ensureAnnexBuffer(required)) return false;
    uint32_t output = 0;
    auto append = [&](const uint8_t *data, uint32_t size) {
      _annexBuffer[output++] = 0; _annexBuffer[output++] = 0; _annexBuffer[output++] = 0; _annexBuffer[output++] = 1;
      memcpy(_annexBuffer + output, data, size); output += size;
    };
    if (includeParameters) {
      for (unsigned index = 0; ; ++index) { int size = 0; const uint8_t *sps = static_cast<const uint8_t *>(MP4D_read_sps(_demux, _videoTrack, index, &size)); if (!sps || size <= 0) break; append(sps, size); }
      for (unsigned index = 0; ; ++index) { int size = 0; const uint8_t *pps = static_cast<const uint8_t *>(MP4D_read_pps(_demux, _videoTrack, index, &size)); if (!pps || size <= 0) break; append(pps, size); }
    }
    for (uint32_t offset = 0; offset + lengthBytes <= bytes; ) {
      uint32_t size = 0; for (uint8_t index = 0; index < lengthBytes; ++index) size = (size << 8) | sample[offset + index];
      offset += lengthBytes; append(sample + offset, size); offset += size;
    }
    outputBytes = output;
    return true;
  }

  uint8_t readySlots() const {
    uint8_t ready = 0;
    StateLock lock(videoStreamStateMutex());
    if (!lock.locked()) return 0;
    for (uint8_t index = 0; index < FrameSlots; ++index) if (_slots[index].state == SlotState::Ready) ++ready;
    return ready;
  }

  uint8_t freeSlots() const {
    uint8_t freeSlots = 0;
    StateLock lock(videoStreamStateMutex());
    if (!lock.locked()) return 0;
    for (uint8_t index = 0; index < FrameSlots; ++index) if (_slots[index].state == SlotState::Free) ++freeSlots;
    return freeSlots;
  }

  uint32_t ringFillMs() const {
    return _audioSampleRate ? (uint32_t)(((uint64_t)_pcmCount * 1000UL) / _audioSampleRate) : 0;
  }

  uint32_t submittedAudioMs() const {
    return _audioSampleRate ? (uint32_t)(((uint64_t)_audioFramesSubmitted * 1000UL) / _audioSampleRate) : 0;
  }

  uint32_t playbackElapsedMs(bool publish = true) {
    const uint32_t wallElapsed = _phase == Phase::Paused ? _pausedElapsedMs : millis() - _startedAt;
    uint32_t elapsed = wallElapsed;
    if (_audioCodec != AudioCodec::None && _audioSampleRate) {
      const uint32_t submittedMs = submittedAudioMs();
      const bool audioComplete = _audioSample >= _audioSamples && _pcmCount == 0 && wallElapsed >= submittedMs;
      if (!audioComplete) elapsed = min<uint32_t>(elapsed, submittedMs);
    }
    if (publish) {
      StateLock lock(videoStreamStateMutex());
      if (lock.locked()) _snapshot.elapsedMs = elapsed;
    }
    return elapsed;
  }

  void disableAudio(const String &status) {
    _speaker.stop();
    _audioCodec = AudioCodec::None;
    _pcmRead = _pcmWrite = _pcmCount = 0;
    StateLock lock(videoStreamStateMutex());
    if (lock.locked()) {
      _snapshot.hasAudio = false;
      _snapshot.status = status;
    }
  }

  void updateUnderrunStatus() {
    if (_audioCodec == AudioCodec::None || _audioSample >= _audioSamples) return;
    const bool underrun = _pcmCount == 0 && submittedAudioMs() <= millis() - _startedAt;
    if (underrun == _audioUnderrun) return;
    _audioUnderrun = underrun;
    publishSnapshot(true, underrun ? F("Buffering") : F("Playing"));
  }

  void logPerformance(uint32_t elapsed) {
    const uint32_t now = millis();
    if (now - _lastProgressLogMs < 1000) return;
    const uint32_t interval = _lastProgressLogMs ? now - _lastProgressLogMs : 1000;
    _lastProgressLogMs = now;
    const uint32_t decodeFps10 = interval ? (_decodedSinceLog * 10000UL) / interval : 0;
    const uint32_t presentedSinceLog = __atomic_exchange_n(&_presentedSinceLog, 0, __ATOMIC_RELAXED);
    const uint32_t presentFps10 = interval ? (presentedSinceLog * 10000UL) / interval : 0;
    const uint32_t droppedSinceLog = __atomic_exchange_n(&_droppedSinceLog, 0, __ATOMIC_RELAXED);
    const uint32_t dropFps10 = interval ? (droppedSinceLog * 10000UL) / interval : 0;
    const uint32_t averageDecodeUs = _videoDecodeCount ? (uint32_t)(_videoDecodeUs / _videoDecodeCount) : 0;
    const uint32_t averageScaleUs = _scaleCount ? (uint32_t)(_scaleUs / _scaleCount) : 0;
    Serial.printf("[Video] progress=%lu/%lu video=%lu/%lu audio=%lu/%lu fps(d/p/x)=%lu.%lu/%lu.%lu/%lu.%lu pcm=%lums submitted=%lu i2s0=%lu sd=%lu/%lu avgUs(decode/scale)=%lu/%lu drops=%lu\n",
                  static_cast<unsigned long>(elapsed), static_cast<unsigned long>(_durationMs),
                  static_cast<unsigned long>(_videoSample), static_cast<unsigned long>(_videoSamples),
                  static_cast<unsigned long>(_audioSample), static_cast<unsigned long>(_audioSamples),
                  static_cast<unsigned long>(decodeFps10 / 10), static_cast<unsigned long>(decodeFps10 % 10),
                  static_cast<unsigned long>(presentFps10 / 10), static_cast<unsigned long>(presentFps10 % 10),
                  static_cast<unsigned long>(dropFps10 / 10), static_cast<unsigned long>(dropFps10 % 10),
                  static_cast<unsigned long>(ringFillMs()), static_cast<unsigned long>(_audioFramesSubmitted),
                  static_cast<unsigned long>(_i2sZeroWrites), static_cast<unsigned long>(_sdReadCount),
                  static_cast<unsigned long>(_sdReadBytes), static_cast<unsigned long>(averageDecodeUs),
                  static_cast<unsigned long>(averageScaleUs),
                  static_cast<unsigned long>(__atomic_load_n(&_droppedFrames, __ATOMIC_RELAXED)));
    _decodedSinceLog = 0;
    _videoDecodeUs = _scaleUs = 0;
    _videoDecodeCount = _scaleCount = 0;
    updateUnderrunStatus();
  }

  static uint32_t timestampMs(uint32_t timestamp, uint32_t timeScale) {
    return timeScale ? (uint32_t)(((uint64_t)timestamp * 1000UL) / timeScale) : 0;
  }

  static uint32_t presentationTimestampMs(int64_t timestamp, uint32_t timeScale) {
    if (timestamp < 0 || !timeScale) return 0;
    const uint64_t milliseconds = ((uint64_t)timestamp * 1000UL) / timeScale;
    return milliseconds > 0xffffffffULL ? 0xffffffffUL : (uint32_t)milliseconds;
  }

  void releaseAudioDecoder() {
    if (_aac) AACFreeDecoder((HAACDecoder)_aac);
    if (_mp3) free(_mp3);
    if (_amr) Decoder_Interface_exit(_amr);
    _aac = _mp3 = _amr = nullptr;
  }

  void cleanup() {
    _speaker.stop();
    _h264.end();
    _legacy.end();
    releaseAudioDecoder();
    if (_source) _source.close();
    if (_demux) { MP4D_close(_demux); delete _demux; _demux = nullptr; }
    free(_presentation); _presentation = nullptr;
    free(_sampleBuffer); _sampleBuffer = nullptr; _sampleBufferBytes = 0;
    free(_annexBuffer); _annexBuffer = nullptr; _annexBufferBytes = 0;
    free(_audioBuffer); _audioBuffer = nullptr; _audioBufferBytes = 0;
    free(_readWindow); _readWindow = nullptr; _readWindowStart = _readWindowBytes = 0;
    free(_audioScratch); _audioScratch = nullptr;
    free(_pcmRing); _pcmRing = nullptr; _pcmRead = _pcmWrite = _pcmCount = 0;
    releaseFrameSlots();
    _videoCodec = VideoCodec::None;
    _audioCodec = AudioCodec::None;
    _videoSamples = _audioSamples = _videoSample = _audioSample = 0;
    _presentationCount = _presentationIndex = 0;
    _videoFlushed = _videoFinished = _firstVideoTimestampSet = false;
    _audioFramesSubmitted = _startedAt = _pausedElapsedMs = _audioSampleRate = _audioChannels = 0;
    _playbackStarted = false;
    _sourceWidth = _sourceHeight = _drawX = _drawY = _drawWidth = _drawHeight = 0;
    _durationMs = _droppedFrames = _presentationClockMs = _lastSnapshotMs = _lastProgressLogMs =
      _lastPresentedAtMs = 0;
    _decodedSinceLog = _presentedSinceLog = _droppedSinceLog = _i2sZeroWrites = 0;
    _sdReadCount = _sdReadBytes = 0;
    _videoDecodeUs = _scaleUs = 0;
    _videoDecodeCount = _scaleCount = 0;
    _audioUnderrun = false;
    _pauseAfterViewportReconfigure = _pauseAfterViewportFrame = false;
  }
};

VideoPlayerScreen::VideoPlayerScreen(Navigator &navigator, TouchController &touch, DisplayController &display,
                                     SdCardController &sdCard, SpeakerController &speaker,
                                     SecretStorage &secretStorage)
    : Screen(navigator, touch, display), _sdCard(sdCard), _speaker(speaker), _secretStorage(secretStorage),
      _stream(nullptr), _state(State::Opening), _viewMode(ViewMode::FullScreen),
      _pendingViewMode(ViewMode::FullScreen), _secretAccess(false), _sdMounted(false), _lastTouchDown(false),
      _closeRequested(false), _autoPlay(false), _viewModeChangePending(false),
      _playButtonUnderlayValid(false), _durationMs(0), _elapsedMs(0), _lastDrawMs(0),
      _autoFullScreenAtMs(0), _viewportWidth(0), _viewportHeight(0), _sourceWidth(0),
      _sourceHeight(0), _playButtonUnderlayX(0), _playButtonUnderlayY(0), _playButtonUnderlay{} {}

VideoPlayerScreen::~VideoPlayerScreen() {
  delete _stream;
}

void VideoPlayerScreen::onEnter(const ScreenData &data) {
  resetView();
  _secretAccess = data.fileAccess == FileAccessMode::Secret;
  _sdMounted = _sdCard.isMounted();
  _path = data.kind == ScreenDataKind::VideoPath ? data.text : "";
  _viewportWidth = _display.width();
  _viewportHeight = _display.height();
  Serial.printf("[Video] screen enter path=%s viewport=%ux%u\n", _path.c_str(), _viewportWidth, _viewportHeight);
  draw();
  if (!_path.length()) {
    _state = State::Error;
    _error = F("No video selected");
    useEmbeddedErrorMode();
    draw();
    return;
  }
  if (!_sdMounted) {
    _state = State::Error;
    _error = F("SD card unavailable");
    useEmbeddedErrorMode();
    draw();
    return;
  }
  delete _stream;
  _stream = new VideoStreamWorker(_speaker, _secretStorage, _viewportWidth, _viewportHeight);
  if (!_stream || !_stream->prepare(_path, _secretAccess)) {
    _state = State::Error;
    _error = F("Video worker unavailable");
    useEmbeddedErrorMode();
  }
  draw();
}

void VideoPlayerScreen::onExit() {
  if (_stream && !_closeRequested) _stream->pause();
}

bool VideoPlayerScreen::releaseResources() {
  if (!_stream) return true;
  if (!_stream->end()) return false;
  delete _stream;
  _stream = nullptr;
  return true;
}

void VideoPlayerScreen::onResume() {
  _lastTouchDown = false;
  if (_stream) _stream->play();
  draw();
}

void VideoPlayerScreen::handleTouch(const TouchPoint &point) {
  if (!point.touched) { _lastTouchDown = false; return; }
  if (_lastTouchDown) return;
  _lastTouchDown = true;
  if (_viewMode == ViewMode::FullScreen) {
    requestViewMode(ViewMode::Embedded, _state == State::Playing);
    return;
  }
  if (UiKit::navigationIconContains(point.x, point.y)) { requestClose(); return; }
  if (playButtonContains(point.x, point.y) && _stream && _state != State::Closing && _state != State::Error) {
    if (_state == State::Playing) {
      _autoFullScreenAtMs = 0;
      Serial.println(F("[Video] UI pause"));
      _stream->pause();
    }
    else if (_state == State::Paused || _state == State::Buffering) {
      Serial.println(F("[Video] UI play"));
      _stream->configureViewport(_viewportWidth, _viewportHeight);
      _stream->play();
      scheduleAutoFullScreen();
    }
    else if (_state == State::Finished) {
      Serial.println(F("[Video] UI replay"));
      _autoPlay = true;
      _stream->replay();
      scheduleAutoFullScreen();
    }
    return;
  }
  if (videoCanvasContains(point.x, point.y)) {
    _autoFullScreenAtMs = 0;
    requestViewMode(ViewMode::FullScreen);
  }
}

void VideoPlayerScreen::requestViewMode(ViewMode mode, bool pauseAfterFrame) {
  const ViewMode target = _viewModeChangePending ? _pendingViewMode : _viewMode;
  if (target == mode) return;
  if (mode == ViewMode::FullScreen) _autoFullScreenAtMs = 0;
  // A paused/finished worker cannot decode a replacement frame. Preserve the
  // current image and only change chrome; playback will configure the current
  // mode before it resumes.
  if (_state == State::Paused || _state == State::Finished) {
    applyViewMode(mode, true);
    return;
  }
  _pendingViewMode = mode;
  _viewModeChangePending = true;
  const uint16_t width = _display.width();
  const uint16_t height = mode == ViewMode::FullScreen
                          ? _display.height()
                          : (_display.height() > EmbeddedViewportTop + EmbeddedProgressReserve
                             ? _display.height() - EmbeddedViewportTop - EmbeddedProgressReserve : 0);
  Serial.printf("[Video] view pending=%s viewport=%ux%u\n",
                mode == ViewMode::FullScreen ? "full" : "embedded", width, height);
  if (_stream) _stream->configureViewport(width, height, pauseAfterFrame);
}

void VideoPlayerScreen::applyViewMode(ViewMode mode, bool clearOutgoingChrome) {
  if (_viewMode == mode) return;
  if (clearOutgoingChrome && _viewMode == ViewMode::Embedded && mode == ViewMode::FullScreen) {
    restorePlayButtonUnderlay();
    clearEmbeddedChrome();
  }
  _viewMode = mode;
  _pendingViewMode = mode;
  _viewModeChangePending = false;
  updateViewportForViewMode();
  if (mode == ViewMode::Embedded) {
    drawTopBar();
    drawProgress();
    drawPlayButton();
  }
}

void VideoPlayerScreen::updateViewportForViewMode() {
  _viewportWidth = _display.width();
  _viewportHeight = _viewMode == ViewMode::FullScreen
                      ? _display.height()
                      : (_display.height() > EmbeddedViewportTop + EmbeddedProgressReserve
                         ? _display.height() - EmbeddedViewportTop - EmbeddedProgressReserve : 0);
}

void VideoPlayerScreen::useEmbeddedErrorMode() {
  _autoFullScreenAtMs = 0;
  _viewModeChangePending = false;
  _pendingViewMode = ViewMode::Embedded;
  if (_viewMode != ViewMode::Embedded) applyViewMode(ViewMode::Embedded, false);
}

void VideoPlayerScreen::clearEmbeddedChrome() {
  if (!_display.isReady()) return;
  TFT_eSPI &tft = _display.tft();
  tft.fillRect(0, 0, _display.width(), UiKit::AppBarHeight, TFT_BLACK);
  tft.fillRect(0, _display.height() - EmbeddedProgressReserve, _display.width(),
               EmbeddedProgressReserve, TFT_BLACK);
}

void VideoPlayerScreen::scheduleAutoFullScreen() {
  _autoFullScreenAtMs = millis() + AutoFullScreenDelayMs;
}

void VideoPlayerScreen::tick(uint32_t nowMs) {
  handleSdCardEvent(_sdCard.takeEvent());
  updateStream(nowMs);
  if (_autoFullScreenAtMs && _state == State::Playing &&
      (int32_t)(nowMs - _autoFullScreenAtMs) >= 0) {
    _autoFullScreenAtMs = 0;
    requestViewMode(ViewMode::FullScreen);
  }
  if (_closeRequested && (!_stream || _stream->stopped())) _navigator.pop();
}

void VideoPlayerScreen::resetView() {
  _path = _error = "";
  _state = State::Opening;
  _viewMode = ViewMode::FullScreen;
  _pendingViewMode = ViewMode::FullScreen;
  _lastTouchDown = _closeRequested = false;
  _autoPlay = true;
  _viewModeChangePending = false;
  _durationMs = _elapsedMs = _lastDrawMs = 0;
  _autoFullScreenAtMs = 0;
  _sourceWidth = _sourceHeight = 0;
  _playButtonUnderlayValid = false;
}

void VideoPlayerScreen::requestClose() {
  if (_closeRequested) return;
  _closeRequested = true;
  _state = State::Closing;
  _autoFullScreenAtMs = 0;
  Serial.println(F("[Video] UI close"));
  if (_stream) _stream->requestStop();
  draw();
}

void VideoPlayerScreen::updateStream(uint32_t nowMs) {
  if (!_stream) return;
  const VideoStreamWorker::Snapshot snapshot = _stream->snapshot();
  _durationMs = snapshot.durationMs;
  _elapsedMs = snapshot.elapsedMs;
  _sourceWidth = snapshot.sourceWidth;
  _sourceHeight = snapshot.sourceHeight;
  switch (snapshot.phase) {
    case VideoStreamWorker::Phase::Opening: _state = State::Opening; break;
    case VideoStreamWorker::Phase::Buffered:
      _state = State::Buffering;
      if (_autoPlay && !_closeRequested) { _autoPlay = false; _stream->play(); }
      break;
    case VideoStreamWorker::Phase::Playing: _state = State::Playing; presentFrames(); break;
    case VideoStreamWorker::Phase::Paused: _state = State::Paused; presentFrames(); break;
    case VideoStreamWorker::Phase::Finished: _state = State::Finished; presentFrames(); break;
    case VideoStreamWorker::Phase::Error:
      _state = State::Error;
      _error = snapshot.error;
      useEmbeddedErrorMode();
      break;
    case VideoStreamWorker::Phase::Stopping: _state = State::Closing; break;
    case VideoStreamWorker::Phase::Idle: break;
  }
  if (nowMs - _lastDrawMs >= 150 || _state == State::Error) {
    _lastDrawMs = nowMs;
    draw(_state == State::Error);
  }
}

void VideoPlayerScreen::presentFrames() {
  if (!_stream || (_state != State::Playing && _state != State::Paused && _state != State::Finished)) return;
  VideoStreamWorker::Frame newest;
  bool haveFrame = false;
  VideoStreamWorker::Frame frame;
  // Free stale ready slots first, but always display the newest one. Dropping
  // every late frame makes a slow clip freeze permanently as its timestamps
  // move farther from the audio clock.
  while (_stream->takeFrame(frame)) {
    if (haveFrame) _stream->releaseFrame(newest.slot, true);
    newest = frame;
    haveFrame = true;
  }
  if (!haveFrame) return;
  if (_viewModeChangePending) applyViewMode(_pendingViewMode, true);
  if (newest.pixels && newest.width && newest.height) {
    _display.tft().pushImage(newest.x, viewportTop() + newest.y, newest.width, newest.height,
                             const_cast<uint16_t *>(newest.pixels));
    capturePlayButtonUnderlay(newest.pixels, newest.x, viewportTop() + newest.y,
                              newest.width, newest.height);
  }
  _stream->releaseFrame(newest.slot, false);
  if (_viewMode == ViewMode::Embedded) {
    drawTopBar();
    drawProgress();
    drawPlayButton();
  }
}

void VideoPlayerScreen::handleSdCardEvent(SdCardEvent event) {
  if (event == SdCardEvent::None) return;
  if (event == SdCardEvent::Mounted) { _sdMounted = true; return; }
  _sdMounted = false;
  Serial.printf("[Video] SD event=%u\n", static_cast<unsigned>(event));
  if (_stream) _stream->requestStop();
  _state = State::Error;
  _error = event == SdCardEvent::Removed ? F("SD card removed") : F("SD card unavailable");
  useEmbeddedErrorMode();
  draw();
}

void VideoPlayerScreen::draw(bool full) {
  if (!_display.isReady()) return;
  TFT_eSPI &tft = _display.tft();
  if (full) {
    tft.fillScreen(TFT_BLACK);
    if (_viewMode == ViewMode::Embedded) drawTopBar();
  }
  if (_viewMode == ViewMode::Embedded) {
    drawPlayButton();
    drawProgress();
  }
  if (_state == State::Error) drawError();
}

void VideoPlayerScreen::drawTopBar() {
  const int slash = _path.lastIndexOf('/');
  const String name = slash < 0 ? _path : _path.substring(slash + 1);
  UiKit::drawTopAppBar(_display.tft(), _display.width(), name);
}

void VideoPlayerScreen::capturePlayButtonUnderlay(const uint16_t *pixels, int16_t frameX, int16_t frameY,
                                                   uint16_t frameWidth, uint16_t frameHeight) {
  if (!pixels) { _playButtonUnderlayValid = false; return; }
  int16_t activeX = 0, activeY = 0;
  uint16_t activeWidth = 0, activeHeight = 0;
  videoFrameRect(activeX, activeY, activeWidth, activeHeight);
  const int16_t left = activeX + activeWidth / 2 - PlaybackControlUnderlaySize / 2;
  const int16_t top = activeY + activeHeight / 2 - PlaybackControlUnderlaySize / 2;
  const int16_t sourceX = left - frameX;
  const int16_t sourceY = top - frameY;
  if (sourceX < 0 || sourceY < 0 || sourceX + PlaybackControlUnderlaySize > frameWidth ||
      sourceY + PlaybackControlUnderlaySize > frameHeight) {
    _playButtonUnderlayValid = false;
    return;
  }
  for (uint16_t row = 0; row < PlaybackControlUnderlaySize; ++row) {
    memcpy(_playButtonUnderlay + row * PlaybackControlUnderlaySize,
           pixels + (sourceY + row) * frameWidth + sourceX,
           PlaybackControlUnderlaySize * sizeof(uint16_t));
  }
  _playButtonUnderlayX = left;
  _playButtonUnderlayY = top;
  _playButtonUnderlayValid = true;
}

void VideoPlayerScreen::restorePlayButtonUnderlay() {
  if (!_playButtonUnderlayValid || !_display.isReady()) return;
  _display.tft().pushImage(_playButtonUnderlayX, _playButtonUnderlayY,
                           PlaybackControlUnderlaySize, PlaybackControlUnderlaySize,
                           _playButtonUnderlay);
  _playButtonUnderlayValid = false;
}

uint16_t VideoPlayerScreen::viewportTop() const {
  return _display.height() > _viewportHeight ? (_display.height() - _viewportHeight) / 2 : 0;
}

void VideoPlayerScreen::videoFrameRect(int16_t &x, int16_t &y, uint16_t &width, uint16_t &height) const {
  width = _viewportWidth;
  height = _viewportHeight;
  x = 0;
  y = viewportTop();
  if (!_sourceWidth || !_sourceHeight || !_viewportWidth || !_viewportHeight) return;
  if ((uint32_t)_sourceWidth * _viewportHeight > (uint32_t)_sourceHeight * _viewportWidth) {
    height = max<uint32_t>(1, ((uint32_t)_sourceHeight * _viewportWidth) / _sourceWidth);
    y += (_viewportHeight - height) / 2;
  } else {
    width = max<uint32_t>(1, ((uint32_t)_sourceWidth * _viewportHeight) / _sourceHeight);
    x = (_viewportWidth - width) / 2;
  }
}

void VideoPlayerScreen::drawPlayButton() {
  if (_viewMode != ViewMode::Embedded) return;
  TFT_eSPI &tft = _display.tft();
  int16_t frameX = 0, frameY = 0;
  uint16_t frameWidth = 0, frameHeight = 0;
  videoFrameRect(frameX, frameY, frameWidth, frameHeight);
  const int16_t cx = frameX + frameWidth / 2;
  const int16_t cy = frameY + frameHeight / 2;
  const bool enabled = _state != State::Opening && _state != State::Closing && _state != State::Error && _sdMounted;
  const uint16_t background = TFT_DARKGREY;
  tft.fillSmoothCircle(cx, cy, PlaybackControlSize / 2, background, TFT_BLACK);
  UiKit::drawPlaybackIcon(tft, cx, cy, _state == State::Playing, enabled ? TFT_WHITE : TFT_LIGHTGREY, background);
}

void VideoPlayerScreen::drawProgress() {
  const int16_t x = 16, width = _display.width() - 32, y = _display.height() - 18;
  const uint8_t percent = _durationMs ? (uint8_t)min<uint32_t>(100, ((uint64_t)_elapsedMs * 100) / _durationMs) : 0;
  UiKit::drawProgressBar(_display.tft(), x, y, width, 10, percent, false, TFT_CYAN, TFT_DARKGREY, TFT_BLACK, true);
}

void VideoPlayerScreen::drawError() {
  TFT_eSPI &tft = _display.tft();
  constexpr uint8_t MaxErrorLines = 6;
  String lines[MaxErrorLines];
  uint8_t count = 0;
  int start = 0;
  while (count < MaxErrorLines && start <= _error.length()) {
    const int newline = _error.indexOf('\n', start);
    lines[count++] = newline < 0 ? _error.substring(start) : _error.substring(start, newline);
    if (newline < 0) break;
    start = newline + 1;
  }
  if (!count) return;
  const int16_t centerY = viewportTop() + _viewportHeight / 2;
  const int16_t firstY = centerY - ((int16_t)count - 1) * 10;
  for (uint8_t index = 0; index < count; ++index) {
    UiText::draw(tft, UiKit::ellipsize(lines[index], _display.width() - 32, UiText::Size::Compact),
                 _display.width() / 2, firstY + index * 20,
                 index == 0 ? TFT_RED : TFT_LIGHTGREY, TFT_BLACK, UiText::Size::Compact, MC_DATUM);
  }
}

bool VideoPlayerScreen::playButtonContains(int16_t x, int16_t y) const {
  if (_viewMode != ViewMode::Embedded) return false;
  int16_t frameX = 0, frameY = 0;
  uint16_t frameWidth = 0, frameHeight = 0;
  videoFrameRect(frameX, frameY, frameWidth, frameHeight);
  const int16_t cx = frameX + frameWidth / 2;
  const int16_t cy = frameY + frameHeight / 2;
  const int16_t half = PlaybackControlSize / 2;
  return x >= cx - half && x < cx + half && y >= cy - half && y < cy + half;
}

bool VideoPlayerScreen::videoCanvasContains(int16_t x, int16_t y) const {
  return x >= 0 && x < _display.width() && y >= viewportTop() && y < viewportTop() + _viewportHeight;
}
