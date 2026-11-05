#pragma once

#include <Arduino.h>

#include "../../../third_party/minimp3/minimp3.h"

#include "../../media/AudioTags.h"
#include "../../media/MediaCache.h"
#include "../../media/Mp3SeekTable.h"
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
// An MP3 plays as soon as its tags are read. The frame scan that yields the
// exact duration, the seek table, and the `.empa` cache runs alongside
// playback through its own positional reader (StorageFile::directReader), one
// window per loadStep(). Until it finishes, durationFrames() is an estimate,
// canSeek() is false, and the track ends where the stream ends. Only when no
// second descriptor is free does the scan share the stream's file, in which
// case it runs before playback as loading().
//
// Errors close the file and leave a message in error(); the tags, format, and
// duration read so far stay available so the screen can keep showing them.
// A background scan that fails only stops the scan.
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
  // Opens and validates the header. `plainName` is the decrypted file name
  // in a hidden folder (whose stored names carry no extension) and picks the
  // format. Tag parsing and any MP3 scan then run through loadStep().
  // Returns false with error() set.
  bool open(const String &path, bool secretAccess, const String &plainName);
  // Advances tag parsing, then the MP3 scan, by one bounded step. Keep calling
  // it while loading() or scanning(), including during playback.
  void loadStep();

  bool isOpen() const { return (bool)_file; }
  // Work that must finish before playback can start.
  bool loading() const { return _tagsParsing || (_mp3Analyzing && _scanShared); }
  // The MP3 frame scan, in the background or (when shared) as loading.
  bool scanning() const { return _mp3Analyzing; }
  bool tagsParsing() const { return _tagsParsing; }
  bool failed() const { return _error.length() > 0; }
  const String &error() const { return _error; }
  const AudioTags::Info &tags() const { return _tags.info(); }

  Format format() const { return _format; }
  uint32_t sampleRate() const { return _sampleRate; }
  uint16_t channels() const { return _channels; }
  uint16_t bitsPerSample() const { return _bitsPerSample; }
  // Exact decoded sample count; for an MP3 only once durationKnown().
  uint32_t frameCount() const { return _frameCount; }
  bool durationKnown() const { return _frameCountKnown; }
  // The exact count when known, otherwise the best estimate so far: an MP3's
  // Info/Xing or VBRI frame count, or its size over the first frame's size.
  uint32_t durationFrames() const;
  uint32_t framePosition() const { return _framePosition; }
  bool ended() const {
    return _streamEnded || (_frameCountKnown && _frameCount > 0 && _framePosition >= _frameCount);
  }
  // Average for MP3 (compressed bytes over duration), nominal for WAV; 0 if unknown.
  uint32_t bitrateKbps() const;

  // True when seek() accepts any frame: always for WAV, and for MP3 once the
  // scan (or a valid `.empa` v2 cache) has produced a seek table. Without
  // one an MP3 can only rewind to the start.
  bool canSeek() const;
  // Sets the position at once; the file work happens in the next stream()
  // calls. An MP3 restarts at the seek-table entry before the target and
  // decodes and discards whole frames up to it, a few per call (reported as
  // Busy).
  bool seek(uint32_t frame);

  // Decodes and writes up to one output chunk. Ended means there was nothing
  // left to write; the caller lets the speaker's queue drain.
  StreamResult stream(SpeakerController &speaker);

private:
  static const uint16_t SourceBufferBytes = 4096;
  static const uint16_t Mp3InputBufferBytes = 4096;
  static const uint16_t Mp3MaxPcmFrames = MINIMP3_MAX_SAMPLES_PER_FRAME;
  // A background scan step reads one window and parses at most this many
  // frame headers from it.
  static const uint32_t ScanWindowBytes = 8192;
  static const uint8_t Mp3AnalysisFramesPerStep = 128;
  // How much of the first frame is read to find an Info/Xing/VBRI header.
  static const uint16_t Mp3FirstFrameProbeBytes = 64;
  static const uint16_t Mp3CacheHeaderBytes = 44;
  static const uint8_t Mp3CacheVersion = 2;
  static const uint8_t Mp3DiscardFramesPerStep = 8;
  // One whole MPEG-1 frame, so each tick can queue ~26 ms of audio and the
  // speaker stays ahead of the background scan's reads.
  static const uint16_t OutputFrames = 1152;
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
  bool _frameCountKnown;
  // The stream reached its end before the scan produced the exact count.
  bool _streamEnded;
  uint32_t _estimatedFrames;
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
  // The scan's own reader and window; _scanShared when it fell back to _file.
  StorageFile _scanFile;
  bool _scanShared;
  uint8_t *_scanBuffer;
  uint32_t _scanWindowStart;
  uint32_t _scanWindowLength;
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
  // Seek table, in a PSRAM buffer allocated per MP3 open.
  uint32_t *_tocStorage;
  Mp3SeekTable _toc;
  bool _tocReady;
  uint16_t _mp3FrameSamples;
  uint32_t _mp3ScanFrames;
  // Pending work of a seek: whole frames to decode and discard, then samples
  // to drop from the first frame that is played.
  uint32_t _mp3SkipFrames;
  uint16_t _mp3SkipSamples;

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
  bool beginScanReader();
  void releaseScanReader();
  void finishScan();
  void stopScan(const __FlashStringHelper *reason);
  bool decodeMp3Header(const uint8_t *bytes,
                       uint32_t position,
                       uint32_t &frameBytes,
                       uint16_t &frameSamples,
                       uint32_t &sampleRate,
                       uint16_t &channels) const;
  bool loadMp3Cache();
  void writeMp3Cache();
  void analyzeMp3Step();
  bool parseMp3FrameHeader(uint32_t position,
                           uint32_t &frameBytes,
                           uint16_t &frameSamples,
                           uint32_t &sampleRate,
                           uint16_t &channels);
  bool allocateSeekTable();
  void releaseSeekTable();
  bool resetMp3Decoder(uint32_t offset);
  bool prepareMp3Position();
  StreamResult discardMp3Frames();
  bool refillMp3Input();
  bool decodeMp3Frame();
  bool prepareStream();
  StreamResult streamWav(SpeakerController &speaker);
  StreamResult streamMp3(SpeakerController &speaker);
  int32_t decodeSample(const uint8_t *sample) const;
};
