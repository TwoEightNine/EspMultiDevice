#pragma once

#include <Arduino.h>
#include <FS.h>

#include "Screen.h"
#include "SdCardController.h"
#include "SpeakerController.h"
#include "SecretStorage.h"
#include "VideoH264.h"
#include "VideoLegacy.h"

struct MP4D_demux_tag;
typedef struct MP4D_demux_tag MP4D_demux_t;

struct VideoPresentationPoint {
  int64_t timestamp;
};

class VideoPlayerScreen : public Screen {
public:
  VideoPlayerScreen(Navigator &navigator, TouchController &touch, DisplayController &display,
                    SdCardController &sdCard, SpeakerController &speaker, SecretStorage &secretStorage);
  ~VideoPlayerScreen() override;

  void onEnter(const ScreenData &data) override;
  void onExit() override;
  void handleTouch(const TouchPoint &point) override;
  void tick(uint32_t nowMs) override;

private:
  enum class State : uint8_t { Ready, Building, Playing, Paused, Finished, Error };
  enum class AudioCodec : uint8_t { None, Aac, Mp3, AmrNb, Unsupported };
  enum class VideoCodec : uint8_t { None, H264, H263, Mpeg4Part2 };

  static const uint16_t MaxVideoWidth = 320;
  static const uint16_t MaxVideoHeight = 240;
  static const uint32_t MinFrameDelayMs = 100;
  static const uint16_t IoChunkBytes = 8192;
  static const uint16_t PcmFrames = 512;

  SdCardController &_sdCard;
  SpeakerController &_speaker;
  SecretStorage &_secretStorage;
  StorageFile _source;
  StorageFile _cache;
  StorageFile _cacheRead;
  bool _secretAccess;
  MP4D_demux_t *_demux;
  VideoH264 _h264;
  VideoLegacy _legacy;
  String _path;
  String _fileName;
  String _cachePath;
  String _cacheTempPath;
  String _error;
  String _audioStatus;
  String _audioDiagnostic;
  State _state;
  AudioCodec _audioCodec;
  VideoCodec _videoCodec;
  bool _sdMounted;
  bool _lastTouchDown;
  uint32_t _sourceSize;
  uint32_t _sourceWriteTime;
  uint16_t _sourceWidth;
  uint16_t _sourceHeight;
  uint16_t _drawWidth;
  uint16_t _drawHeight;
  int16_t _drawLeft;
  int16_t _drawTop;
  uint32_t _videoTrack;
  uint32_t _audioTrack;
  uint32_t _videoSamples;
  uint32_t _audioSamples;
  uint32_t _videoSample;
  uint32_t _audioSample;
  uint32_t _videoTimeScale;
  uint32_t _audioTimeScale;
  uint32_t _buildFrames;
  uint32_t _buildLastTimestamp;
  uint32_t _buildLastDelay;
  VideoPresentationPoint *_presentationTimeline;
  uint32_t _presentationCount;
  uint32_t _presentationIndex;
  bool _buildFlushed;
  uint32_t _playFrame;
  uint32_t _playFrames;
  uint32_t _playDelay;
  uint32_t _playStartedAt;
  uint32_t _nextFrameAt;
  uint32_t _pauseElapsedMs;
  uint32_t _pauseFrameRemainingMs;
  uint32_t _durationMs;
  uint32_t _lastDrawMs;
  uint32_t _audioSampleRate;
  uint32_t _audioFramesWritten;
  uint32_t _audioTailStartedAt;
  uint16_t _audioChannels;
  uint8_t *_sampleBuffer;
  uint32_t _sampleBufferBytes;
  uint8_t *_annexBuffer;
  uint32_t _annexBufferBytes;
  uint16_t *_pendingFrame;
  bool _hasPendingFrame;
  uint16_t *_playFrameBuffer;
  int16_t *_audioScratch;
  uint8_t _cacheChunk[IoChunkBytes];
  int16_t _pcm[PcmFrames];
  void *_aac;
  void *_mp3;
  void *_amr;

  void reset();
  void releaseDemux();
  void releaseAudioDecoder();
  bool openSource();
  bool inspectSource();
  bool loadCache();
  bool beginBuild();
  bool buildStep();
  bool preparePresentationTimeline();
  void releasePresentationTimeline();
  bool appendPendingFrame(uint32_t delayMs);
  bool captureDecodedFrame(const VideoH264::Frame &frame);
  static bool decodedFrame(const VideoH264::Frame &frame, void *context);
  bool finishBuild();
  bool startPlayback();
  void pausePlayback();
  void finishPlayback();
  bool loadPlaybackFrame(uint32_t frame);
  void tickPlayback(uint32_t nowMs);
  void pumpAudio(uint32_t elapsedMs);
  bool decodeAudioSample();
  bool decodeAacSample(const uint8_t *bytes, uint32_t length);
  bool decodeMp3Sample(const uint8_t *bytes, uint32_t length);
  bool decodeAmrNbSample(const uint8_t *bytes, uint32_t length);
  bool readSample(uint32_t track, uint32_t sample, uint32_t &bytes, uint32_t &timestamp, uint32_t &duration);
  bool ensureSampleBuffer(uint32_t bytes);
  bool ensureAnnexBuffer(uint32_t bytes);
  bool buildAnnexB(const uint8_t *sample, uint32_t bytes, bool includeParameters, uint32_t &outputBytes);
  bool writeFrameRgb444(const uint16_t *pixels, uint32_t delayMs);
  bool readCacheHeader(uint32_t &frames, uint32_t &duration);
  bool parseAacConfig();
  void logAudioTrack() const;
  uint32_t timestampMs(uint32_t timestamp, uint32_t timeScale) const;
  uint32_t presentationTimestampMs(int64_t timestamp, uint32_t timeScale) const;
  uint32_t playbackElapsedMs(uint32_t nowMs) const;
  String cachePathForSource() const;
  String displayName(const String &path) const;
  bool fail(const String &message);
  bool fail(const __FlashStringHelper *message);
  void handleSdCardEvent(SdCardEvent event);
  void stopSpeaker();
  void draw(bool full = true);
  void drawTopBar();
  void drawPlayButton();
  void drawProgress();
  void drawStatus();
  void drawError();
  bool controlContains(int16_t x, int16_t y) const;
};
