#pragma once

#include <Arduino.h>
#include <FS.h>

#include "../../../third_party/minimp3/minimp3.h"

#include "../../core/Screen.h"
#include "../../services/audio/AudioSettings.h"
#include "../../services/storage/SdCardController.h"
#include "../../services/storage/SecretStorage.h"
#include "../../services/audio/SpeakerController.h"
#include "../../media/MediaCache.h"

class AudioPlayerScreen : public Screen {
public:
  AudioPlayerScreen(Navigator &navigator,
                    TouchController &touch,
                    DisplayController &display,
                    SdCardController &sdCard,
                    SpeakerController &speaker,
                    AudioSettings &audioSettings,
                    SecretStorage &secretStorage,
                    MediaCache &mediaCache);

  void onEnter(const ScreenData &data) override;
  void onExit() override;
  void handleTouch(const TouchPoint &point) override;
  void tick(uint32_t nowMs) override;

private:
  enum class State : uint8_t {
    Ready,
    Playing,
    Paused,
    Finished,
    Error
  };

  enum class SourceFormat : uint8_t {
    None,
    Wav,
    Mp3
  };

  static const uint16_t SourceBufferBytes = 4096;
  static const uint16_t Mp3InputBufferBytes = 4096;
  static const uint16_t Mp3MaxPcmFrames = MINIMP3_MAX_SAMPLES_PER_FRAME;
  static const uint8_t Mp3AnalysisFramesPerTick = 16;
  static const uint16_t Mp3CacheHeaderBytes = 36;
  static const uint16_t OutputFrames = 256;
  SdCardController &_sdCard;
  SpeakerController &_speaker;
  AudioSettings &_audioSettings;
  SecretStorage &_secretStorage;
  MediaCache &_mediaCache;
  StorageFile _file;
  bool _secretAccess;
  String _path;
  String _fileName;
  String _error;
  State _state;
  SourceFormat _format;
  bool _sdMounted;
  bool _lastTouchDown;
  bool _volumePickerVisible;
  uint32_t _lastDrawMs;
  uint32_t _finishAtMs;
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

  void reset();
  bool loadSource(const String &path);
  SourceFormat formatForPath(const String &path) const;
  bool parseWav();
  bool parseFormatChunk(uint32_t offset, uint32_t size);
  bool beginMp3Analysis();
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
  bool fail(const __FlashStringHelper *message);
  bool readExact(uint8_t *buffer, size_t length);
  bool seekTo(uint32_t position);
  bool startPlayback();
  void pausePlayback();
  void finishPlayback();
  void stopSpeaker();
  void streamChunk();
  void streamWavChunk();
  void streamMp3Chunk();
  int32_t decodeSample(const uint8_t *sample) const;
  void handleSdCardEvent(SdCardEvent event);

  void draw(bool force = true);
  void drawTopBar();
  void drawDynamic(bool force);
  void drawPlayButton();
  void drawVolumeButton();
  void drawVolumePicker();
  void drawProgress();
  void drawStatus();
  void drawError();
  String displayName(const String &path) const;
  String truncateLabel(const String &label, int16_t maxWidth);
  String timeText(uint32_t frames) const;
  bool controlContains(int16_t x, int16_t y) const;
  bool volumeButtonContains(int16_t x, int16_t y) const;
  int8_t volumePickerLevelAt(int16_t x, int16_t y) const;
};
