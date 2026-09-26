#include "AudioSettings.h"

#include <Preferences.h>

#include "SpeakerController.h"

namespace {
const char *PreferencesNamespace = "audio";
const char *VolumeKey = "volume";
}

AudioSettings::AudioSettings(SpeakerController &speaker)
  : _speaker(speaker),
    _volume(DefaultVolume) {
}

void AudioSettings::begin() {
  Preferences preferences;
  if (preferences.begin(PreferencesNamespace, false)) {
    uint8_t stored = preferences.getUChar(VolumeKey, DefaultVolume);
    if (stored <= MaxVolume) {
      _volume = stored;
    } else {
      _volume = DefaultVolume;
      preferences.putUChar(VolumeKey, _volume);
    }
    preferences.end();
  } else {
    _volume = DefaultVolume;
  }

  _speaker.setVolume(_volume);
}

uint8_t AudioSettings::volume() const {
  return _volume;
}

bool AudioSettings::setVolume(uint8_t volume) {
  if (volume > MaxVolume) {
    return false;
  }

  if (volume == _volume) {
    return true;
  }

  Preferences preferences;
  if (!preferences.begin(PreferencesNamespace, false)) {
    return false;
  }
  bool saved = preferences.putUChar(VolumeKey, volume) == sizeof(volume);
  preferences.end();
  if (!saved) return false;
  _volume = volume;
  _speaker.setVolume(_volume);
  return saved;
}
