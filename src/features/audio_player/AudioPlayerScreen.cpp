#include "AudioPlayerScreen.h"

#include "../../app/Navigator.h"
#include "../../ui/UiKit.h"

#include <esp_heap_caps.h>
#include <math.h>

namespace {
// Layout of the 320 x 480 portrait screen; see docs/ui-kit.md.
constexpr int16_t CoverSize = AudioCoverWorker::Size;
constexpr int16_t CoverTop = 64;
constexpr int16_t CoverRadius = AudioCoverWorker::CornerRadius;
constexpr int16_t CoverGlyphSize = 88;
constexpr uint16_t CoverColor = AudioCoverWorker::TileColor;
constexpr uint16_t CoverGlyphColor = 0x52AA;  // #555555

constexpr int16_t TextTop = 274;
constexpr int16_t TextBottom = 347;
constexpr int16_t TitleY = 288;   // Text rows are vertical centres.
constexpr int16_t ArtistY = 314;
constexpr int16_t AlbumY = 336;
constexpr int16_t TextMargin = 16;

constexpr int16_t SeekX = 16;
constexpr int16_t SeekY = 360;
constexpr int16_t SeekHitTop = 344;
constexpr int16_t SeekHitBottom = 384;
constexpr uint16_t TrackColor = 0x4208;  // #444444
constexpr int16_t TimeY = 386;
constexpr int16_t TimeTop = 377;
constexpr int16_t TimeHeight = 19;
constexpr int16_t TimeWidth = 96;

constexpr int16_t TransportY = 436;
constexpr int16_t CellHalf = 24;
constexpr int16_t PlayRadius = 32;
constexpr int16_t RepeatX = 40;
constexpr int16_t PreviousX = 100;
constexpr int16_t NextX = 220;
constexpr int16_t VolumeX = 280;
constexpr uint16_t PlayDisabledColor = 0x31A6;  // #333333
constexpr uint16_t InactiveColor = TFT_DARKGREY;

constexpr int16_t VolumePanelX = 16;
constexpr int16_t VolumePanelY = 336;
constexpr int16_t VolumePanelHeight = 60;
constexpr int16_t VolumePanelRadius = 12;
constexpr int16_t VolumeClearTop = 326;  // Below the artist line.
constexpr uint16_t PanelColor = 0x2104;
constexpr uint16_t PanelTrackColor = 0x52AA;
constexpr int16_t VolumeRowY = VolumePanelY + VolumePanelHeight / 2;
constexpr int16_t VolumeIconX = 44;
constexpr int16_t VolumeSliderX = 76;
constexpr int16_t VolumeSliderWidth = 172;
constexpr int16_t VolumeSliderHitLeft = 60;
constexpr int16_t VolumeSliderHitRight = 268;
constexpr int16_t VolumeValueX = 278;
constexpr uint32_t VolumeTimeoutMs = 3000;

constexpr int16_t InfoX = 16;
constexpr int16_t InfoTop = 96;
constexpr int16_t InfoRowHeight = 30;
constexpr int16_t InfoRowsTop = 92;  // Relative to InfoTop.
constexpr uint8_t InfoRowCount = 5;
constexpr int16_t InfoHeight = InfoRowsTop + InfoRowHeight * InfoRowCount + 34;

constexpr uint32_t PlayingRedrawMs = 200;
constexpr uint32_t LoadingRedrawMs = 100;

// Timestamps taken with millis() during a tick can be newer than its nowMs.
uint32_t elapsedSince(uint32_t nowMs, uint32_t thenMs) {
  return (int32_t)(nowMs - thenMs) > 0 ? nowMs - thenMs : 0;
}

float clampUnit(float value) {
  if (!(value > 0.0f)) return 0.0f;
  return value > 1.0f ? 1.0f : value;
}

String joinDot(const String &left, const String &right) {
  if (left.length() == 0) return right;
  if (right.length() == 0) return left;
  return left + F(" \xC2\xB7 ") + right;
}

// Splits text into a first line that fits `width` (broken at the last space
// when there is one) and the remainder.
void splitLine(const String &text, int16_t width, UiText::Size size, String &first, String &rest) {
  if (UiText::measure(text, size) <= width) {
    first = text;
    rest = String();
    return;
  }
  int fit = 0;
  int lastSpace = -1;
  for (int i = 1; i <= (int)text.length(); i++) {
    if (i < (int)text.length() && ((uint8_t)text[i] & 0xC0) == 0x80) continue;
    if (UiText::measure(text.substring(0, i), size) > width) break;
    fit = i;
    if (text[i - 1] == ' ') lastSpace = i;
  }
  if (lastSpace > 0) fit = lastSpace;
  if (fit == 0) fit = 1;
  first = text.substring(0, fit);
  rest = text.substring(fit);
  first.trim();
  rest.trim();
}
}  // namespace

AudioPlayerScreen::AudioPlayerScreen(Navigator &navigator,
                                     TouchController &touch,
                                     DisplayController &display,
                                     SdCardController &sdCard,
                                     SpeakerController &speaker,
                                     AudioSettings &audioSettings,
                                     SecretStorage &secretStorage,
                                     MediaCache &mediaCache)
  : Screen(navigator, touch, display),
    _sdCard(sdCard),
    _speaker(speaker),
    _audioSettings(audioSettings),
    _source(secretStorage, mediaCache),
    _cover(secretStorage),
    _coverPixels(nullptr),
    _coverGeneration(0),
    _secretAccess(false),
    _sdMounted(false),
    _pendingOpen(false),
    _lastTouchDown(false),
    _state(State::Ready),
    _repeat(Repeat::Off),
    _drag(Drag::None),
    _dragFraction(0.0f),
    _dragVolume(0),
    _volumeOpen(false),
    _infoOpen(false),
    _volumeTouchMs(0),
    _loadingStartMs(0),
    _lastDrawMs(0),
    _finishAtMs(0),
    _seekDrawn(false),
    _drawnThumb(false),
    _drawnFill(0),
    _drawnFraction(0.0f),
    _drawnElapsed(-1),
    _drawnRemaining(-1) {
}

void AudioPlayerScreen::onEnter(const ScreenData &data) {
  stopSpeaker();
  _source.reset();
  dropCover();
  _lastTouchDown = false;
  _volumeOpen = false;
  _infoOpen = false;
  _drag = Drag::None;
  _repeat = Repeat::Off;
  _state = State::Ready;
  _error = "";
  _finishAtMs = 0;
  _secretAccess = data.fileAccess == FileAccessMode::Secret;
  _path = data.kind == ScreenDataKind::AudioPath ? data.text : "";
  int16_t slash = _path.lastIndexOf('/');
  _fileName = slash < 0 ? _path : _path.substring(slash + 1);
  _sdMounted = _sdCard.isMounted();
  _pendingOpen = false;
  _loadingStartMs = millis();

  if (data.kind != ScreenDataKind::AudioPath || _path.length() == 0) {
    fail(F("No audio selected"));
  } else if (!_sdMounted) {
    fail(F("SD card unavailable"));
  } else {
    // Show the player first; the file is opened from the next tick().
    _pendingOpen = true;
  }
  draw();
}

void AudioPlayerScreen::onExit() {
  if (_drag != Drag::None) endDrag();
  stopSpeaker();
  _cover.cancel();
  _source.closeFile();
  _pendingOpen = false;
}

bool AudioPlayerScreen::releaseResources() {
  dropCover();
  // Frees the MP3 seek table as well as the file.
  _source.reset();
  // False until the worker has unwound its job; Navigator retries.
  return _cover.end();
}

void AudioPlayerScreen::handleTouch(const TouchPoint &point) {
  if (!point.touched) {
    _lastTouchDown = false;
    if (_drag != Drag::None) endDrag();
    return;
  }
  if (_lastTouchDown) return;
  _lastTouchDown = true;
  const int16_t x = point.x;
  const int16_t y = point.y;

  if (UiKit::navigationIconContains(x, y)) {
    _navigator.pop();
    return;
  }
  if (_infoOpen) {
    closeInfo();
    return;
  }
  if (UiKit::appBarActionContains(x, y, _display.width())) {
    openInfo();
    return;
  }
  if (_volumeOpen) {
    if (cellContains(VolumeX, x, y)) {
      closeVolume();
    } else if (volumeSliderContains(x, y)) {
      beginDrag(Drag::Volume, x);
    } else if (volumePanelContains(x, y)) {
      _volumeTouchMs = millis();
    } else {
      closeVolume();
    }
    return;
  }
  if (seekContains(x, y)) {
    if (_source.canSeek() && _state != State::Error) beginDrag(Drag::Seek, x);
    return;
  }
  if (playContains(x, y)) {
    if (_state == State::Playing) {
      pausePlayback();
    } else {
      startPlayback();
    }
  } else if (cellContains(PreviousX, x, y)) {
    restartTrack();
  } else if (cellContains(RepeatX, x, y)) {
    cycleRepeat();
  } else if (cellContains(VolumeX, x, y)) {
    openVolume();
  }
}

void AudioPlayerScreen::tick(uint32_t nowMs) {
  handleSdCardEvent(_sdCard.takeEvent());
  takeCover();
  if (_drag != Drag::None) {
    const TouchPoint held = _touch.point();
    if (held.touched) updateDrag(held.x);
  }
  if (_volumeOpen && _drag != Drag::Volume && elapsedSince(nowMs, _volumeTouchMs) >= VolumeTimeoutMs) {
    closeVolume();
  }

  if (_pendingOpen) {
    _pendingOpen = false;
    openSource();
    refresh();
    return;
  }
  if (_source.loading()) {
    const bool hadTags = _source.tagsParsing();
    _source.loadStep();
    if (_source.failed()) {
      failFromSource();
      refresh();
      return;
    }
    if (hadTags && !_source.tagsParsing()) {
      drawTrackText();
      requestCover();
    }
    if (!_source.loading()) {
      onSourceLoaded();
    } else if (elapsedSince(nowMs, _lastDrawMs) >= LoadingRedrawMs) {
      drawSeekRow(false);
    }
    return;
  }

  if (_state != State::Playing) return;
  if (_source.ended()) {
    if ((int32_t)(nowMs - _finishAtMs) >= 0) finishPlayback();
    return;
  }
  if (_source.stream(_speaker) == AudioSource::StreamResult::Failed) {
    failFromSource();
    refresh();
    return;
  }
  if (_source.ended()) {
    if (_repeat != Repeat::Off && _source.seek(0)) {
      // Repeat without stopping the speaker, so there is no gap.
      drawSeekRow(false);
    } else {
      // Let the speaker's DMA queue drain before uninstalling the I2S driver.
      _finishAtMs = millis() + 100;
    }
  }
  if (elapsedSince(nowMs, _lastDrawMs) >= PlayingRedrawMs) drawSeekRow(false);
}

bool AudioPlayerScreen::openSource() {
  // A remount reopens the same track: keep a cover that is already shown.
  _cover.cancel();
  ++_coverGeneration;
  if (_coverGeneration == 0) ++_coverGeneration;
  _error = "";
  _state = State::Ready;
  _finishAtMs = 0;
  _loadingStartMs = millis();
  if (!_source.open(_path, _secretAccess)) {
    failFromSource();
    return false;
  }
  return true;
}

void AudioPlayerScreen::onSourceLoaded() {
  drawTrackText();
  if (!startPlayback()) {
    refresh();
    return;
  }
  drawSeekRow(true);
  drawTransport();
  if (_infoOpen) drawTrackInfo();
}

bool AudioPlayerScreen::playable() const {
  return _source.isOpen() && _state != State::Error && _sdMounted && !loading();
}

bool AudioPlayerScreen::startPlayback() {
  if (!playable()) return false;
  if (_state == State::Finished && !_source.seek(0)) {
    failFromSource();
    refresh();
    return false;
  }
  _finishAtMs = 0;
  if (!_speaker.start(_source.sampleRate())) {
    fail(_speaker.lastError());
    refresh();
    return false;
  }
  _state = State::Playing;
  drawPlayButton();
  return true;
}

void AudioPlayerScreen::pausePlayback() {
  stopSpeaker();
  _state = State::Paused;
  drawPlayButton();
}

void AudioPlayerScreen::finishPlayback() {
  // Reached only by seeking to the end; streaming handles repeat itself.
  if (_repeat != Repeat::Off && _source.seek(0)) {
    _finishAtMs = 0;
    drawSeekRow(false);
    return;
  }
  stopSpeaker();
  _state = State::Finished;
  drawSeekRow(false);
  drawPlayButton();
}

void AudioPlayerScreen::restartTrack() {
  if (!playable() || !_source.seek(0)) return;
  _finishAtMs = 0;
  if (_state == State::Finished) {
    startPlayback();
  }
  drawSeekRow(false);
}

void AudioPlayerScreen::cycleRepeat() {
  _repeat = _repeat == Repeat::Off ? Repeat::All
          : _repeat == Repeat::All ? Repeat::One
                                   : Repeat::Off;
  drawRepeatCell();
}

void AudioPlayerScreen::fail(const String &message) {
  stopSpeaker();
  _source.closeFile();
  _pendingOpen = false;
  _drag = Drag::None;
  _error = message;
  _state = State::Error;
}

void AudioPlayerScreen::failFromSource() {
  fail(_source.error().length() > 0 ? _source.error() : String(F("Audio playback failed")));
}

void AudioPlayerScreen::stopSpeaker() {
  _speaker.stop();
}

void AudioPlayerScreen::handleSdCardEvent(SdCardEvent event) {
  if (event == SdCardEvent::None) return;
  if (event == SdCardEvent::Removed || event == SdCardEvent::MountFailed) {
    _sdMounted = false;
    _cover.cancel();
    fail(event == SdCardEvent::Removed ? F("SD card removed") : F("SD card unavailable"));
    refresh();
    return;
  }
  if (event == SdCardEvent::Mounted) {
    _sdMounted = true;
    if (_path.length() > 0) _pendingOpen = true;
    refresh();
  }
}

void AudioPlayerScreen::requestCover() {
  const AudioTags::Info &tags = _source.tags();
  if (_coverPixels || !tags.hasCover()) return;
  if (!_cover.request(_path, _secretAccess, tags.coverOffset, tags.coverLength, _coverGeneration)) {
    Serial.println(F("[Audio cover] worker unavailable"));
  }
}

void AudioPlayerScreen::takeCover() {
  AudioCoverWorker::Result result{};
  while (_cover.take(result)) {
    if (result.generation != _coverGeneration || !result.pixels || _coverPixels) {
      heap_caps_free(result.pixels);
      continue;
    }
    _coverPixels = result.pixels;
    drawCover();
  }
}

void AudioPlayerScreen::dropCover() {
  _cover.cancel();
  ++_coverGeneration;
  if (_coverGeneration == 0) ++_coverGeneration;
  heap_caps_free(_coverPixels);
  _coverPixels = nullptr;
}

void AudioPlayerScreen::beginDrag(Drag drag, int16_t x) {
  if (drag == Drag::Seek) {
    _dragFraction = seekFractionAt(x);
    _drag = drag;
    drawSeekRow(false);
  } else {
    _dragVolume = _audioSettings.volume();
    _drag = drag;
    updateDrag(x);
  }
}

void AudioPlayerScreen::updateDrag(int16_t x) {
  if (_drag == Drag::Seek) {
    const float fraction = seekFractionAt(x);
    if (fraction == _dragFraction) return;
    _dragFraction = fraction;
    drawSeekRow(false);
  } else if (_drag == Drag::Volume) {
    _volumeTouchMs = millis();
    const uint8_t level = volumeLevelAt(x);
    if (level == _dragVolume) return;
    const int16_t previous = _dragVolume;
    _dragVolume = level;
    // Live while dragging; AudioSettings persists once, on release.
    _speaker.setVolume(level);
    drawVolumeSlider(previous);
    drawVolumeCell();
  }
}

void AudioPlayerScreen::endDrag() {
  const Drag drag = _drag;
  _drag = Drag::None;
  if (drag == Drag::Seek) {
    const uint32_t frame = (uint32_t)lroundf(_dragFraction * _source.frameCount());
    if (_source.seek(frame)) {
      if (_state == State::Finished && !_source.ended()) {
        _state = State::Paused;
        drawPlayButton();
      } else if (_state == State::Playing && _source.ended()) {
        _finishAtMs = millis();
      }
    }
    drawSeekRow(false);
  } else if (drag == Drag::Volume) {
    _volumeTouchMs = millis();
    if (!_audioSettings.setVolume(_dragVolume)) {
      const int16_t previous = _dragVolume;
      _speaker.setVolume(_audioSettings.volume());
      if (_volumeOpen) drawVolumeSlider(previous);
      drawVolumeCell();
    }
  }
}

float AudioPlayerScreen::seekFractionAt(int16_t x) const {
  const int16_t width = _display.width() - SeekX * 2;
  return clampUnit((float)(x - SeekX) / width);
}

uint8_t AudioPlayerScreen::volumeLevelAt(int16_t x) const {
  const float fraction = clampUnit((float)(x - VolumeSliderX) / VolumeSliderWidth);
  return (uint8_t)lroundf(fraction * AudioSettings::MaxVolume);
}

uint8_t AudioPlayerScreen::currentVolume() const {
  return _drag == Drag::Volume ? _dragVolume : _audioSettings.volume();
}

uint32_t AudioPlayerScreen::displayedFrame() const {
  if (_drag == Drag::Seek) return (uint32_t)lroundf(_dragFraction * _source.frameCount());
  return _source.framePosition();
}

void AudioPlayerScreen::openVolume() {
  _volumeOpen = true;
  _volumeTouchMs = millis();
  drawVolumePanel();
}

void AudioPlayerScreen::closeVolume() {
  if (_drag == Drag::Volume) endDrag();
  _volumeOpen = false;
  if (!_display.isReady() || _infoOpen) return;
  _display.tft().fillRect(0, VolumeClearTop, _display.width(),
                          VolumePanelY + VolumePanelHeight - VolumeClearTop, TFT_BLACK);
  drawTrackText();
  drawSeekRow(true);
}

void AudioPlayerScreen::openInfo() {
  if (_drag != Drag::None) endDrag();
  if (_volumeOpen && _display.isReady()) {
    // The sheet does not reach the panel's bottom edge; close() redraws all.
    _display.tft().fillRect(0, VolumeClearTop, _display.width(),
                            VolumePanelY + VolumePanelHeight - VolumeClearTop, TFT_BLACK);
  }
  _volumeOpen = false;
  _infoOpen = true;
  drawTrackInfo();
}

void AudioPlayerScreen::closeInfo() {
  _infoOpen = false;
  draw();
}

void AudioPlayerScreen::draw() {
  if (!_display.isReady()) return;
  TFT_eSPI &tft = _display.tft();
  tft.fillScreen(TFT_BLACK);
  UiKit::drawTopAppBar(tft, _display.width(), F("Now playing"),
                       UiKit::NavigationIcon::Close, UiKit::AppBarAction::More);
  drawCover();
  drawTrackText();
  drawSeekRow(true);
  drawTransport();
  if (_volumeOpen) drawVolumePanel();
  if (_infoOpen) drawTrackInfo();
}

// Repaints everything that depends on the playback state, without the cover.
void AudioPlayerScreen::refresh() {
  drawTrackText();
  drawSeekRow(true);
  drawTransport();
}

void AudioPlayerScreen::drawCover() {
  if (!_display.isReady() || _infoOpen) return;
  TFT_eSPI &tft = _display.tft();
  const int16_t x = (_display.width() - CoverSize) / 2;
  if (_coverPixels) {
    // The worker already blended the rounded corners against black.
    tft.pushImage(x, CoverTop, CoverSize, CoverSize, _coverPixels);
    return;
  }
  tft.fillSmoothRoundRect(x, CoverTop, CoverSize, CoverSize, CoverRadius, CoverColor, TFT_BLACK);
  UiKit::drawMusicNoteIcon(tft, _display.width() / 2, CoverTop + CoverSize / 2, CoverGlyphSize,
                           CoverGlyphColor, CoverColor);
}

void AudioPlayerScreen::drawTrackText() {
  if (!_display.isReady() || _infoOpen) return;
  TFT_eSPI &tft = _display.tft();
  const int16_t width = _display.width();
  const int16_t centerX = width / 2;
  const int16_t textWidth = width - TextMargin * 2;
  // The volume panel covers the album line.
  const int16_t bottom = _volumeOpen ? VolumeClearTop : TextBottom;
  tft.fillRect(0, TextTop, width, bottom - TextTop, TFT_BLACK);

  UiText::draw(tft, UiText::ellipsize(trackTitle(), textWidth, UiText::Size::Title), centerX, TitleY,
               TFT_WHITE, TFT_BLACK, UiText::Size::Title, MC_DATUM);

  String line;
  uint16_t color = TFT_LIGHTGREY;
  if (_state == State::Error) {
    line = _error;
    color = TFT_RED;
  } else if (tagsPending()) {
    line = F("Reading track...");
    color = InactiveColor;
  } else if (_source.tags().artist[0]) {
    line = _source.tags().artist;
  } else {
    line = F("Unknown artist");
    color = InactiveColor;
  }
  UiText::draw(tft, UiText::ellipsize(line, textWidth, UiText::Size::Content), centerX, ArtistY,
               color, TFT_BLACK, UiText::Size::Content, MC_DATUM);

  if (_volumeOpen) return;
  const String album = albumLine();
  if (album.length() > 0) {
    UiText::draw(tft, UiText::ellipsize(album, textWidth, UiText::Size::Compact), centerX, AlbumY,
                 InactiveColor, TFT_BLACK, UiText::Size::Compact, MC_DATUM);
  }
}

void AudioPlayerScreen::drawSeekRow(bool force) {
  if (!_display.isReady() || _infoOpen || _volumeOpen) return;
  TFT_eSPI &tft = _display.tft();
  const int16_t width = _display.width() - SeekX * 2;
  _lastDrawMs = millis();

  if (loading()) {
    if (force || _seekDrawn) {
      tft.fillRect(0, SeekY - UiKit::SeekBarBandHalf, _display.width(),
                   UiKit::SeekBarBandHalf * 2 + 1, TFT_BLACK);
    }
    UiKit::drawProgressBar(tft, SeekX, SeekY - 4, width, 8, 0, true, TFT_CYAN, TrackColor,
                           TFT_BLACK, true, millis() - _loadingStartMs);
    _seekDrawn = false;
    drawTimes(force);
    return;
  }

  const uint32_t frames = _source.frameCount();
  const float fraction = frames > 0 ? clampUnit((float)displayedFrame() / frames) : 0.0f;
  const bool thumb = _source.canSeek() && _state != State::Error;
  const uint16_t fill = _state == State::Error ? TrackColor : TFT_CYAN;
  if (force || !_seekDrawn || thumb != _drawnThumb || fill != _drawnFill) {
    UiKit::drawSeekBar(tft, SeekX, SeekY, width, fraction, thumb, fill, TrackColor, TFT_BLACK);
  } else {
    UiKit::updateSeekBar(tft, SeekX, SeekY, width, _drawnFraction, fraction, thumb, fill,
                         TrackColor, TFT_BLACK);
  }
  _seekDrawn = true;
  _drawnThumb = thumb;
  _drawnFill = fill;
  _drawnFraction = fraction;
  drawTimes(force);
}

void AudioPlayerScreen::drawTimes(bool force) {
  TFT_eSPI &tft = _display.tft();
  const int16_t right = _display.width() - SeekX;
  int32_t elapsed = -1;
  int32_t remaining = -1;
  const uint32_t rate = _source.sampleRate();
  if (!loading() && rate > 0 && _source.frameCount() > 0) {
    const uint32_t total = _source.frameCount() / rate;
    elapsed = (int32_t)(displayedFrame() / rate);
    remaining = total > (uint32_t)elapsed ? (int32_t)(total - elapsed) : 0;
  }
  if (force || elapsed != _drawnElapsed) {
    tft.fillRect(SeekX, TimeTop, TimeWidth, TimeHeight, TFT_BLACK);
    if (elapsed >= 0) {
      UiText::draw(tft, timeText(elapsed), SeekX, TimeY, TFT_LIGHTGREY, TFT_BLACK,
                   UiText::Size::Compact, ML_DATUM);
    }
  }
  if (force || remaining != _drawnRemaining) {
    tft.fillRect(right - TimeWidth, TimeTop, TimeWidth, TimeHeight, TFT_BLACK);
    if (remaining >= 0) {
      UiText::draw(tft, String('-') + timeText(remaining), right, TimeY, TFT_LIGHTGREY,
                   TFT_BLACK, UiText::Size::Compact, MR_DATUM);
    }
  }
  _drawnElapsed = elapsed;
  _drawnRemaining = remaining;
}

void AudioPlayerScreen::drawTransport() {
  if (!_display.isReady()) return;
  drawRepeatCell();
  drawPlayButton();  // Also draws the previous cell.
  drawNextCell();
  drawVolumeCell();
}

void AudioPlayerScreen::drawRepeatCell() {
  if (!_display.isReady()) return;
  TFT_eSPI &tft = _display.tft();
  tft.fillRect(RepeatX - CellHalf, TransportY - CellHalf, CellHalf * 2, CellHalf * 2, TFT_BLACK);
  UiKit::drawRepeatIcon(tft, RepeatX, TransportY, _repeat == Repeat::One,
                        _repeat == Repeat::Off ? TFT_WHITE : TFT_CYAN, TFT_BLACK);
}

void AudioPlayerScreen::drawPreviousCell() {
  if (!_display.isReady()) return;
  TFT_eSPI &tft = _display.tft();
  tft.fillRect(PreviousX - CellHalf, TransportY - CellHalf, CellHalf * 2, CellHalf * 2, TFT_BLACK);
  UiKit::drawSkipIcon(tft, PreviousX, TransportY, false,
                      playable() ? TFT_WHITE : InactiveColor, TFT_BLACK);
}

void AudioPlayerScreen::drawPlayButton() {
  if (!_display.isReady()) return;
  TFT_eSPI &tft = _display.tft();
  const int16_t centerX = _display.width() / 2;
  const bool enabled = playable();
  const uint16_t fill = enabled ? TFT_WHITE : PlayDisabledColor;
  tft.fillSmoothCircle(centerX, TransportY, PlayRadius, fill, TFT_BLACK);
  const bool playing = _state == State::Playing;
  // The triangle's mass sits left of its bounding box; nudge it for balance.
  UiKit::drawPlaybackIcon(tft, playing ? centerX : centerX - 1, TransportY, playing,
                          enabled ? TFT_BLACK : InactiveColor, fill, 2.0f);
  // Prev's enabled state follows the same conditions.
  drawPreviousCell();
}

void AudioPlayerScreen::drawNextCell() {
  if (!_display.isReady()) return;
  TFT_eSPI &tft = _display.tft();
  tft.fillRect(NextX - CellHalf, TransportY - CellHalf, CellHalf * 2, CellHalf * 2, TFT_BLACK);
  // Inactive until the player has a folder queue.
  UiKit::drawSkipIcon(tft, NextX, TransportY, true, InactiveColor, TFT_BLACK);
}

void AudioPlayerScreen::drawVolumeCell() {
  if (!_display.isReady()) return;
  TFT_eSPI &tft = _display.tft();
  tft.fillRect(VolumeX - CellHalf, TransportY - CellHalf, CellHalf * 2, CellHalf * 2, TFT_BLACK);
  UiKit::drawVolumeIcon(tft, VolumeX, TransportY, currentVolume() == 0,
                        _volumeOpen ? TFT_CYAN : TFT_WHITE, TFT_BLACK);
}

void AudioPlayerScreen::drawVolumePanel() {
  if (!_display.isReady() || _infoOpen) return;
  TFT_eSPI &tft = _display.tft();
  const int16_t panelWidth = _display.width() - VolumePanelX * 2;
  tft.fillRect(0, VolumeClearTop, _display.width(),
               VolumePanelY + VolumePanelHeight - VolumeClearTop, TFT_BLACK);
  tft.fillSmoothRoundRect(VolumePanelX, VolumePanelY, panelWidth, VolumePanelHeight,
                          VolumePanelRadius, PanelColor, TFT_BLACK);
  drawVolumeSlider(-1);
  drawVolumeCell();
}

void AudioPlayerScreen::drawVolumeSlider(int16_t previousLevel) {
  if (!_display.isReady() || !_volumeOpen || _infoOpen) return;
  TFT_eSPI &tft = _display.tft();
  const uint8_t level = currentVolume();
  const float fraction = (float)level / AudioSettings::MaxVolume;
  if (previousLevel < 0) {
    UiKit::drawSeekBar(tft, VolumeSliderX, VolumeRowY, VolumeSliderWidth, fraction, true,
                       TFT_CYAN, PanelTrackColor, PanelColor);
  } else {
    UiKit::updateSeekBar(tft, VolumeSliderX, VolumeRowY, VolumeSliderWidth,
                         (float)previousLevel / AudioSettings::MaxVolume, fraction, true,
                         TFT_CYAN, PanelTrackColor, PanelColor);
  }
  tft.fillRect(VolumeIconX - 14, VolumeRowY - 14, 28, 28, PanelColor);
  UiKit::drawVolumeIcon(tft, VolumeIconX, VolumeRowY, level == 0, TFT_WHITE, PanelColor);
  tft.fillRect(VolumeValueX - 18, VolumeRowY - 12, 36, 24, PanelColor);
  UiText::draw(tft, String(level), VolumeValueX, VolumeRowY, TFT_WHITE, PanelColor,
               UiText::Size::Content, MC_DATUM);
}

void AudioPlayerScreen::drawTrackInfo() {
  if (!_display.isReady()) return;
  TFT_eSPI &tft = _display.tft();
  const int16_t width = _display.width() - InfoX * 2;
  const int16_t left = InfoX + UiKit::ContentPadding;
  const int16_t right = InfoX + width - UiKit::ContentPadding;
  const int16_t textWidth = right - left;
  UiKit::drawPopupMenuFrame(tft, InfoX, InfoTop, width, InfoHeight, TFT_BLACK);

  UiText::draw(tft, F("File"), left, InfoTop + 20, InactiveColor, TFT_BLACK,
               UiText::Size::Compact, ML_DATUM);
  String first;
  String rest;
  splitLine(_fileName, textWidth, UiText::Size::Content, first, rest);
  UiText::draw(tft, first, left, InfoTop + 44, TFT_WHITE, TFT_BLACK, UiText::Size::Content,
               ML_DATUM);
  if (rest.length() > 0) {
    UiText::draw(tft, UiText::ellipsize(rest, textWidth, UiText::Size::Content), left,
                 InfoTop + 68, TFT_WHITE, TFT_BLACK, UiText::Size::Content, ML_DATUM);
  }

  const AudioSource::Format format = _source.format();
  const uint32_t rate = _source.sampleRate();
  const bool known = rate > 0 && !_source.loading();
  String formatText = format == AudioSource::Format::Mp3 ? String(F("MP3"))
                    : format == AudioSource::Format::Wav ? String(F("WAV"))
                                                         : String(F("Unknown"));
  if (format == AudioSource::Format::Wav && _source.bitsPerSample() > 0) {
    formatText += F(", ");
    formatText += _source.bitsPerSample();
    formatText += F("-bit PCM");
  }
  String rateText = F("\xE2\x80\x94");
  if (rate > 0) {
    char buffer[16];
    if (rate % 1000 == 0) {
      snprintf(buffer, sizeof(buffer), "%lu kHz", (unsigned long)(rate / 1000));
    } else {
      snprintf(buffer, sizeof(buffer), "%.1f kHz", rate / 1000.0f);
    }
    rateText = buffer;
  }
  const uint16_t channels = _source.channels();
  String channelText = channels == 1 ? String(F("Mono"))
                     : channels == 2 ? String(F("Stereo"))
                                     : String(F("\xE2\x80\x94"));
  String bitrateText = F("\xE2\x80\x94");
  const uint32_t kbps = known ? _source.bitrateKbps() : 0;
  if (kbps > 0) {
    bitrateText = String(kbps) + F(" kbps");
    if (format == AudioSource::Format::Mp3) bitrateText += F(" avg");
  }
  String durationText = _source.loading() ? String(F("Scanning...")) : String(F("\xE2\x80\x94"));
  if (known && _source.frameCount() > 0) durationText = timeText(_source.frameCount() / rate);

  const __FlashStringHelper *labels[InfoRowCount] = {
    F("Format"), F("Sample rate"), F("Channels"), F("Bitrate"), F("Duration")
  };
  const String values[InfoRowCount] = {formatText, rateText, channelText, bitrateText, durationText};
  for (uint8_t i = 0; i < InfoRowCount; i++) {
    const int16_t rowY = InfoTop + InfoRowsTop + InfoRowHeight * i + InfoRowHeight / 2;
    UiText::draw(tft, labels[i], left, rowY, InactiveColor, TFT_BLACK, UiText::Size::Compact,
                 ML_DATUM);
    UiText::draw(tft, UiText::ellipsize(values[i], textWidth - 96, UiText::Size::Content), right,
                 rowY, TFT_WHITE, TFT_BLACK, UiText::Size::Content, MR_DATUM);
  }
  UiText::draw(tft, F("Tap anywhere to close"), _display.width() / 2, InfoTop + InfoHeight - 18,
               InactiveColor, TFT_BLACK, UiText::Size::Compact, MC_DATUM);
}

String AudioPlayerScreen::trackTitle() const {
  if (!tagsPending() && _source.tags().title[0]) return String(_source.tags().title);
  // Untagged: the file name without its extension.
  int16_t dot = _fileName.lastIndexOf('.');
  return dot > 0 ? _fileName.substring(0, dot) : _fileName;
}

String AudioPlayerScreen::albumLine() const {
  if (tagsPending()) return String();
  const AudioTags::Info &info = _source.tags();
  return joinDot(String(info.album), info.year ? String(info.year) : String());
}

String AudioPlayerScreen::timeText(uint32_t seconds) const {
  char buffer[20];
  if (seconds >= 3600) {
    snprintf(buffer, sizeof(buffer), "%lu:%02lu:%02lu", (unsigned long)(seconds / 3600),
             (unsigned long)(seconds / 60 % 60), (unsigned long)(seconds % 60));
  } else {
    snprintf(buffer, sizeof(buffer), "%lu:%02lu", (unsigned long)(seconds / 60),
             (unsigned long)(seconds % 60));
  }
  return String(buffer);
}

bool AudioPlayerScreen::cellContains(int16_t centerX, int16_t x, int16_t y) const {
  return x >= centerX - CellHalf && x < centerX + CellHalf &&
         y >= TransportY - CellHalf && y < TransportY + CellHalf;
}

bool AudioPlayerScreen::playContains(int16_t x, int16_t y) const {
  const int16_t centerX = _display.width() / 2;
  return x >= centerX - PlayRadius && x < centerX + PlayRadius &&
         y >= TransportY - PlayRadius && y < TransportY + PlayRadius;
}

bool AudioPlayerScreen::seekContains(int16_t x, int16_t y) const {
  return x >= 0 && x < _display.width() && y >= SeekHitTop && y < SeekHitBottom;
}

bool AudioPlayerScreen::volumePanelContains(int16_t x, int16_t y) const {
  return x >= VolumePanelX && x < _display.width() - VolumePanelX &&
         y >= VolumePanelY && y < VolumePanelY + VolumePanelHeight;
}

bool AudioPlayerScreen::volumeSliderContains(int16_t x, int16_t y) const {
  return volumePanelContains(x, y) && x >= VolumeSliderHitLeft && x < VolumeSliderHitRight;
}
