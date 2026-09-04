#include "SecretStorage.h"

#include <SD.h>
#include <esp_system.h>
#include <string.h>

#include "../../core/SpiBusLock.h"

const char *SecretStorage::Directory = "/.configs";
const char *SecretStorage::IndexPath = "/.configs/index";

namespace {
const char Hex[] = "0123456789abcdef";
const size_t ChunkBytes = 512;
}

StorageFile::StorageFile() : _file(), _key(nullptr), _pin(0), _encrypted(false) {}

StorageFile::StorageFile(const fs::File &file, const uint8_t *key, uint16_t pin, bool encrypted)
  : _file(file), _key(key), _pin(pin), _encrypted(encrypted && key != nullptr) {}

void StorageFile::crypt(uint8_t *buffer, size_t length, uint32_t position) const {
  if (!_encrypted || !_key) return;
  for (size_t i = 0; i < length; i++) buffer[i] ^= _key[(_pin + position + i) % SecretStorage::KeyBytes];
}

int StorageFile::read(uint8_t *buffer, size_t length) {
  if (!_file || !buffer || length == 0) return 0;
  const uint32_t start = _file.position();
  const int count = _file.read(buffer, length);
  if (count > 0) crypt(buffer, (size_t)count, start);
  return count;
}

size_t StorageFile::write(const uint8_t *buffer, size_t length) {
  if (!_file || !buffer || length == 0) return 0;
  const uint32_t start = _file.position();
  if (!_encrypted) return _file.write(buffer, length);
  uint8_t chunk[ChunkBytes];
  size_t offset = 0;
  while (offset < length) {
    const size_t amount = length - offset > sizeof(chunk) ? sizeof(chunk) : length - offset;
    memcpy(chunk, buffer + offset, amount);
    crypt(chunk, amount, start + offset);
    const size_t written = _file.write(chunk, amount);
    offset += written;
    if (written != amount) break;
  }
  return offset;
}

bool StorageFile::seek(uint32_t position) { return _file && _file.seek(position); }

SecretStorage::SecretStorage() : _key{}, _pin(0), _unlocked(false) {}

bool SecretStorage::unlock(uint16_t pin, String &error) {
  error = "";
  if (pin >= KeyBytes) { error = F("Invalid PIN"); return false; }
  if (!SD.exists(IndexPath)) {
    if (!createIndex(error)) return false;
  }
  if (!loadIndex(error)) return false;
  _pin = pin;
  _unlocked = true;
  return true;
}

void SecretStorage::lock() {
  memset(_key, 0, sizeof(_key));
  _pin = 0;
  _unlocked = false;
}

bool SecretStorage::loadIndex(String &error) {
  error = "";
  if (!SpiBusLock::lock(pdMS_TO_TICKS(100))) { error = F("Secret storage busy"); return false; }
  fs::File file = SD.open(IndexPath, FILE_READ);
  const bool valid = file && file.size() == KeyBytes && file.read(_key, KeyBytes) == KeyBytes;
  if (file) file.close();
  SpiBusLock::unlock();
  if (!valid) { memset(_key, 0, sizeof(_key)); error = F("Secret key is invalid"); }
  return valid;
}

bool SecretStorage::createIndex(String &error) {
  error = "";
  if (!SpiBusLock::lock(pdMS_TO_TICKS(100))) { error = F("Secret storage busy"); return false; }
  bool ready = (SD.exists(Directory) || SD.mkdir(Directory));
  const String temporary = String(IndexPath) + F(".tmp");
  if (ready) SD.remove(temporary.c_str());
  fs::File file = ready ? SD.open(temporary.c_str(), FILE_WRITE) : fs::File();
  uint8_t bytes[512];
  bool written = (bool)file;
  for (uint16_t offset = 0; written && offset < KeyBytes; offset += sizeof(bytes)) {
    const size_t amount = KeyBytes - offset > sizeof(bytes) ? sizeof(bytes) : KeyBytes - offset;
    esp_fill_random(bytes, amount);
    written = file.write(bytes, amount) == amount;
  }
  if (file) { file.flush(); file.close(); }
  if (written) written = SD.rename(temporary.c_str(), IndexPath);
  if (!written) SD.remove(temporary.c_str());
  SpiBusLock::unlock();
  if (!written) error = F("Cannot create secret key");
  return written;
}

StorageFile SecretStorage::open(const String &path, const char *mode, bool encrypted) const {
  fs::File file = SD.open(path.c_str(), mode);
  return StorageFile(file, _key, _pin, encrypted && _unlocked);
}

bool SecretStorage::exists(const String &path) const { return SD.exists(path.c_str()); }
bool SecretStorage::remove(const String &path) const { return SD.remove(path.c_str()); }
bool SecretStorage::rename(const String &from, const String &to) const { return SD.rename(from.c_str(), to.c_str()); }
bool SecretStorage::mkdir(const String &path) const { return SD.exists(path.c_str()) || SD.mkdir(path.c_str()); }

bool SecretStorage::encryptName(const String &plain, String &encoded, String &error) const {
  encoded = "";
  error = "";
  if (!_unlocked) { error = F("Secret storage is locked"); return false; }
  if (plain.length() == 0 || plain.length() > 127) { error = F("File name is too long for secret storage"); return false; }
  encoded.reserve(plain.length() * 2);
  for (uint16_t i = 0; i < plain.length(); i++) {
    const uint8_t value = (uint8_t)plain[i] ^ _key[(_pin + i) % KeyBytes];
    encoded += Hex[value >> 4];
    encoded += Hex[value & 0x0f];
  }
  return true;
}

bool SecretStorage::fromHex(char value, uint8_t &result) {
  if (value >= '0' && value <= '9') { result = value - '0'; return true; }
  if (value >= 'a' && value <= 'f') { result = value - 'a' + 10; return true; }
  if (value >= 'A' && value <= 'F') { result = value - 'A' + 10; return true; }
  return false;
}

bool SecretStorage::decryptName(const String &encoded, String &plain) const {
  plain = "";
  if (!_unlocked || encoded.length() == 0 || (encoded.length() & 1)) return false;
  plain.reserve(encoded.length() / 2);
  for (uint16_t i = 0; i < encoded.length(); i += 2) {
    uint8_t high = 0, low = 0;
    if (!fromHex(encoded[i], high) || !fromHex(encoded[i + 1], low)) return false;
    plain += (char)(((high << 4) | low) ^ _key[(_pin + i / 2) % KeyBytes]);
  }
  return true;
}

String SecretStorage::displayName(const String &plain) const {
  String display;
  display.reserve(plain.length());
  for (uint16_t i = 0; i < plain.length(); i++) {
    const uint8_t byte = (uint8_t)plain[i];
    display += byte >= 32 && byte <= 126 ? (char)byte : '?';
  }
  return display.length() ? display : String(F("?"));
}

bool SecretStorage::isSecretPath(const String &path) const {
  return path == Directory || path.startsWith(String(Directory) + "/");
}
