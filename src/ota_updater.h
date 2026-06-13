#ifndef OTA_UPDATER_H
#define OTA_UPDATER_H

#include <Arduino.h>
#include <FS.h>

// Forward declarations
class BoardDriver;
class HTTPClient;

// OTA update check result
struct OtaUpdateInfo {
  bool available;
  String version;
  String firmwareUrl;
  String webAssetsUrl;
};

class OtaUpdater {
 public:
  OtaUpdater(BoardDriver* boardDriver);

  OtaUpdateInfo checkForUpdate();

  bool applyFirmwareFromUrl(const String& url);

  bool applyWebAssetsFromUrl(const String& url);

  bool applyWebAssetsFromStream(Stream& stream, size_t contentLength);

  // Incremental TAR extraction for the manual web-assets upload. Unlike the
  // Stream variant (which buffers the whole tar to a temp file first — too big
  // for the 320 KB LittleFS partition), this consumes the async upload chunks
  // as they arrive and writes each entry straight to its final path, so only
  // the extracted files occupy the FS. Call begin once, feed every chunk, then
  // end.
  void beginWebStream();
  void feedWebStream(const uint8_t* data, size_t len);
  bool endWebStream(int* filesOut = nullptr);

  static const char* getCurrentVersion();

  // Apply web assets + firmware from an OtaUpdateInfo (used by autoUpdate and web UI apply).
  // Applies web assets first, then firmware. Firmware triggers reboot on success.
  void applyUpdate(const OtaUpdateInfo& info);

 private:
  BoardDriver* boardDriver;

  // Compare semantic versions. Returns true if remote is newer than current.
  static bool isNewerVersion(const String& current, const String& remote);

  // Configure an HTTPClient and perform a GET request. Returns true on HTTP 200.
  // On success, caller must call http.end(). On failure, http.end() is called internally.
  static bool beginHttpGet(HTTPClient& http, const String& url, int timeoutMs = 10000);

  // Read exactly 'length' bytes from a stream with timeout. Returns actual bytes read.
  static size_t readStreamBytes(Stream& stream, uint8_t* buffer, size_t length, unsigned long timeoutMs = 10000);

  // Skip the TAR data blocks for an entry of the given file size. Updates bytesProcessed.
  static bool skipTarBlocks(Stream& stream, size_t fileSize, size_t& bytesProcessed);

  // Recursively remove all LittleFS files except those under /games/
  static void removeWebAssets(const String& dirPath = "/");

  // --- Streaming TAR parser state (beginWebStream/feedWebStream/endWebStream).
  // The upload arrives as async chunks of arbitrary size that can split a
  // 512-byte TAR block across callbacks, so we reassemble blocks here and
  // process them one at a time, writing each file straight to LittleFS.
  void wsProcessBlock(const uint8_t* block);    // handle one full 512-byte block
  uint8_t m_wsBlock[512];     // partial-block accumulator
  size_t  m_wsFill = 0;       // bytes currently in m_wsBlock
  bool    m_wsActive = false; // begin() called, end() not yet
  bool    m_wsDone = false;   // end-of-archive (zero block) seen
  bool    m_wsInFile = false; // currently consuming a file's data blocks
  bool    m_wsSkip = false;   // current entry is skipped (dir / games / open failed)
  size_t  m_wsFileRemaining = 0;       // real file bytes still to write
  size_t  m_wsDataBlocksRemaining = 0; // 512-byte data blocks still to consume
  File    m_wsFile;           // current output file (when !m_wsSkip)
  int     m_wsFiles = 0;      // files written this session
};

#endif // OTA_UPDATER_H
