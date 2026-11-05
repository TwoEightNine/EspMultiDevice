#pragma once

#include <Arduino.h>

#include "../../../third_party/minimp3/minimp3.h"

#include "../../media/AudioTags.h"
#include "../../media/MediaCache.h"
#include "../../services/storage/SecretStorage.h"

class SpeakerController;

// One open WAV or MP3 file for the Audio Player: header parsing, tag reading,
// the MP3 frame scan and its `.empa` cache, positioning, and mono PCM
// streaming to the speaker. It never draws or navigates. Every call does a
// bounded amount of work on the caller's (UI) task.
//
// Every SD transaction holds SpiBusLock, because the cover worker reads the
// same card from the other core. loadStep() and stream() wait only briefly
// for the bus; on a miss they do nothing and the caller retries next tick
// (stream() reports Busy), which the speaker's ~200 ms DMA queue absorbs.
//
// Errors close the file and leave a message in error(); the tags, format, and
// duration read so far stay available so the screen can keep showing them.
class AudioSource {
public:
  enum class Format : uint8_t {
    None,
    Wav,
    Mp3
  };

  enum class StreamResult : uint8_t {
    Streaming,
    Busy,
    Ended,
    Failed
  };

  AudioSource(SecretStorage &secretStorage, MediaCache &mediaCache);

  // Closes the file and forgets everything about it.
  void reset();
  // Closes the file but keeps the tags, format, and position for display.
  void closeFile();
  // Opens and validates the header. Tag parsing and any MP3 scan then run
  // through loadStep(). Returns false with error() set.
  bool open(const String &path, bool secretAccess);
  // Advances tag parsing, then the MP3 scan, by one bounded step.
  void loadStep();

  bool isOpen() const { return (bool)_file; }
  bool loading() const { return _tagsParsing || _mp3Analyzing; }
  bool tagsParsing() const { return _tagsParsing; }
  bool failed() const { return _error.length() > 0; }
  const String &error() const { return _error; }
  const AudioTags::Info &tags() const { return _tags.info(); }

  Format format() const { return _format; }
  uint32_t sampleRate() const { return _sampleRate; }
  uint16_t channels() const { return _channels; }
  uint16_t bitsPerSample() const { return _bitsPerSample; }
  uint32_t frameCount() const { return _frameCount; }
  uint32_t framePosition() const { return _framePosition; }
  bool ended() const { return _frameCount > 0 && _framePosition >= _frameCount; }
  // Average for MP3 (compressed bytes over duration), nominal for WAV; 0 if unknown.
  uint32_t bitrateKbps() const;

  // True when seek() accepts any frame. MP3 can only rewind to the start
  // until it has a seek table.
  bool canSeek() const;
  bool seek(uint32_t frame);

  // Decodes and writes up to one output chunk. Ended means there was nothing
  // left to write; the caller lets the speaker's queue drain.
  StreamResult stream(SpeakerController &speaker);

private:
  static const uint16_t SourceBufferBytes = 4096;
  static const uint16_t Mp3InputBufferBytes = 4096;
  static const uint16_t Mp3MaxPcmFrames = MINIMP3_MAX_SAMPLES_PER_FRAME;
  static const uint8_t Mp3AnalysisFramesPerStep = 16;
  static const uint16_t Mp3CacheHeaderBytes = 36;
  static const uint16_t OutputFrames = 256;
  static const uint8_t TagFramesPerStep = 8;
  // Per-tick work waits this long for the bus before skipping the tick.
  static const uint32_t BusWaitMs = 10;
  // One-off opens and closes wait longer; they cannot be retried silently.
  static const uint32_t BusOpenWaitMs = 250;

  SecretStorage &_secretStorage;
  MediaCache &_mediaCache;
  // StorageFile's validity operators are non-const.
  mutable StorageFile _file;
  String _path;
  bool _secretAccess;
  String _error;
  Format _format;
  // False after anything moved the file position away from the stream:
  // opening, tag reads, the MP3 scan, or a seek.
  bool _streamReady;
  // True while this object holds SpiBusLock, so nested helpers (fail() ->
  // closeFile()) do not try to take the non-recursive mutex again.
  bool _busLocked;
  // The scan finished under the bus lock; MediaCache takes the lock itself,
  // so the `.empa` write runs after it is released.
  bool _mp3CachePending;
  uint32_t _dataOffset;
  uint32_t _dataBytes;
  uint32_t _sourceFileSize;
  uint32_t _sourceWriteTime;
  uint32_t _frameCount;
  uint32_t _framePosition;
  uint32_t _sampleRate;
  uint16_t _channels;
  uint16_t _bitsPerSample;
  uint16_t _blockAlign;
  uint8_t _sourceBuffer[SourceBufferBytes];
  int16_t _outputBuffer[OutputFrames];

  AudioTags::Parser _tags;
  bool _tagsParsing;
  uint32_t _wavInfoOffset;
  uint32_t _wavInfoSize;
  uint32_t _wavId3Offset;
  uint32_t _wavId3Size;

  bool _mp3Analyzing;
  uint32_t _mp3DataOffset;
  uint32_t _mp3AudioEnd;
  uint32_t _mp3AnalysisPosition;
  uint8_t _mp3InputBuffer[Mp3InputBufferBytes];
  uint16_t _mp3InputOffset;
  uint16_t _mp3InputBytes;
  mp3dec_t _mp3Decoder;
  mp3d_sample_t _mp3PcmBuffer[Mp3MaxPcmFrames];
  uint16_t _mp3PcmFrames;
  uint16_t _mp3PcmPosition;

  bool lockBus(uint32_t waitMs);
  void unlockBus();
  bool fail(const __FlashStringHelper *message);
  bool readExact(uint8_t *buffer, size_t length);
  bool seekTo(uint32_t position);

  bool parseWav();
  bool parseFormatChunk(uint32_t offset, uint32_t size);
  void beginTags();
  static bool readTagBytes(void *context, uint32_t offset, uint8_t *buffer, size_t length);
  bool beginMp3Analysis();
  bool readMp3Bounds();
  bool loadMp3Cache();
  void writeMp3Cache();
  void analyzeMp3Step();
  bool parseMp3FrameHeader(uint32_t position,
                           uint32_t &frameBytes,
                           uint16_t &frameSamples,
                           uint32_t &sampleRate,
                           uint16_t &channels);
  bool resetMp3Decoder();
  bool refillMp3Input();
  bool decodeMp3Frame();
  bool prepareStream();
  StreamResult streamWav(SpeakerController &speaker);
  StreamResult streamMp3(SpeakerController &speaker);
  int32_t decodeSample(const uint8_t *sample) const;
};
