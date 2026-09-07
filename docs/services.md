# Storage, audio, network, and device behavior

## Files and protected storage

Files keeps an LRU of up to eight directories during one Files-screen visit. Listings are enumerated once and sorted directories-first with case-insensitive natural A–Z sorting. Returning to a cached folder avoids a filesystem read. The cache survives a player or keyboard round trip and is discarded when Files closes or SD state changes.

The bottom pager has Previous, Page, and Next controls. Ordinary files and folders have a three-dot menu: Rename opens the keyboard with its complete name, and Delete requires a one-second hold. Folder deletion is recursive.

Holding the Home Files tile for one second opens a four-digit PIN input. The PIN selects the first byte of `/.configs/index`, a randomly generated 10,000-byte key file created only if absent. Names are hex-armored for FAT compatibility and names/source/cache bytes are XOR transformed. In normal folders, secret mode adds **Move to secret**; folder moves use a hidden staging directory before source removal. Non-directory entries inside `/.configs` expose Rename, hold-to-delete, and **Move out of secret** actions; moving out writes a fully decrypted plaintext copy to `/Documents` through a hidden staging directory, auto-renaming collisions as `name (1).ext`, then removes the encrypted source only after commit. Secret folders remain browse-only.

This is intentional lightweight obfuscation, not authenticated encryption or secure password protection. An incorrect PIN opens unusable names/content. `SecretStorage` never overwrites a malformed existing key, and it locks whenever the SD card unmounts.

## Wi-Fi and time

`WiFiController` runs station mode and exposes `Disabled`, `Disconnected`, `Scanning`, `Connecting`, `Connected`, and `Failed`. It scans, stores/sorts up to 12 networks, connects with a 15-second timeout, disconnects, forgets credentials, and reconnects to saved ESP32 Wi-Fi credentials when allowed. The Settings flow owns the UI; Home reflects connection state.

`TimeController` begins SNTP only while Wi-Fi is connected, marks time available after a valid epoch, and reports a fixed UTC+4 local clock. Change `UtcOffsetSeconds` if the product’s local-time requirement changes.

## Audio and workout

`MicController` captures INMP441 audio at 16 kHz on I2S port 0. Its optional physical power-switch sense line makes the recorder unavailable while the mic is off and stops a recording safely if power is removed. `SpeakerController` uses I2S port 1, starts muted, controls the amplifier enable pin, applies global volume, and keeps output buffering local to protect playback from filesystem/cache jitter.

Recording and speaker playback never run concurrently because they share I2S clock and word-select pins. Stop/mute the relevant component before repurposing the bus.

`AudioSettings` persists the speaker volume using ESP32 Preferences. `WorkoutScreen` has configurable work/rest periods in five-second steps from 5 to 600 seconds and plays audible stage cues using the shared speaker and volume setting.

## Touch, battery, and startup diagnostics

The XPT2046 is polled without IRQ. Calibration learns a 4×4 mesh and stores it under `/.system/touch`; before calibration, the controller uses a conservative noise floor. Its press/release debounce is 25/40 ms and normal event delivery happens only on state transitions.

The battery input expects a single Li-ion cell through a 100 kΩ / 100 kΩ divider. The controller measures every Home refresh and uses the median of exactly three ADC samples, with no cross-refresh smoothing or averaging. The displayed percentage only decreases during one controller runtime; runtime charging support is not available. Under Wi-Fi/display/speaker load the resting-cell curve can report a lower transient percentage.

At boot the serial log (115200 baud) prints reset cause, CPU frequency, flash bytes, and total/free/largest PSRAM block. A reported brownout reset indicates supply sag rather than an application exception.
