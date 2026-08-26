#pragma once

#include <Arduino.h>
#include <FS.h>

// A small seekable storage wrapper.  Media decoders use this exactly like an
// fs::File; encryption stays here rather than leaking into decoder logic.
class StorageFile {
public:
  StorageFile();
  StorageFile(const fs::File &file, const uint8_t *key, uint16_t pin, bool encrypted);

  operator bool() { return (bool)_file; }
  bool operator!() { return !_file; }
  int read(uint8_t *buffer, size_t length);
  int read(void *buffer, size_t length) { return read(static_cast<uint8_t *>(buffer), length); }
  size_t write(const uint8_t *buffer, size_t length);
  size_t write(const void *buffer, size_t length) {
    return write(static_cast<const uint8_t *>(buffer), length);
  }
  bool seek(uint32_t position);
  size_t size() { return _file ? _file.size() : 0; }
  size_t position() { return _file ? _file.position() : 0; }
  uint32_t getLastWrite() { return _file ? _file.getLastWrite() : 0; }
  void flush() { if (_file) _file.flush(); }
  void close() { if (_file) _file.close(); }

private:
  fs::File _file;
  const uint8_t *_key;
  uint16_t _pin;
  bool _encrypted;
  void crypt(uint8_t *buffer, size_t length, uint32_t position) const;
};

class SecretStorage {
public:
  static const uint16_t KeyBytes = 10000;
  static const char *Directory;
  static const char *IndexPath;

  SecretStorage();

  // Creates the key only when it is absent. Existing malformed keys are never
  // overwritten, so a card problem cannot silently destroy secret data.
  bool unlock(uint16_t pin, String &error);
  void lock();
  bool unlocked() const { return _unlocked; }
  uint16_t pin() const { return _pin; }

  StorageFile open(const String &path, const char *mode, bool encrypted) const;
  bool exists(const String &path) const;
  bool remove(const String &path) const;
  bool rename(const String &from, const String &to) const;
  bool mkdir(const String &path) const;

  bool encryptName(const String &plain, String &encoded, String &error) const;
  bool decryptName(const String &encoded, String &plain) const;
  String displayName(const String &plain) const;
  bool isSecretPath(const String &path) const;

private:
  uint8_t _key[KeyBytes];
  uint16_t _pin;
  bool _unlocked;

  bool loadIndex(String &error);
  bool createIndex(String &error);
  static bool fromHex(char value, uint8_t &result);
};
