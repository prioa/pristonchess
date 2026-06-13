#include "ota_updater.h"
#include "board_driver.h"
#include "led_colors.h"
#include "version.h"
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <Update.h>
#include <WiFi.h>
#include "serial_tee.h"  // must be last: redefines Serial -> tee

// TAR header is always 512 bytes
static const size_t TAR_BLOCK_SIZE = 512;

OtaUpdater::OtaUpdater(BoardDriver* bd) : boardDriver(bd) {}

const char* OtaUpdater::getCurrentVersion() {
  return FIRMWARE_VERSION;
}

// ========== Private Helpers ==========

bool OtaUpdater::isNewerVersion(const String& current, const String& remote) {
  if (current == "dev") return true;

  int cMajor = 0, cMinor = 0, cPatch = 0;
  int rMajor = 0, rMinor = 0, rPatch = 0;
  sscanf(current.c_str(), "%d.%d.%d", &cMajor, &cMinor, &cPatch);
  sscanf(remote.c_str(), "%d.%d.%d", &rMajor, &rMinor, &rPatch);

  if (rMajor != cMajor) return rMajor > cMajor;
  if (rMinor != cMinor) return rMinor > cMinor;
  return rPatch > cPatch;
}

bool OtaUpdater::beginHttpGet(HTTPClient& http, const String& url, int timeoutMs) {
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(timeoutMs);
  http.setUserAgent("OpenChess/" FIRMWARE_VERSION);

  if (!http.begin(url)) {
    Serial.println("OTA: Failed to connect: " + url);
    return false;
  }

  int httpCode = http.GET();
  if (httpCode != 200) {
    Serial.printf("OTA: HTTP %d from: %s\n", httpCode, url.c_str());
    http.end();
    return false;
  }
  return true;
}

size_t OtaUpdater::readStreamBytes(Stream& stream, uint8_t* buffer, size_t length, unsigned long timeoutMs) {
  size_t bytesRead = 0;
  unsigned long lastDataTime = millis();
  while (bytesRead < length) {
    int avail = stream.available();
    if (avail > 0) {
      size_t toRead = min((size_t)avail, length - bytesRead);
      int r = stream.readBytes((char*)(buffer + bytesRead), toRead);
      if (r > 0) {
        bytesRead += r;
        lastDataTime = millis();
      }
    } else if (millis() - lastDataTime > timeoutMs) {
      break;
    } else {
      delay(1);
    }
  }
  return bytesRead;
}

bool OtaUpdater::skipTarBlocks(Stream& stream, size_t fileSize, size_t& bytesProcessed) {
  uint8_t skip[TAR_BLOCK_SIZE];
  size_t blocks = (fileSize + TAR_BLOCK_SIZE - 1) / TAR_BLOCK_SIZE;
  for (size_t b = 0; b < blocks; b++) {
    size_t r = readStreamBytes(stream, skip, TAR_BLOCK_SIZE);
    bytesProcessed += r;
    if (r < TAR_BLOCK_SIZE) return false;
  }
  return true;
}

void OtaUpdater::removeWebAssets(const String& dirPath) {
  File dir = LittleFS.open(dirPath);
  if (!dir || !dir.isDirectory()) return;

  File entry = dir.openNextFile();
  while (entry) {
    String name = entry.name();
    String fullPath = dirPath.endsWith("/") ? dirPath + name : dirPath + "/" + name;
    bool isDir = entry.isDirectory();
    entry.close();

    if (!fullPath.startsWith("/games") && fullPath != "/ota_temp.tar") {
      if (isDir) {
        removeWebAssets(fullPath);
        LittleFS.rmdir(fullPath);
      } else {
        LittleFS.remove(fullPath);
      }
    }

    entry = dir.openNextFile();
  }
  dir.close();
}

// ========== Public Methods ==========

OtaUpdateInfo OtaUpdater::checkForUpdate() {
  OtaUpdateInfo info = {false, "", "", ""};

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("OTA: No WiFi connection, skipping update check");
    return info;
  }

  HTTPClient http;
  if (!beginHttpGet(http, OTA_GITHUB_API_URL)) return info;

  String payload = http.getString();
  http.end();

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.printf("OTA: JSON parse error: %s\n", err.c_str());
    return info;
  }

  String tagName = doc["tag_name"] | "";
  if (tagName.startsWith("v")) tagName = tagName.substring(1);

  if (tagName.isEmpty()) {
    Serial.println("OTA: No tag found in release");
    return info;
  }

  info.version = tagName;
  if (!isNewerVersion(FIRMWARE_VERSION, tagName)) {
    Serial.printf("OTA: Up to date (v%s)\n", FIRMWARE_VERSION);
    return info;
  }
  info.available = true;
  Serial.printf("OTA: Update available: v%s (current: %s)\n", info.version.c_str(), FIRMWARE_VERSION);

  // Find firmware.bin and web_assets.tar in release assets
  JsonArray assets = doc["assets"];
  for (JsonObject asset : assets) {
    String name = asset["name"] | "";
    String url = asset["browser_download_url"] | "";
    if (name == "firmware.bin")
      info.firmwareUrl = url;
    else if (name == "web_assets.tar")
      info.webAssetsUrl = url;
  }

  return info;
}

bool OtaUpdater::applyFirmwareFromUrl(const String& url) {
  if (url.isEmpty()) {
    Serial.println("OTA: No firmware URL provided");
    return false;
  }

  Serial.println("OTA: Downloading firmware from: " + url);

  HTTPClient http;
  if (!beginHttpGet(http, url)) return false;

  int contentLength = http.getSize();
  if (contentLength <= 0) {
    Serial.println("OTA: Invalid firmware content length");
    http.end();
    return false;
  }

  WiFiClient* stream = http.getStreamPtr();

  Serial.printf("OTA: Starting firmware update (%d bytes)\n", contentLength);

  if (!Update.begin(contentLength, U_FLASH)) {
    Serial.printf("OTA: Not enough space for firmware update. Error: %s\n", Update.errorString());
    http.end();
    return false;
  }

  std::atomic<bool>* stopFlag = boardDriver->startWaitingAnimation();

  size_t written = Update.writeStream(*stream);

  if (stopFlag) stopFlag->store(true);

  if (written != contentLength) {
    Serial.printf("OTA: Firmware write failed. Written: %d/%d\n", written, contentLength);
    Update.abort();
    http.end();
    return false;
  }

  if (!Update.end(true)) {
    Serial.printf("OTA: Firmware finalize failed: %s\n", Update.errorString());
    http.end();
    return false;
  }

  Serial.println("OTA: Firmware update successful! Rebooting...");
  boardDriver->flashBoardAnimation(LedColors::Blue, 2);
  http.end();
  delay(1000);
  ESP.restart();
  return true;
}

bool OtaUpdater::applyWebAssetsFromUrl(const String& url) {
  if (url.isEmpty()) {
    Serial.println("OTA: No web assets URL provided");
    return false;
  }

  Serial.println("OTA: Downloading web assets from: " + url);

  HTTPClient http;
  if (!beginHttpGet(http, url)) return false;

  int contentLength = http.getSize();
  if (contentLength <= 0) {
    Serial.println("OTA: Invalid web assets content length");
    http.end();
    return false;
  }

  std::atomic<bool>* stopFlag = boardDriver->startWaitingAnimation();
  WiFiClient* stream = http.getStreamPtr();
  bool success = applyWebAssetsFromStream(*stream, contentLength);
  if (stopFlag) stopFlag->store(true);
  http.end();
  return success;
}

bool OtaUpdater::applyWebAssetsFromStream(Stream& stream, size_t totalSize) {
  Serial.printf("OTA: Starting web assets update (%d bytes)\n", totalSize);

  uint8_t header[TAR_BLOCK_SIZE];
  int filesWritten = 0;
  size_t bytesRead = 0;

  removeWebAssets("/");

  while (true) {
    // Read TAR header block (512 bytes)
    size_t headerRead = readStreamBytes(stream, header, TAR_BLOCK_SIZE);
    bytesRead += headerRead;
    if (headerRead < TAR_BLOCK_SIZE) break;

    // Check for end-of-archive (two zero blocks)
    bool allZero = true;
    for (size_t i = 0; i < TAR_BLOCK_SIZE; i++) {
      if (header[i] != 0) {
        allZero = false;
        break;
      }
    }
    if (allZero) break;

    // Parse filename from TAR header (first 100 bytes, null-terminated)
    char filename[101];
    memcpy(filename, header, 100);
    filename[100] = '\0';

    // Parse file size from TAR header (octal, bytes 124-135)
    char sizeStr[13];
    memcpy(sizeStr, header + 124, 12);
    sizeStr[12] = '\0';
    size_t fileSize = strtoul(sizeStr, nullptr, 8);

    // Parse type flag (byte 156): '0' or '\0' = regular file, '5' = directory
    char typeFlag = header[156];

    // Skip directories, symlinks, and zero-size entries
    if (typeFlag == '5' || typeFlag == '2' || fileSize == 0) {
      skipTarBlocks(stream, fileSize, bytesRead);
      continue;
    }

    // Build output path: prepend / for LittleFS
    String outPath = "/" + String(filename);
    while (outPath.endsWith("/")) outPath.remove(outPath.length() - 1);

    // Skip files in /games/ directory
    if (outPath.startsWith("/games/") || outPath == "/games") {
      skipTarBlocks(stream, fileSize, bytesRead);
      continue;
    }

    // Create parent directories (LittleFS.mkdir creates intermediate directories)
    int lastSlash = outPath.lastIndexOf('/');
    if (lastSlash > 0) {
      LittleFS.mkdir(outPath.substring(0, lastSlash));
    }

    Serial.printf("OTA: Extracting %s (%d bytes)\n", outPath.c_str(), fileSize);

    File outFile = LittleFS.open(outPath, "w");
    if (!outFile) {
      Serial.printf("OTA: Failed to create file: %s\n", outPath.c_str());
      skipTarBlocks(stream, fileSize, bytesRead);
      continue;
    }

    // Read file data (TAR entries are padded to 512-byte block boundaries)
    size_t remaining = fileSize;
    size_t blockAlignedSize = ((fileSize + TAR_BLOCK_SIZE - 1) / TAR_BLOCK_SIZE) * TAR_BLOCK_SIZE;
    size_t dataRead = 0;
    uint8_t buf[TAR_BLOCK_SIZE];

    while (dataRead < blockAlignedSize) {
      size_t toRead = min(sizeof(buf), blockAlignedSize - dataRead);
      size_t chunkRead = readStreamBytes(stream, buf, toRead);
      bytesRead += chunkRead;

      // Write only actual file data (not TAR block padding)
      size_t writeLen = min(chunkRead, remaining);
      if (writeLen > 0) {
        outFile.write(buf, writeLen);
        remaining -= writeLen;
      }

      dataRead += chunkRead;
      if (chunkRead < toRead) break; // Stream ended
    }

    outFile.close();
    filesWritten++;
  }

  Serial.printf("OTA: Web assets update complete. %d files extracted.\n", filesWritten);

  if (filesWritten > 0)
    boardDriver->flashBoardAnimation(LedColors::Cyan, 2);
  else
    boardDriver->flashBoardAnimation(LedColors::Red, 2);

  return filesWritten > 0;
}

// ========== Streaming web-assets upload ==========
// Same TAR semantics as applyWebAssetsFromStream(), but driven by async upload
// chunks instead of a seekable Stream. Each completed 512-byte block is handed
// to wsProcessBlock(); files are written straight to their final path, so peak
// FS usage is just the extracted files (no whole-tar temp copy) and the 282 KB
// asset set fits the 320 KB partition.

void OtaUpdater::beginWebStream() {
  Serial.println("OTA: Web assets stream starting");
  removeWebAssets("/");
  m_wsFill = 0;
  m_wsActive = true;
  m_wsDone = false;
  m_wsInFile = false;
  m_wsSkip = false;
  m_wsFileRemaining = 0;
  m_wsDataBlocksRemaining = 0;
  m_wsFiles = 0;
}

void OtaUpdater::wsProcessBlock(const uint8_t* block) {
  if (m_wsDone) return;  // past end-of-archive: ignore trailing zero/padding blocks

  if (m_wsInFile) {
    // A data (or trailing-padding) block for the current entry.
    size_t w = m_wsFileRemaining < TAR_BLOCK_SIZE ? m_wsFileRemaining : TAR_BLOCK_SIZE;
    if (!m_wsSkip && m_wsFile && w > 0)
      m_wsFile.write(block, w);
    m_wsFileRemaining -= w;
    m_wsDataBlocksRemaining--;
    if (m_wsDataBlocksRemaining == 0) {
      if (!m_wsSkip && m_wsFile) {
        m_wsFile.close();
        m_wsFiles++;
      }
      m_wsInFile = false;
      m_wsSkip = false;
    }
    return;
  }

  // Header block. End-of-archive is a fully zero block.
  bool allZero = true;
  for (size_t i = 0; i < TAR_BLOCK_SIZE; i++) {
    if (block[i] != 0) { allZero = false; break; }
  }
  if (allZero) { m_wsDone = true; return; }

  char filename[101];
  memcpy(filename, block, 100);
  filename[100] = '\0';

  char sizeStr[13];
  memcpy(sizeStr, block + 124, 12);
  sizeStr[12] = '\0';
  size_t fileSize = strtoul(sizeStr, nullptr, 8);

  char typeFlag = block[156];

  size_t dataBlocks = (fileSize + TAR_BLOCK_SIZE - 1) / TAR_BLOCK_SIZE;

  // Directories, symlinks and zero-byte entries carry no file data we keep.
  if (typeFlag == '5' || typeFlag == '2' || fileSize == 0) {
    if (dataBlocks > 0) {
      m_wsInFile = true;
      m_wsSkip = true;
      m_wsFileRemaining = fileSize;
      m_wsDataBlocksRemaining = dataBlocks;
    }
    return;
  }

  String outPath = "/" + String(filename);
  while (outPath.endsWith("/")) outPath.remove(outPath.length() - 1);

  // Preserve saved games — never overwrite /games/.
  bool skip = outPath.startsWith("/games/") || outPath == "/games";

  if (!skip) {
    int lastSlash = outPath.lastIndexOf('/');
    if (lastSlash > 0)
      LittleFS.mkdir(outPath.substring(0, lastSlash));
    m_wsFile = LittleFS.open(outPath, "w");
    if (!m_wsFile) {
      Serial.printf("OTA: Failed to create file: %s\n", outPath.c_str());
      skip = true;  // consume the data blocks but drop them
    } else {
      Serial.printf("OTA: Extracting %s (%u bytes)\n", outPath.c_str(), (unsigned)fileSize);
    }
  }

  m_wsInFile = true;
  m_wsSkip = skip;
  m_wsFileRemaining = fileSize;
  m_wsDataBlocksRemaining = dataBlocks;
}

void OtaUpdater::feedWebStream(const uint8_t* data, size_t len) {
  if (!m_wsActive) return;
  // Reassemble arbitrary-sized chunks into 512-byte TAR blocks.
  while (len > 0) {
    size_t need = TAR_BLOCK_SIZE - m_wsFill;
    size_t take = len < need ? len : need;
    memcpy(m_wsBlock + m_wsFill, data, take);
    m_wsFill += take;
    data += take;
    len -= take;
    if (m_wsFill == TAR_BLOCK_SIZE) {
      wsProcessBlock(m_wsBlock);
      m_wsFill = 0;
    }
  }
}

bool OtaUpdater::endWebStream(int* filesOut) {
  // Close any file left open by a truncated/misaligned archive.
  if (m_wsInFile && !m_wsSkip && m_wsFile) {
    m_wsFile.close();
    m_wsFiles++;
  }
  m_wsInFile = false;
  m_wsActive = false;
  m_wsFill = 0;

  Serial.printf("OTA: Web assets stream complete. %d files extracted.\n", m_wsFiles);
  if (filesOut) *filesOut = m_wsFiles;

  if (m_wsFiles > 0)
    boardDriver->flashBoardAnimation(LedColors::Cyan, 2);
  else
    boardDriver->flashBoardAnimation(LedColors::Red, 2);

  return m_wsFiles > 0;
}

void OtaUpdater::applyUpdate(const OtaUpdateInfo& info) {
  // Apply web assets first (doesn't require reboot)
  if (!info.webAssetsUrl.isEmpty()) {
    Serial.println("OTA: Updating web assets...");
    if (applyWebAssetsFromUrl(info.webAssetsUrl))
      Serial.println("OTA: Web assets updated successfully");
    else
      Serial.println("OTA: Web assets update failed");
  }

  // Apply firmware update (triggers reboot on success)
  if (!info.firmwareUrl.isEmpty()) {
    Serial.println("OTA: Updating firmware...");
    if (!applyFirmwareFromUrl(info.firmwareUrl))
      Serial.println("OTA: Firmware update failed");
  }
}