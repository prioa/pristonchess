#include "wifi_manager_esp32.h"
#include "chess_learning.h"
#include "elo_fetcher.h"
#include "openings_learn.h"
#include "profiles/profiles.h"

// Owned by main.cpp.
extern Profiles profiles;
#include "chess_lichess.h"
#include "chess_utils.h"
#include "chess_engine.h"
#include "move_history.h"
#include "version.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <Update.h>
#include <esp_wifi.h>
#include "serial_tee.h"  // must be last: redefines Serial -> tee

static const char* INITIAL_FEN = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";
// Samsung captive portal detection is more reliable when SoftAP is not in RFC1918 ranges.
static const IPAddress AP_IP(200, 200, 200, 1);
static const IPAddress AP_GATEWAY(200, 200, 200, 1);
static const IPAddress AP_SUBNET(255, 255, 255, 0);

WiFiManagerESP32::WiFiManagerESP32(BoardDriver* bd, MoveHistory* mh) : boardDriver(bd), moveHistory(mh), server(HTTP_PORT), gameMode("0"), lichessToken(""), botConfig(), scanAllChannels(false), profileCount(0), connectedProfileIndex(-1), scanResults(nullptr), scanResultCount(0), currentFen(INITIAL_FEN), hasPendingEdit(false), hasPendingResign(false), hasPendingDraw(false), pendingResignColor('?'), promotion{}, lastBoardPollTime(0), boardEvaluation(0.0f), otaUpdater(bd), autoOtaEnabled(false), otaChecked(false) {
  promotion.reset();
  pendingWiFi.reset();
}

void WiFiManagerESP32::begin() {
  Serial.println("=== Starting OpenChess WiFi Manager ===");

  if (ChessUtils::ensureNvsInitialized()) {
    // Load WiFi profiles
    loadProfiles();
    // Load Lichess token
    prefs.begin("lichess", false);
    if (prefs.isKey("token"))
      lichessToken = prefs.getString("token", "");
    prefs.end();
    if (lichessToken.length() > 0)
      Serial.println("Lichess API token loaded from NVS");
    // Load OTA auto-update preference
    prefs.begin("ota", false);
    autoOtaEnabled = prefs.getBool("autoUpdate", false);
    prefs.end();
  }

  // Signal that a device joined our AP — set the flag from the WiFi event task,
  // consumed on the main thread (loop) so the LED pulse runs in a safe context.
  WiFi.onEvent([this](arduino_event_id_t event, arduino_event_info_t info) {
    apClientConnected = true;
  }, ARDUINO_EVENT_WIFI_AP_STACONNECTED);

  bool connected = connectToSavedProfile();
  Serial.println("==== WiFi Connection Information ====");
  if (connected) {
    Serial.println("Connected to WiFi network:");
    Serial.println("- SSID: " + profiles[0].ssid);
    Serial.println("- Password: " + profiles[0].password);
    Serial.println("- Website: http://" MDNS_HOSTNAME ".local (" + WiFi.localIP().toString() + ")");
    // No boot marquee on success — when everything is OK the board boots
    // straight into the idle screen. The scrolling status text is reserved for
    // problems (AP fallback / AP error below, and FS/NVS errors in setup()).
  } else {
    bool apOk = startAPFallback();
    Serial.println("A WiFi Access Point was created:");
    Serial.println("- SSID: " AP_SSID);
    Serial.println("- Password: " AP_PASSWORD);
    Serial.println("- Website: http://" MDNS_HOSTNAME ".local (" + WiFi.softAPIP().toString() + ")");
    Serial.println("- MAC Address: " + WiFi.softAPmacAddress());
    Serial.println("Configure WiFi credentials from WebUI to join your WiFi network");
    if (boardDriver)
      boardDriver->showBootMessage(apOk ? "AP MODE" : "AP ERR",
                                   apOk ? LedColors::Yellow : LedColors::Red);
  }
  Serial.println("=====================================\n");

  if (autoOtaEnabled && lastUpdateInfo.available)
    otaUpdater.applyUpdate(lastUpdateInfo);

  auto sendCaptiveRedirect = [this](AsyncWebServerRequest* request) {
    AsyncWebServerResponse* response = request->beginResponse(302);
    response->addHeader("Location", "http://" + WiFi.softAPIP().toString() + "/");
    response->addHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    response->addHeader("Pragma", "no-cache");
    response->addHeader("Expires", "0");
    request->send(response);
  };
  // Captive portal detection endpoints
  // References: https://github.com/tonyp7/esp32-wifi-manager/issues/57 https://github.com/tripflex/captive-portal/blob/master/src/mgos_captive_portal.c#L369-L375
  // Android/Samsung
  server.on("/mobile/status.php", HTTP_GET, [sendCaptiveRedirect](AsyncWebServerRequest* request) { sendCaptiveRedirect(request); });
  server.on("/generate_204", HTTP_GET, [sendCaptiveRedirect](AsyncWebServerRequest* request) { sendCaptiveRedirect(request); });
  server.on("/gen_204", HTTP_GET, [sendCaptiveRedirect](AsyncWebServerRequest* request) { sendCaptiveRedirect(request); });
  // Windows
  server.on("/ncsi.txt", HTTP_GET, [sendCaptiveRedirect](AsyncWebServerRequest* request) { sendCaptiveRedirect(request); });
  // Firefox/OSX
  server.on("/success.txt", HTTP_GET, [sendCaptiveRedirect](AsyncWebServerRequest* request) { sendCaptiveRedirect(request); });
  // Apple
  server.on("/hotspot-detect.html", HTTP_GET, [sendCaptiveRedirect](AsyncWebServerRequest* request) { sendCaptiveRedirect(request); });
  // Set up OpenChess web server routes
  server.on("/board-update", HTTP_GET, [this](AsyncWebServerRequest* request) { request->send(200, "application/json", this->getBoardUpdateJSON()); });
  server.on("/board-update", HTTP_POST, [this](AsyncWebServerRequest* request) { this->handleBoardEditSuccess(request); });
  server.on("/sim-highlight", HTTP_POST, [this](AsyncWebServerRequest* request) { this->handleSimHighlight(request); });
  server.on("/highlight-colors", HTTP_POST, [this](AsyncWebServerRequest* request) { this->handleHighlightColors(request); });
  server.on("/promotion", HTTP_POST, [this](AsyncWebServerRequest* request) { this->handlePromotion(request); });
  server.on("/resign", HTTP_POST, [this](AsyncWebServerRequest* request) { this->handleResign(request); });
  server.on("/draw", HTTP_POST, [this](AsyncWebServerRequest* request) { this->handleDraw(request); });
  // Manual game end — winner=w / b / d. Used by board.html "Spiel beenden".
  server.on("/endgame", HTTP_POST, [this](AsyncWebServerRequest* request) {
    if (!request->hasArg("winner")) {
      request->send(400, "text/plain", "Missing winner");
      return;
    }
    String w = request->arg("winner");
    char c = w.length() ? w.charAt(0) : '?';
    if (c != 'w' && c != 'b' && c != 'd' && c != 'x') {
      request->send(400, "text/plain", "Invalid winner");
      return;
    }
    // 'x' = cancel/abort: end the game without specifying a winner or end-reason.
    pendingManualWinner = c;
    hasPendingManualEnd = true;
    Serial.printf("Manual end requested via web: winner=%c\n", c);
    request->send(200, "text/plain", "OK");
  });
  server.on("/wifi", HTTP_GET, [this](AsyncWebServerRequest* request) { request->send(200, "application/json", this->getWiFiInfoJSON()); });
  server.on("/wifi", HTTP_POST, [this](AsyncWebServerRequest* request) { this->handleConnectWiFi(request); });
  server.on("/gameselect", HTTP_POST, [this](AsyncWebServerRequest* request) { this->handleGameSelection(request); });

  // ----- Learning mode endpoints ---------------------------------------
  // GET /learn/list — opening catalog with per-level mastery (0–5).
  server.on("/learn/list", HTTP_GET, [](AsyncWebServerRequest* request) {
    JsonDocument doc;
    JsonArray arr = doc["openings"].to<JsonArray>();
    for (size_t i = 0; i < LEARN_OPENINGS_COUNT; i++) {
      const LearnOpening& op = LEARN_OPENINGS[i];
      JsonObject o = arr.add<JsonObject>();
      o["idx"]   = (int)i;
      o["id"]    = op.id;
      o["name"]  = op.name;
      o["eco"]   = op.eco;
      o["color"] = String(op.playerColor);
      o["type"]  = op.type;   // 0=opening, 1=finish, 2=tactic
      JsonObject plies = o["plies"].to<JsonObject>();
      plies["l1"] = op.pliesL1;
      plies["l2"] = op.pliesL2;
      plies["l3"] = op.pliesL3;
      JsonObject mastery = o["mastery"].to<JsonObject>();
      mastery["l1"] = ChessLearning::loadMastery(op.id, 1);
      mastery["l2"] = ChessLearning::loadMastery(op.id, 2);
      mastery["l3"] = ChessLearning::loadMastery(op.id, 3);
    }
    String out;
    serializeJson(doc, out);
    request->send(200, "application/json", out);
  });
  // POST /learn/restart — restart the current lesson from move 1.
  server.on("/learn/restart", HTTP_POST, [this](AsyncWebServerRequest* request) {
    if (activeLearning == nullptr) {
      request->send(400, "text/plain", "No active lesson");
      return;
    }
    activeLearning->requestRestart();
    request->send(200, "text/plain", "OK");
  });
  // POST /learn/reset — wipe all mastery counters (factory reset for the
  // learning progress). Useful from the learn.html "Fortschritt zurücksetzen".
  server.on("/learn/reset", HTTP_POST, [](AsyncWebServerRequest* request) {
    if (!ChessUtils::ensureNvsInitialized()) {
      request->send(500, "text/plain", "NVS init failed");
      return;
    }
    Preferences p;
    if (p.begin("learn", false)) {
      p.clear();
      p.end();
    }
    request->send(200, "text/plain", "OK");
  });

  // Player profile registry (Human-vs-Human stats).
  // Use ::profiles to disambiguate from WiFiManagerESP32::profiles (the
  // WiFi profile array which lives in the same class scope).
  server.on("/api/profiles", HTTP_GET, [](AsyncWebServerRequest* request) {
    request->send(200, "application/json", ::profiles.allJson());
  });
  // Force-refresh ELO on demand. Used by the players.html "Jetzt
  // aktualisieren" button. Blocking: fires a few HTTPS requests so can take
  // 1–2 seconds per profile.
  server.on("/api/profiles/refresh", HTTP_POST,
            [](AsyncWebServerRequest* request) {
    int e = EloFetcher::refreshAllProfiles();
    char buf[32];
    snprintf(buf, sizeof(buf), "{\"elo\":%d}", e);
    request->send(200, "application/json", buf);
  });
  server.on("/api/profiles", HTTP_POST, [](AsyncWebServerRequest* request) {
    // Reconstruct a JSON request body from x-www-form-urlencoded args so we
    // can reuse the existing Profiles::apply() dispatch. This matches the
    // pattern OpenChess uses for /gameselect etc.
    JsonDocument req;
    if (request->hasArg("action"))   req["action"]   = request->arg("action");
    if (request->hasArg("id"))       req["id"]       = request->arg("id");
    if (request->hasArg("name"))     req["name"]     = request->arg("name");
    if (request->hasArg("avatar"))   req["avatar"]   = request->arg("avatar");
    if (request->hasArg("color"))    req["color"]    = request->arg("color");
    if (request->hasArg("chesscom")) req["chesscom"] = request->arg("chesscom");
    String body;
    serializeJson(req, body);
    String result = ::profiles.apply(body);
    if (result.length() == 0) {
      request->send(400, "text/plain", "Invalid profile action");
    } else {
      request->send(200, "application/json", result);
    }
  });
  server.on("/lichess", HTTP_GET, [this](AsyncWebServerRequest* request) { request->send(200, "application/json", this->getLichessInfoJSON()); });
  server.on("/lichess", HTTP_POST, [this](AsyncWebServerRequest* request) { this->handleSaveLichessToken(request); });
  server.on("/board-settings", HTTP_GET, [this](AsyncWebServerRequest* request) { request->send(200, "application/json", this->getBoardSettingsJSON()); });
  server.on("/board-settings", HTTP_POST, [this](AsyncWebServerRequest* request) { this->handleBoardSettings(request); });
  server.on("/paint", HTTP_GET, [this](AsyncWebServerRequest* request) {
    int slot = request->hasArg("slot") ? request->arg("slot").toInt() : boardDriver->getActivePaintSlot();
    if (slot < 0 || slot >= BoardDriver::paintSlotCount()) slot = boardDriver->getActivePaintSlot();
    String out; out.reserve(NUM_ROWS * NUM_COLS * 6);
    char hex[7];
    for (int r = 0; r < NUM_ROWS; r++)
      for (int c = 0; c < NUM_COLS; c++) {
        LedRGB p = boardDriver->getPaintPixel(slot, r, c);
        snprintf(hex, sizeof(hex), "%02x%02x%02x", p.r, p.g, p.b);
        out += hex;
      }
    request->send(200, "text/plain", out);
  });
  server.on("/paint", HTTP_POST, [this](AsyncWebServerRequest* request) {
    if (!request->hasArg("grid")) { request->send(400, "text/plain", "missing grid"); return; }
    String g = request->arg("grid");
    if (g.length() < (size_t)(NUM_ROWS * NUM_COLS * 6)) { request->send(400, "text/plain", "bad grid"); return; }
    int slot = request->hasArg("slot") ? request->arg("slot").toInt() : boardDriver->getActivePaintSlot();
    if (slot < 0 || slot >= BoardDriver::paintSlotCount()) slot = boardDriver->getActivePaintSlot();
    auto hx = [](char ch) -> int { if (ch >= '0' && ch <= '9') return ch - '0'; if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10; if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10; return 0; };
    for (int i = 0; i < NUM_ROWS * NUM_COLS; i++) {
      int o = i * 6;
      LedRGB col{ (uint8_t)(hx(g[o]) * 16 + hx(g[o+1])), (uint8_t)(hx(g[o+2]) * 16 + hx(g[o+3])), (uint8_t)(hx(g[o+4]) * 16 + hx(g[o+5])) };
      boardDriver->setPaintPixel(slot, i / NUM_COLS, i % NUM_COLS, col);
    }
    boardDriver->savePaintSlot(slot);
    request->send(200, "text/plain", "OK");
  });
  server.on("/paint/select", HTTP_POST, [this](AsyncWebServerRequest* request) {
    if (!request->hasArg("slot")) { request->send(400, "text/plain", "missing slot"); return; }
    int slot = request->arg("slot").toInt();
    if (slot < 0 || slot >= BoardDriver::paintSlotCount()) { request->send(400, "text/plain", "bad slot"); return; }
    boardDriver->setActivePaintSlot((uint8_t)slot);
    boardDriver->savePaintMeta();
    request->send(200, "text/plain", "OK");
  });
  server.on("/paint/name", HTTP_POST, [this](AsyncWebServerRequest* request) {
    if (!request->hasArg("slot") || !request->hasArg("name")) { request->send(400, "text/plain", "missing"); return; }
    int slot = request->arg("slot").toInt();
    if (slot < 0 || slot >= BoardDriver::paintSlotCount()) { request->send(400, "text/plain", "bad slot"); return; }
    boardDriver->setPaintName(slot, request->arg("name").c_str());
    boardDriver->savePaintMeta();
    request->send(200, "text/plain", "OK");
  });
  server.on("/paint/list", HTTP_GET, [this](AsyncWebServerRequest* request) {
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    int active = boardDriver->getActivePaintSlot();
    for (int i = 0; i < BoardDriver::paintSlotCount(); i++) {
      JsonObject o = arr.add<JsonObject>();
      o["slot"] = i;
      o["name"] = boardDriver->getPaintName(i);
      o["active"] = (i == active);
    }
    String out; serializeJson(doc, out);
    request->send(200, "application/json", out);
  });
  server.on("/audio", HTTP_POST, [this](AsyncWebServerRequest* request) {
    if (!request->hasArg("bands")) { request->send(400, "text/plain", "missing bands"); return; }
    String b = request->arg("bands");
    auto hx = [](char ch) -> int { if (ch >= '0' && ch <= '9') return ch - '0'; if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10; if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10; return 0; };
    uint8_t vals[NUM_COLS] = {0};
    int n = (int)(b.length() / 2); if (n > NUM_COLS) n = NUM_COLS;
    for (int i = 0; i < n; i++) vals[i] = (uint8_t)(hx(b[i * 2]) * 16 + hx(b[i * 2 + 1]));
    boardDriver->setAudioBands(vals, n);
    request->send(200, "text/plain", "OK");
  });
  server.on("/game-input", HTTP_POST, [this](AsyncWebServerRequest* request) {
    if (!request->hasArg("dir")) { request->send(400, "text/plain", "missing dir"); return; }
    boardDriver->setGameInput(request->arg("dir").toInt());
    request->send(200, "text/plain", "OK");
  });
  server.on("/board-wave", HTTP_POST, [this](AsyncWebServerRequest* request) {
    // Trigger the TV-style mode-change transition (outside-in off, inside-out on).
    boardDriver->modeChangeTransition();
    request->send(200, "text/plain", "OK");
  });
  server.on("/board-calibrate", HTTP_POST, [this](AsyncWebServerRequest* request) { this->handleBoardCalibration(request); });
  server.on("/games", HTTP_GET, [this](AsyncWebServerRequest* request) { this->handleGamesRequest(request); });
  server.on("/games", HTTP_DELETE, [this](AsyncWebServerRequest* request) { this->handleDeleteGame(request); });
  server.on("/hardware-config", HTTP_GET, [this](AsyncWebServerRequest* request) { this->getHardwareConfigJSON(request); });
  server.on("/hardware-config", HTTP_POST, [this](AsyncWebServerRequest* request) { this->handleHardwareConfig(request); });
  server.on("/debug-state", HTTP_GET, [this](AsyncWebServerRequest* request) { this->handleDebugState(request); });
  server.on("/debug-testleds", HTTP_POST, [this](AsyncWebServerRequest* request) { this->handleDebugTestLeds(request); });
  server.on("/debug-hold-col", HTTP_POST, [this](AsyncWebServerRequest* request) {
    int col = request->hasParam("col", true) ? request->getParam("col", true)->value().toInt() : -1;
    boardDriver->debugHoldCol(col);
    request->send(200, "text/plain", col >= 0 ? ("Halte Spalte " + String(col)) : "Scan normal");
  });
  server.on("/debug-clearleds", HTTP_POST, [this](AsyncWebServerRequest* request) {
    boardDriver->acquireLEDs();
    boardDriver->clearAllLEDs(true);
    boardDriver->releaseLEDs();
    request->send(200, "text/plain", "OK");
  });
  // === Diagnostic test endpoints ===
  server.on("/debug/pullup", HTTP_GET, [this](AsyncWebServerRequest* request) {
    uint8_t m = boardDriver->pullupSanityCheck();
    JsonDocument d;
    d["mask"] = m;
    JsonArray rows = d["rows"].to<JsonArray>();
    for (int r = 0; r < 8; r++) rows.add((m >> r) & 1 ? "HIGH" : "LOW");
    String out; serializeJson(d, out);
    request->send(200, "application/json", out);
  });
  server.on("/debug/sr-invert", HTTP_GET, [this](AsyncWebServerRequest* request) {
    JsonDocument d; d["srInvertOutputs"] = boardDriver->getSrInvert();
    String out; serializeJson(d, out);
    request->send(200, "application/json", out);
  });
  server.on("/debug/sr-invert", HTTP_POST, [this](AsyncWebServerRequest* request) {
    bool v = boardDriver->toggleSrInvert();
    JsonDocument d; d["srInvertOutputs"] = v;
    String out; serializeJson(d, out);
    request->send(200, "application/json", out);
  });
  server.on("/debug/sr-pattern", HTTP_POST, [this](AsyncWebServerRequest* request) {
    uint8_t p = (uint8_t)(request->hasParam("p", true) ? strtol(request->getParam("p", true)->value().c_str(), nullptr, 0) : 0);
    bool hold = request->hasParam("hold", true) ? request->getParam("hold", true)->value() == "1" : true;
    boardDriver->debugSrPattern(p, hold);
    request->send(200, "text/plain", hold ? "held" : "released");
  });
  server.on("/debug/classify", HTTP_POST, [this](AsyncWebServerRequest* request) {
    uint32_t ms = request->hasParam("ms", true) ? (uint32_t)request->getParam("ms", true)->value().toInt() : 8000;
    if (ms > 30000) ms = 30000;
    uint8_t grid[8][8];
    boardDriver->classifyPins(ms, grid);
    JsonDocument d;
    JsonArray g = d["grid"].to<JsonArray>();
    for (int r = 0; r < 8; r++) {
      JsonArray row = g.add<JsonArray>();
      for (int c = 0; c < 8; c++) row.add(grid[r][c]);
    }
    String out; serializeJson(d, out);
    request->send(200, "application/json", out);
  });
  // Serial-log tail: returns log bytes newer than ?after=<seq> plus the new
  // sequence number, so the web monitor can poll for just the delta. The tee
  // (serial_tee.h) mirrors every Serial.print* into the ring buffer this reads.
  server.on("/debug/log", HTTP_GET, [](AsyncWebServerRequest* request) {
    uint32_t after = request->hasParam("after")
                         ? (uint32_t)strtoul(request->getParam("after")->value().c_str(), nullptr, 10)
                         : 0;
    // Single-threaded AsyncTCP task → a function-local static buffer is safe
    // and keeps 2 KB off the limited async-task stack.
    static char logbuf[2048];
    size_t n = 0;
    uint32_t newSeq = serialLogFetch(after, logbuf, sizeof(logbuf), &n);
    JsonDocument d;
    d["seq"] = newSeq;
    d["data"] = (const char*)logbuf;
    String out; serializeJson(d, out);
    request->send(200, "application/json", out);
  });
  server.on("/debug/phantom-start", HTTP_POST, [this](AsyncWebServerRequest* request) {
    boardDriver->phantomLogStart();
    request->send(200, "text/plain", "armed (30s)");
  });
  server.on("/debug/phantom-fetch", HTTP_GET, [this](AsyncWebServerRequest* request) {
    BoardDriver::PhantomEntry entries[64];
    size_t n = boardDriver->phantomLogFetch(entries, 64);
    JsonDocument d;
    JsonArray arr = d["events"].to<JsonArray>();
    for (size_t i = 0; i < n; i++) {
      JsonObject o = arr.add<JsonObject>();
      o["ms"] = entries[i].ms;
      o["row"] = entries[i].row;
      o["col"] = entries[i].col;
      o["low"] = entries[i].low;
    }
    String out; serializeJson(d, out);
    request->send(200, "application/json", out);
  });
  server.on("/debug/led-walk", HTTP_POST, [this](AsyncWebServerRequest* request) {
    request->send(200, "text/plain", "running");
    boardDriver->ledWalkTest();
  });
  server.on("/debug/led-rgb", HTTP_POST, [this](AsyncWebServerRequest* request) {
    request->send(200, "text/plain", "running");
    boardDriver->ledRgbSequenceTest();
  });
  // Reset the LED pixel-index map to the default straight wiring (persisted).
  // Fixes a wrong/offset LED highlight caused by a bad sensor-driven calibration.
  server.on("/debug/led-map-reset", HTTP_POST, [this](AsyncWebServerRequest* request) {
    boardDriver->resetLedMapToDefault();
    request->send(200, "text/plain", "LED map reset to default straight wiring");
  });
  // Rematch after a Human-vs-Human game: start a fresh game with the SAME two
  // players but colours swapped, reusing the same time control. The main loop
  // picks up gameMode just like a normal /gameselect (even from the post-game
  // hold), so the new game starts right away.
  server.on("/rematch", HTTP_POST, [this](AsyncWebServerRequest* request) {
    if (hvhWhiteId.length() == 0 && hvhBlackId.length() == 0) {
      request->send(409, "text/plain", "no previous HvH players");
      return;
    }
    String tmp = hvhWhiteId; hvhWhiteId = hvhBlackId; hvhBlackId = tmp;  // swap sides
    gameMode = "1";
    Serial.printf("Rematch (colours swapped): white=%s black=%s time=%lums\n",
                  hvhWhiteId.c_str(), hvhBlackId.c_str(), (unsigned long)hvhTimeLimitMs);
    request->send(200, "text/plain", "rematch");
  });
  // Live preview of the pickup-reveal effect with the current settings (for the
  // settings page — play it on the idle board while sliders are being adjusted).
  server.on("/board-preview", HTTP_GET, [this](AsyncWebServerRequest* request) {
    boardDriver->requestRevealPreview();
    request->send(200, "text/plain", "preview");
  });
  server.on("/board-preview-shiny", HTTP_GET, [this](AsyncWebServerRequest* request) {
    bool on = true;   // default: turn it on; pass ?on=0 to switch off
    if (request->hasParam("on")) on = request->getParam("on")->value() != "0";
    boardDriver->setShinyPreview(on);
    request->send(200, "text/plain", on ? "on" : "off");
  });
  server.on("/board-preview-loop", HTTP_GET, [this](AsyncWebServerRequest* request) {
    bool on = true;   // endless pickup-reveal preview toggle; ?on=0 stops it
    if (request->hasParam("on")) on = request->getParam("on")->value() != "0";
    boardDriver->setRevealLoop(on);
    request->send(200, "text/plain", on ? "on" : "off");
  });
  // LED-map recovery tooling (no sensors): light one RAW strip pixel to read off
  // the wiring by eye, then push the reconstructed map.
  server.on("/debug/led-raw", HTTP_GET, [this](AsyncWebServerRequest* request) {
    int i = request->hasParam("i") ? request->getParam("i")->value().toInt() : -2;
    boardDriver->setDiagRawPixel(i);
    request->send(200, "text/plain", String("diag raw pixel = ") + i);
  });
  server.on("/debug/led-map-exit", HTTP_GET, [this](AsyncWebServerRequest* request) {
    boardDriver->setDiagRawPixel(-2);
    request->send(200, "text/plain", "diag off (idle animation resumed)");
  });
  // Brightness headroom test: GET /debug/brightness-test?lum=N (0..255) shows
  // all-white at luminance N (bypasses the safety cap). lum<0 or omitted = off.
  server.on("/debug/brightness-test", HTTP_GET, [this](AsyncWebServerRequest* request) {
    int lum = request->hasParam("lum") ? request->getParam("lum")->value().toInt() : -1;
    if (lum > 255) lum = 255;
    boardDriver->setBrightnessTest(lum);
    request->send(200, "text/plain", lum >= 0
                  ? (String("all-white @ luminance ") + lum + " (~" + (lum * 100 / 255) + "%) — Spannung am Strip-Ende messen")
                  : "brightness test off");
  });
  // Body = 64 comma/space-separated pixel indices (row-major a8..h8, a7.., a1..h1).
  server.on(
      "/debug/led-map-set", HTTP_POST,
      [](AsyncWebServerRequest* request) {},
      NULL,
      [this](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
        static String body;
        if (index == 0) body = "";
        for (size_t i = 0; i < len; i++) body += (char)data[i];
        if (index + len == total) {
          uint8_t flat[LED_COUNT];
          int count = 0, val = 0;
          bool have = false;
          for (size_t i = 0; i <= body.length(); i++) {
            char c = (i < body.length()) ? body[i] : ',';
            if (c >= '0' && c <= '9') { val = val * 10 + (c - '0'); have = true; }
            else if (have) { if (count < LED_COUNT) flat[count] = (uint8_t)val; count++; val = 0; have = false; }
          }
          if (count == LED_COUNT && boardDriver->setLedMap(flat, LED_COUNT)) {
            boardDriver->setDiagRawPixel(-2);
            request->send(200, "text/plain", "LED map updated and persisted");
          } else {
            request->send(400, "text/plain", String("Invalid map: count=") + count + " (need exactly 64 unique values 0..63)");
          }
        }
      });
  // OTA update endpoints
  server.on("/ota/status", HTTP_GET, [this](AsyncWebServerRequest* request) { this->handleOtaStatus(request); });
  server.on("/ota/settings", HTTP_POST, [this](AsyncWebServerRequest* request) { this->handleOtaSettings(request); });
  server.on("/ota/apply", HTTP_POST, [this](AsyncWebServerRequest* request) { this->handleOtaApply(request); });
  // OTA manual upload endpoints - JS sends raw binary body (application/octet-stream), so only the body handler (3rd callback) fires; the multipart file handler (2nd) is unused.
  server.on(
      "/ota/upload/firmware", HTTP_POST,
      [](AsyncWebServerRequest* request) {},
      NULL,
      [this](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) { this->onFirmwareUploadBody(request, data, len, index, total); });
  server.on(
      "/ota/upload/web", HTTP_POST,
      [](AsyncWebServerRequest* request) {},
      NULL,
      [this](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) { this->onWebAssetsUploadBody(request, data, len, index, total); });
  // Serve sound files directly (no gzip variant exists, avoids .gz probe errors)
  server.serveStatic("/sounds/", LittleFS, "/sounds/").setTryGzipFirst(false);
  // Serve piece SVGs with aggressive caching, otherwise chrome doesn't actually use the cached versions
  server.serveStatic("/pieces/", LittleFS, "/pieces/").setCacheControl("max-age=31536000, immutable");
  // CSS/JS get a cache lifetime so the service worker's background revalidation
  // is served from the browser HTTP cache instead of re-downloading from LittleFS
  // on every page load. Hard reload (dev) still bypasses this and fetches fresh.
  server.serveStatic("/css/", LittleFS, "/css/").setCacheControl("max-age=86400");
  server.serveStatic("/scripts/", LittleFS, "/scripts/").setCacheControl("max-age=86400");
  // Serve all other static files from LittleFS (gzip handled automatically)
  // SSE for live board updates — MUST be registered before the static catch-all,
  // otherwise serveStatic("/") swallows /events with a 404.
  server.addHandler(&boardEvents);
  server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");
  server.onNotFound([](AsyncWebServerRequest* request) { request->send(404, "text/plain", "Not Found"); });

  server.begin();
  Serial.println("Web server started on port: " + String(HTTP_PORT));

  xTaskCreate(pendingWiFiBackgroundTask, "WiFi_Pending_Task", 8192, this, 4, &pendingWiFiTaskHandle);
}

String WiFiManagerESP32::getBoardUpdateJSON() {
  this->lastBoardPollTime = millis();
  JsonDocument doc;
  doc["fen"] = currentFen;
  {
    // Threat bitboard: bit (row*8+col) set if the side-to-move's square is attacked.
    // Cheap and stateless — only depends on the parsed board + the attacker colour.
    char tboard[8][8]; char tturn = 'w';
    ChessEngine te;
    ChessUtils::fenToBoard(currentFen, tboard, tturn, &te);
    uint64_t threats = 0;
    for (int r = 0; r < 8; r++)
      for (int c = 0; c < 8; c++)
        if (te.isSquareUnderAttack(tboard, r, c, tturn)) threats |= ((uint64_t)1 << (r * 8 + c));
    char hex[17];
    snprintf(hex, sizeof(hex), "%016llx", (unsigned long long)threats);
    doc["threats"] = hex;
  }
  doc["evaluation"] = serialized(String(boardEvaluation, 2));
  // Active game mode (0 = no game running). board.html uses this to show
  // a "Kein Spiel läuft" banner instead of an empty live board. The
  // gameMode string is just a transient selection buffer — main.cpp
  // mirrors the actually active mode via setActiveGameMode().
  doc["mode"] = activeGameMode;
  doc["simManual"] = simManualMode;

  // Transient physical sound events (pickup / wrong-turn pickup). The client
  // plays each new sndSeq once. char 0 -> empty string.
  doc["sndEvent"] = soundEvent ? String((char)soundEvent) : String("");
  doc["sndPiece"] = soundPiece ? String((char)soundPiece) : String("");
  doc["sndSeq"]   = soundEventSeq;

  // Current-game metadata so board.html can render a header like
  // "HvH: Benny vs Rene" or "Bot: Schwer, du als Weiss" without polling
  // separate endpoints.
  switch (activeGameMode) {
    case 1: {  // Human vs Human
      doc["modeName"] = "Mensch vs Mensch";
      if (hvhWhiteId.length() > 0)
        doc["whiteName"] = ::profiles.nameForId(hvhWhiteId.c_str());
      if (hvhBlackId.length() > 0)
        doc["blackName"] = ::profiles.nameForId(hvhBlackId.c_str());
      break;
    }
    case 2: {  // Chess Bot
      doc["modeName"] = "Bot";
      doc["playerIsWhite"] = botConfig.playerIsWhite;
      int depth = botConfig.stockfishSettings.depth;
      const char* diff =
          (depth <= 4)  ? "Einfach" :
          (depth <= 10) ? "Mittel"  :
          (depth <= 16) ? "Schwer"  : "Experte";
      doc["difficulty"] = diff;
      doc["depth"] = depth;
      break;
    }
    case 3: doc["modeName"] = "Lichess";        break;
    case 4: doc["modeName"] = "Sensor-Test";    break;
    case 5: doc["modeName"] = "Simulation";     break;
    case 6: {  // Learning
      doc["modeName"] = "Lernen";
      if (activeLearning) {
        JsonObject lesson = doc["lesson"].to<JsonObject>();
        lesson["openingId"]   = activeLearning->getOpeningId();
        lesson["openingName"] = activeLearning->getOpeningDisplayName();
        lesson["level"]       = activeLearning->getLevel();
        lesson["playerColor"] = String(activeLearning->getPlayerColor());
        lesson["plyIndex"]    = activeLearning->getCurrentPlie();
        lesson["totalPlies"]  = activeLearning->getTotalPlies();
        lesson["mistakes"]    = activeLearning->getMistakes();
        lesson["completed"]   = activeLearning->isLessonCompleted();
        lesson["type"]        = activeLearning->getLessonType();
        // Whose turn the upcoming move belongs to (so the UI can show
        // "dein Zug" vs "Gegenzug").
        char moveColor = (activeLearning->getCurrentPlie() % 2 == 0) ? 'w' : 'b';
        lesson["nextMoveColor"] = String(moveColor);
        const auto* tm = activeLearning->getNextTargetMove();
        if (tm) {
          char uci[6];
          snprintf(uci, sizeof(uci), "%c%d%c%d",
                   (char)('a' + tm->fromCol), 8 - tm->fromRow,
                   (char)('a' + tm->toCol),   8 - tm->toRow);
          lesson["nextMove"] = String(uci);
        }
      }
      break;
    }
    default: break;
  }

  if (promotion.pending) {
    JsonObject promo = doc["promotion"].to<JsonObject>();
    promo["color"] = String(promotion.color);
  }

  // Game status (set by main.cpp every loop)
  doc["turn"]       = String(_gameStatus.turn);
  doc["gameOver"]   = _gameStatus.gameOver;
  doc["winner"]     = String(_gameStatus.winnerColor);
  doc["endReason"]  = String(_gameStatus.endReason);
  doc["inCheck"]    = _gameStatus.inCheck;
  if (_gameStatus.opening[0]) doc["opening"] = _gameStatus.opening;

  JsonObject clk    = doc["clock"].to<JsonObject>();
  clk["whiteMs"]    = _gameStatus.clockWhiteMs;
  clk["blackMs"]    = _gameStatus.clockBlackMs;
  clk["limitMs"]    = _gameStatus.clockLimitMs;
  clk["ticking"]    = _gameStatus.clockTicking;
  clk["wFlag"]      = _gameStatus.whiteFlagged;
  clk["bFlag"]      = _gameStatus.blackFlagged;

  String output;
  serializeJson(doc, output);
  return output;
}

String WiFiManagerESP32::getWiFiInfoJSON() {
  bool connected = (WiFi.status() == WL_CONNECTED);
  JsonDocument doc;
  doc["connected"] = connected;
  doc["scanAllChannels"] = scanAllChannels;
  doc["connectedIndex"] = connectedProfileIndex;

  // Saved networks (never include passwords)
  JsonArray saved = doc["saved"].to<JsonArray>();
  for (int i = 0; i < profileCount; i++) {
    JsonObject net = saved.add<JsonObject>();
    net["ssid"] = profiles[i].ssid;
    net["channel"] = profiles[i].channel;

    bool found = false;
    // Add RSSI from scan results if available
    for (int j = 0; j < scanResultCount; j++) {
      if (scanResults[j].ssid == profiles[i].ssid) {
        net["rssi"] = scanResults[j].rssi;
        net["enc"] = scanResults[j].encryptionType;
        found = true;
        break;
      }
    }
    // For the connected profile, use live WiFi info if scan didn't cover it
    wifi_ap_record_t apInfo;
    if (!found && connected && i == connectedProfileIndex && esp_wifi_sta_get_ap_info(&apInfo) == ESP_OK) {
      net["rssi"] = apInfo.rssi;
      net["enc"] = (uint8_t)apInfo.authmode;
    }
  }

  // Scanned networks (filter out already-saved SSIDs)
  JsonArray scanned = doc["scanned"].to<JsonArray>();
  for (int i = 0; i < scanResultCount; i++) {
    bool alreadySaved = false;
    for (int j = 0; j < profileCount; j++) {
      if (scanResults[i].ssid == profiles[j].ssid) {
        alreadySaved = true;
        break;
      }
    }
    if (alreadySaved || scanResults[i].ssid.length() == 0) continue;
    // Deduplicate: skip if this SSID was already added (multiple APs with same SSID)
    bool duplicate = false;
    for (int k = 0; k < i; k++) {
      if (scanResults[k].ssid == scanResults[i].ssid) {
        duplicate = true;
        break;
      }
    }
    if (duplicate) continue;

    JsonObject net = scanned.add<JsonObject>();
    net["ssid"] = scanResults[i].ssid;
    net["rssi"] = scanResults[i].rssi;
    net["channel"] = scanResults[i].channel;
    net["enc"] = scanResults[i].encryptionType;
  }

  String output;
  serializeJson(doc, output);
  return output;
}

void WiFiManagerESP32::handleBoardEditSuccess(AsyncWebServerRequest* request) {
  if (request->hasArg("fen")) {
    pendingFenEdit = request->arg("fen");
    hasPendingEdit = true;
    Serial.println("Board edit received (FEN): " + pendingFenEdit);
    request->send(200, "text/plain", "OK");
  } else {
    Serial.println("Board edit failed: no FEN parameter");
    request->send(400, "text/plain", "Missing FEN parameter");
  }
}

void WiFiManagerESP32::handleSimHighlight(AsyncWebServerRequest* request) {
  pendingHlFrom       = request->hasArg("from") ? request->arg("from") : String("");
  pendingHlTargets    = request->hasArg("to")   ? request->arg("to")   : String("");
  hasPendingHighlight = true;
  request->send(200, "text/plain", "OK");
}

static LedRGB pcParseHex(const String& s, LedRGB fallback) {
  String h = s;
  if (h.startsWith("#")) h = h.substring(1);
  if (h.length() != 6) return fallback;
  long v = strtol(h.c_str(), nullptr, 16);
  return { (uint8_t)((v >> 16) & 0xFF), (uint8_t)((v >> 8) & 0xFF), (uint8_t)(v & 0xFF) };
}

void WiFiManagerESP32::handleHighlightColors(AsyncWebServerRequest* request) {
  // Missing args keep the current value (partial updates allowed).
  LedRGB s = pcParseHex(request->arg("source"),  boardDriver->hlSourceColor());
  LedRGB t = pcParseHex(request->arg("target"),  boardDriver->hlTargetColor());
  LedRGB c = pcParseHex(request->arg("capture"), boardDriver->hlCaptureColor());
  LedRGB v = pcParseHex(request->arg("valid"),   boardDriver->hlValidColor());
  LedRGB i = pcParseHex(request->arg("invalid"), boardDriver->hlInvalidColor());
  boardDriver->setHighlightColors(s, t, c, v, i);
  request->send(200, "text/plain", "OK");
}

void WiFiManagerESP32::handleResign(AsyncWebServerRequest* request) {
  if (request->hasArg("color")) {
    String color = request->arg("color");
    if (color == "w" || color == "b") {
      pendingResignColor = color.charAt(0);
      hasPendingResign = true;
      Serial.printf("Resign received from web: %s resigns\n", color == "w" ? "White" : "Black");
      request->send(200, "text/plain", "OK");
    } else {
      request->send(400, "text/plain", "Invalid color (use 'w' or 'b')");
    }
  } else {
    request->send(400, "text/plain", "Missing 'color' parameter");
  }
}

void WiFiManagerESP32::handleDraw(AsyncWebServerRequest* request) {
  hasPendingDraw = true;
  Serial.println("Draw agreement received from web");
  request->send(200, "text/plain", "OK");
}

void WiFiManagerESP32::handleConnectWiFi(AsyncWebServerRequest* request) {
  // Handle scanAllChannels toggle
  if (request->hasArg("scanAllChannels")) {
    bool newScanAll = request->arg("scanAllChannels") == "1";
    if (newScanAll != scanAllChannels) {
      scanAllChannels = newScanAll;
      saveProfiles(false);
      Serial.printf("WiFi scan all channels: %s\n", scanAllChannels ? "enabled" : "disabled");
    }
    // If only the toggle was sent, respond OK
    if (!request->hasArg("action")) {
      request->send(200, "text/plain", "OK");
      return;
    }
  }

  String action = request->arg("action");

  // action=scan - request a WiFi scan (deferred to main loop)
  if (action == "scan") {
    pendingWiFi.action = SCAN_NETWORKS;
    request->send(200, "text/plain", "OK");
    return;
  }

  // action=delete&index=N - delete a saved profile
  if (action == "delete" && request->hasArg("index")) {
    int idx = request->arg("index").toInt();
    if (idx < 0 || idx >= profileCount) {
      request->send(400, "text/plain", "Invalid index");
      return;
    }
    pendingWiFi.action = DELETE_PROFILE;
    pendingWiFi.profileIndex = idx;
    request->send(200, "text/plain", "OK");
    return;
  }

  // action=connect&index=N - connect to a saved profile (deferred)
  if (action == "connect" && request->hasArg("index")) {
    int idx = request->arg("index").toInt();
    if (idx < 0 || idx >= profileCount) {
      request->send(400, "text/plain", "Invalid index");
      return;
    }
    pendingWiFi.action = CONNECT_SAVED;
    pendingWiFi.profileIndex = idx;
    request->send(200, "text/plain", "OK");
    return;
  }

  // action=connect with ssid (and optional password) - connect to a new/scanned network (deferred)
  if (action == "connect" && request->hasArg("ssid")) {
    pendingWiFi.newSSID = request->arg("ssid");
    pendingWiFi.newPassword = request->hasArg("password") ? request->arg("password") : "";
    pendingWiFi.newChannel = 0;
    memset(pendingWiFi.newBssid, 0, 6);

    // Look up BSSID and channel from scan results for faster initial connect
    for (int i = 0; i < scanResultCount; i++) {
      if (scanResults[i].ssid == pendingWiFi.newSSID) {
        pendingWiFi.newChannel = scanResults[i].channel;
        memcpy(pendingWiFi.newBssid, scanResults[i].bssid, 6);
        break;
      }
    }

    pendingWiFi.action = CONNECT_NEW;
    request->send(200, "text/plain", "OK");
    return;
  }

  request->send(400, "text/plain", "Missing or invalid parameters");
}

void WiFiManagerESP32::handleGameSelection(AsyncWebServerRequest* request) {
  int mode = 0;
  if (request->hasArg("gamemode"))
    mode = request->arg("gamemode").toInt();
  gameMode = String(mode);
  // Human vs Human: optional player profile IDs (white / black) used for
  // stats logging via the Profiles module + per-player time control.
  if (mode == 1) {
    hvhWhiteId = request->hasArg("whiteId") ? request->arg("whiteId") : String("");
    hvhBlackId = request->hasArg("blackId") ? request->arg("blackId") : String("");
    if (request->hasArg("timeMs")) {
      hvhTimeLimitMs = (uint32_t)request->arg("timeMs").toInt();
    }
    Serial.printf("HvH players: white=%s black=%s time=%lums\n",
                  hvhWhiteId.c_str(), hvhBlackId.c_str(),
                  (unsigned long)hvhTimeLimitMs);
  }
  // If bot game mode, also handle bot config
  if (mode == 2) {
    if (request->hasArg("difficulty") && request->hasArg("playerColor")) {
      switch (request->arg("difficulty").toInt()) {
        case 1:
          botConfig.stockfishSettings = StockfishSettings::easy();
          break;
        case 2:
          botConfig.stockfishSettings = StockfishSettings::medium();
          break;
        case 3:
          botConfig.stockfishSettings = StockfishSettings::hard();
          break;
        case 4:
          botConfig.stockfishSettings = StockfishSettings::expert();
          break;
        default:
          botConfig.stockfishSettings = StockfishSettings::medium();
          break;
      }
      botConfig.playerIsWhite = request->arg("playerColor") == "white";
      Serial.printf("Bot configuration received: Depth=%d, Player is %s\n", botConfig.stockfishSettings.depth, botConfig.playerIsWhite ? "White" : "Black");
    } else {
      request->send(400, "text/plain", "Missing bot parameters");
      return;
    }
  }
  // Simulation: optional manual sub-mode (interactive web board vs scripted).
  if (mode == 5) {
    simManualMode = request->hasArg("simManual") && request->arg("simManual") == "1";
    Serial.printf("Simulation selected (%s)\n", simManualMode ? "manual" : "auto");
  }
  // If Lichess mode, verify token exists
  if (mode == 3) {
    if (lichessToken.length() == 0) {
      request->send(400, "text/plain", "No Lichess API token configured");
      return;
    }
    Serial.println("Lichess mode selected via web");
  }
  // Learning mode: pull opening index + level (+ optional color override).
  if (mode == 6) {
    if (!request->hasArg("openingIdx") || !request->hasArg("level")) {
      request->send(400, "text/plain", "Missing openingIdx/level");
      gameMode = "0";
      return;
    }
    int idx = request->arg("openingIdx").toInt();
    int lvl = request->arg("level").toInt();
    if (idx < 0 || (size_t)idx >= LEARN_OPENINGS_COUNT) {
      request->send(400, "text/plain", "Invalid openingIdx");
      gameMode = "0";
      return;
    }
    if (lvl < 1) lvl = 1;
    if (lvl > 3) lvl = 3;
    learnOpeningIdx = idx;
    learnLevel = lvl;
    learnPlayerColor = ' ';
    if (request->hasArg("color")) {
      String c = request->arg("color");
      if (c == "w" || c == "white") learnPlayerColor = 'w';
      else if (c == "b" || c == "black") learnPlayerColor = 'b';
    }
    Serial.printf("Learn mode: opening=%s level=%d color=%c\n",
                  LEARN_OPENINGS[idx].id, lvl,
                  learnPlayerColor == ' ' ? '?' : learnPlayerColor);
  }
  Serial.println("Game mode selected via web: " + gameMode);
  request->send(200, "text/plain", "OK");
}

String WiFiManagerESP32::getLichessInfoJSON() {
  // Don't expose the actual token, just whether it exists and a masked version
  String maskedToken = "";
  if (lichessToken.length() > 8) {
    maskedToken = lichessToken.substring(0, 4) + "..." + lichessToken.substring(lichessToken.length() - 4);
  } else if (lichessToken.length() > 0) {
    maskedToken = "****";
  }
  JsonDocument doc;
  doc["hasToken"] = (lichessToken.length() > 0);
  doc["maskedToken"] = maskedToken;
  String output;
  serializeJson(doc, output);
  return output;
}

void WiFiManagerESP32::handleSaveLichessToken(AsyncWebServerRequest* request) {
  if (!request->hasArg("token")) {
    request->send(400, "text/plain", "Missing token parameter");
    return;
  }

  String newToken = request->arg("token");
  newToken.trim();

  if (newToken.length() < 10) {
    request->send(400, "text/plain", "Token too short");
    return;
  }

  // Save to NVS
  if (!ChessUtils::ensureNvsInitialized()) {
    request->send(500, "text/plain", "NVS init failed");
    return;
  }

  prefs.begin("lichess", false);
  prefs.putString("token", newToken);
  prefs.end();

  lichessToken = newToken;
  Serial.println("Lichess API token saved to NVS");

  request->send(200, "text/plain", "OK");
}

String WiFiManagerESP32::getBoardSettingsJSON() {
  JsonDocument doc;
  doc["brightness"] = boardDriver->getBrightness();
  doc["dimMultiplier"] = boardDriver->getDimMultiplier();
  doc["idleAnimation"] = (uint8_t)boardDriver->getIdleAnimation();
  char colorHex[8];
  LedRGB c = boardDriver->getIdleColor();
  snprintf(colorHex, sizeof(colorHex), "#%02x%02x%02x", c.r, c.g, c.b);
  doc["idleColor"] = colorHex;
  LedRGB c2 = boardDriver->getIdleColor2();
  snprintf(colorHex, sizeof(colorHex), "#%02x%02x%02x", c2.r, c2.g, c2.b);
  doc["idleColor2"] = colorHex;
  doc["animSpeed"] = boardDriver->getAnimSpeed();
  doc["idleSaturation"] = boardDriver->getIdleSaturation();
  doc["idleRandomColor"] = boardDriver->getIdleRandomColor();
  doc["idleText"] = boardDriver->getIdleText();
  doc["textMode"] = boardDriver->getTextMode();
  LedRGB tc = boardDriver->getTextColor();
  snprintf(colorHex, sizeof(colorHex), "#%02x%02x%02x", tc.r, tc.g, tc.b);
  doc["textColor"] = colorHex;
  LedRGB tb = boardDriver->getTextBg();
  snprintf(colorHex, sizeof(colorHex), "#%02x%02x%02x", tb.r, tb.g, tb.b);
  doc["textBg"] = colorHex;
  doc["autoDim"] = boardDriver->getAutoDim();
  doc["nightOff"] = boardDriver->getNightOff();
  doc["autoDimStart"] = boardDriver->getAutoDimStart();
  doc["autoDimEnd"] = boardDriver->getAutoDimEnd();
  doc["autoDimBrightness"] = boardDriver->getAutoDimBrightness();
  // Per-player chess animation tuning.
  LedRGB apw = boardDriver->animPathColorWhite();
  LedRGB apb = boardDriver->animPathColorBlack();
  snprintf(colorHex, sizeof(colorHex), "#%02x%02x%02x", apw.r, apw.g, apw.b);
  doc["animPathColorW"] = colorHex;
  snprintf(colorHex, sizeof(colorHex), "#%02x%02x%02x", apb.r, apb.g, apb.b);
  doc["animPathColorB"] = colorHex;
  doc["animSpeedW"] = boardDriver->animChessSpeedWhite();
  doc["animSpeedB"] = boardDriver->animChessSpeedBlack();
  doc["knightPathPct"]    = boardDriver->getKnightPathPct();
  doc["pickupPulseMs"]    = boardDriver->getPickupPulseMs();
  doc["walkBudgetMs"]     = boardDriver->getWalkBudgetMs();
  doc["replayIntervalMs"] = boardDriver->getReplayIntervalMs();
  doc["replayOverlayPct"] = boardDriver->getReplayOverlayPct();
  doc["dimOthersOnPickup"] = boardDriver->getDimOthersOnPickup();
  doc["dimOthersPct"]      = boardDriver->getDimOthersPct();
  doc["revealRingMs"]      = boardDriver->getRevealRingMs();
  doc["revealPauseMs"]     = boardDriver->getRevealPauseMs();
  doc["revealSequential"]  = boardDriver->getRevealSequential();
  doc["noobBudgetMs"]      = boardDriver->getNoobBudgetMs();
  doc["afterWalkMs"]       = boardDriver->getAfterWalkMs();
  doc["rayGhostVal"]       = boardDriver->getRayGhostVal();
  doc["shinySweepMs"]      = boardDriver->getShinySweepMs();
  doc["shinyPauseMs"]      = boardDriver->getShinyPauseMs();
  LedRGB shc = boardDriver->getShinyColor();
  snprintf(colorHex, sizeof(colorHex), "#%02x%02x%02x", shc.r, shc.g, shc.b);
  doc["shinyColor"]        = colorHex;
  String output;
  serializeJson(doc, output);
  return output;
}

void WiFiManagerESP32::handleBoardSettings(AsyncWebServerRequest* request) {
  bool changed = false;

  if (request->hasArg("brightness")) {
    int brightness = request->arg("brightness").toInt();
    if (brightness >= 0 && brightness <= 255) {
      boardDriver->setBrightness((uint8_t)brightness);
      changed = true;
    }
  }

  if (request->hasArg("dimMultiplier")) {
    int dimMult = request->arg("dimMultiplier").toInt();
    if (dimMult >= 0 && dimMult <= 100) {
      boardDriver->setDimMultiplier((uint8_t)dimMult);
      changed = true;
    }
  }

  if (request->hasArg("idleAnimation")) {
    int anim = request->arg("idleAnimation").toInt();
    if (anim >= 0 && anim <= 47) {
      boardDriver->setIdleAnimation((IdleAnimation)anim);
      changed = true;
    }
  }

  auto parseHexColor = [](const String& arg, LedRGB& out) -> bool {
    String hex = arg;
    if (hex.startsWith("#")) hex = hex.substring(1);
    if (hex.length() != 6) return false;
    out.r = (uint8_t)strtol(hex.substring(0, 2).c_str(), nullptr, 16);
    out.g = (uint8_t)strtol(hex.substring(2, 4).c_str(), nullptr, 16);
    out.b = (uint8_t)strtol(hex.substring(4, 6).c_str(), nullptr, 16);
    return true;
  };

  if (request->hasArg("idleColor")) {
    LedRGB c{};
    if (parseHexColor(request->arg("idleColor"), c)) {
      boardDriver->setIdleColor(c);
      changed = true;
    }
  }

  if (request->hasArg("idleColor2")) {
    LedRGB c{};
    if (parseHexColor(request->arg("idleColor2"), c)) {
      boardDriver->setIdleColor2(c);
      changed = true;
    }
  }

  if (request->hasArg("animSpeed")) {
    float speed = request->arg("animSpeed").toFloat();
    if (speed >= 0.25f && speed <= 4.0f) {
      boardDriver->setAnimSpeed(speed);
      changed = true;
    }
  }

  if (request->hasArg("idleSaturation")) {
    int sat = request->arg("idleSaturation").toInt();
    if (sat >= 0 && sat <= 100) {
      boardDriver->setIdleSaturation((uint8_t)sat);
      changed = true;
    }
  }

  if (request->hasArg("idleRandomColor")) {
    boardDriver->setIdleRandomColor(request->arg("idleRandomColor") == "1" || request->arg("idleRandomColor") == "true");
    changed = true;
  }

  if (request->hasArg("idleText")) {
    boardDriver->setIdleText(request->arg("idleText").c_str());
    changed = true;
  }

  if (request->hasArg("textMode")) {
    boardDriver->setTextMode((uint8_t)request->arg("textMode").toInt());
    changed = true;
  }

  if (request->hasArg("textColor")) {
    LedRGB c{};
    if (parseHexColor(request->arg("textColor"), c)) { boardDriver->setTextColor(c); changed = true; }
  }

  if (request->hasArg("textBg")) {
    LedRGB c{};
    if (parseHexColor(request->arg("textBg"), c)) { boardDriver->setTextBg(c); changed = true; }
  }

  if (request->hasArg("autoDim")) {
    boardDriver->setAutoDim(request->arg("autoDim") == "1" || request->arg("autoDim") == "true");
    changed = true;
  }

  if (request->hasArg("nightOff")) {
    boardDriver->setNightOff(request->arg("nightOff") == "1" || request->arg("nightOff") == "true");
    changed = true;
  }

  if (request->hasArg("autoDimStart")) {
    int h = request->arg("autoDimStart").toInt();
    if (h >= 0 && h <= 23) { boardDriver->setAutoDimStart((uint8_t)h); changed = true; }
  }

  if (request->hasArg("autoDimEnd")) {
    int h = request->arg("autoDimEnd").toInt();
    if (h >= 0 && h <= 23) { boardDriver->setAutoDimEnd((uint8_t)h); changed = true; }
  }

  if (request->hasArg("autoDimBrightness")) {
    int b = request->arg("autoDimBrightness").toInt();
    if (b >= 10 && b <= 255) { boardDriver->setAutoDimBrightness((uint8_t)b); changed = true; }
  }

  // Per-player chess animation tuning — accept any subset of the four params
  // and write all four back so partial updates don't reset the other side.
  bool animChanged = false;
  LedRGB animW = boardDriver->animPathColorWhite();
  LedRGB animB = boardDriver->animPathColorBlack();
  float speedW = boardDriver->animChessSpeedWhite();
  float speedB = boardDriver->animChessSpeedBlack();
  if (request->hasArg("animPathColorW") && parseHexColor(request->arg("animPathColorW"), animW)) animChanged = true;
  if (request->hasArg("animPathColorB") && parseHexColor(request->arg("animPathColorB"), animB)) animChanged = true;
  if (request->hasArg("animSpeedW")) { speedW = request->arg("animSpeedW").toFloat(); animChanged = true; }
  if (request->hasArg("animSpeedB")) { speedB = request->arg("animSpeedB").toFloat(); animChanged = true; }
  if (animChanged) {
    boardDriver->setAnimPathColors(animW, animB);
    boardDriver->setAnimChessSpeeds(speedW, speedB);
    changed = true;
  }

  if (request->hasArg("knightPathPct")) {
    boardDriver->setKnightPathPct((uint8_t)request->arg("knightPathPct").toInt());
    changed = true;
  }
  if (request->hasArg("pickupPulseMs")) {
    boardDriver->setPickupPulseMs((uint16_t)request->arg("pickupPulseMs").toInt());
    changed = true;
  }
  if (request->hasArg("walkBudgetMs")) {
    boardDriver->setWalkBudgetMs((uint16_t)request->arg("walkBudgetMs").toInt());
    changed = true;
  }
  if (request->hasArg("replayIntervalMs")) {
    boardDriver->setReplayIntervalMs((uint16_t)request->arg("replayIntervalMs").toInt());
    changed = true;
  }
  if (request->hasArg("replayOverlayPct")) {
    boardDriver->setReplayOverlayPct((uint8_t)request->arg("replayOverlayPct").toInt());
    changed = true;
  }
  if (request->hasArg("dimOthersOnPickup")) {
    boardDriver->setDimOthersOnPickup(request->arg("dimOthersOnPickup") == "1" || request->arg("dimOthersOnPickup") == "true");
    changed = true;
  }
  if (request->hasArg("dimOthersPct")) {
    boardDriver->setDimOthersPct((uint8_t)request->arg("dimOthersPct").toInt());
    changed = true;
  }
  if (request->hasArg("revealRingMs")) {
    boardDriver->setRevealRingMs((uint16_t)request->arg("revealRingMs").toInt());
    changed = true;
  }
  if (request->hasArg("revealPauseMs")) {
    boardDriver->setRevealPauseMs((uint16_t)request->arg("revealPauseMs").toInt());
    changed = true;
  }
  if (request->hasArg("noobBudgetMs"))
    boardDriver->setNoobBudgetMs((uint16_t)request->arg("noobBudgetMs").toInt());
  if (request->hasArg("revealSequential")) {
    String v = request->arg("revealSequential");
    boardDriver->setRevealSequential(v == "1" || v == "true");
    changed = true;
  }
  if (request->hasArg("afterWalkMs")) {
    boardDriver->setAfterWalkMs((uint16_t)request->arg("afterWalkMs").toInt());
    changed = true;
  }
  if (request->hasArg("rayGhostVal")) {
    boardDriver->setRayGhostVal((uint8_t)request->arg("rayGhostVal").toInt());
    changed = true;
  }
  if (request->hasArg("shinySweepMs")) {
    boardDriver->setShinySweepMs((uint16_t)request->arg("shinySweepMs").toInt());
    changed = true;
  }
  if (request->hasArg("shinyPauseMs")) {
    boardDriver->setShinyPauseMs((uint16_t)request->arg("shinyPauseMs").toInt());
    changed = true;
  }
  if (request->hasArg("shinyColor")) {
    LedRGB c;
    if (parseHexColor(request->arg("shinyColor"), c)) { boardDriver->setShinyColor(c); changed = true; }
  }

  if (changed) {
    boardDriver->saveLedSettings();
    Serial.println("Board settings updated via web interface");
    request->send(200, "text/plain", "OK");
  } else {
    request->send(400, "text/plain", "No valid settings provided");
  }
}

void WiFiManagerESP32::handleBoardCalibration(AsyncWebServerRequest* request) {
  boardDriver->triggerCalibration();
  request->send(200, "text/plain", "Calibration will start on next reboot");
}

void WiFiManagerESP32::handleDebugState(AsyncWebServerRequest* request) {
  const HardwareConfig& hw = boardDriver->getHardwareConfig();
  JsonDocument doc;
  doc["calibrated"] = boardDriver->isCalibrated();
  doc["freeHeap"] = ESP.getFreeHeap();
  doc["uptime"] = millis() / 1000;
  doc["tempC"] = temperatureRead();  // interner Die-Sensor (Chip-Temp, nicht Umgebung)
  JsonArray sensors = doc["sensors"].to<JsonArray>();
  JsonArray sensorsRaw = doc["sensorsRaw"].to<JsonArray>();
  JsonArray leds = doc["leds"].to<JsonArray>();
  for (int row = 0; row < NUM_ROWS; row++) {
    JsonArray srow = sensors.add<JsonArray>();
    JsonArray srrow = sensorsRaw.add<JsonArray>();
    JsonArray lrow = leds.add<JsonArray>();
    for (int col = 0; col < NUM_COLS; col++) {
      srow.add(boardDriver->getSensorState(row, col));
      srrow.add(boardDriver->getSensorRaw(row, col));
      LedRGB c = boardDriver->getLEDColor(row, col);
      char hex[8];
      snprintf(hex, sizeof(hex), "#%02x%02x%02x", c.r, c.g, c.b);
      lrow.add(hex);
    }
  }
  // Direct digitalRead of ALL defined row pins (snapshot)
  JsonArray pinReads = doc["rowPinReads"].to<JsonArray>();
  for (int i = 0; i < NUM_ALL_ROW_PINS; i++) {
    JsonObject p = pinReads.add<JsonObject>();
    p["gpio"] = ALL_ROW_PINS[i];
    p["low"] = (digitalRead(ALL_ROW_PINS[i]) == LOW);
  }
  JsonObject hwObj = doc["hw"].to<JsonObject>();
  hwObj["ledPin"] = hw.ledPin;
  hwObj["srClk"] = hw.srClkPin;
  hwObj["srLatch"] = hw.srLatchPin;
  hwObj["srData"] = hw.srDataPin;
  hwObj["srInvert"] = hw.srInvertOutputs;
  JsonArray rp = hwObj["rowPins"].to<JsonArray>();
  for (int i = 0; i < NUM_ROWS; i++) rp.add(hw.rowPins[i]);
  // Diagnostics extras (for the index Diagnose dashboard)
  doc["ssid"]   = WiFi.SSID();
  doc["ip"]     = WiFi.localIP().toString();
  doc["rssi"]   = WiFi.RSSI();
  doc["cpuMHz"] = ESP.getCpuFreqMHz();
  doc["sdkVer"] = ESP.getSdkVersion();
  doc["mode"]   = gameMode;
  doc["idleAnim"] = (uint8_t)boardDriver->getIdleAnimation();
  doc["hasLichessToken"] = lichessToken.length() > 0;
  String output;
  serializeJson(doc, output);
  request->send(200, "application/json", output);
}

void WiFiManagerESP32::handleDebugTestLeds(AsyncWebServerRequest* request) {
  boardDriver->acquireLEDs();
  for (int row = 0; row < NUM_ROWS; row++)
    for (int col = 0; col < NUM_COLS; col++)
      boardDriver->setSquareLED(row, col, {255, 0, 0});
  boardDriver->showLEDs();
  boardDriver->releaseLEDs();
  request->send(200, "text/plain", "OK");
}

void WiFiManagerESP32::getHardwareConfigJSON(AsyncWebServerRequest* request) {
  const HardwareConfig& hw = boardDriver->getHardwareConfig();
  JsonDocument doc;
  doc["ledPin"] = hw.ledPin;
  doc["srClkPin"] = hw.srClkPin;
  doc["srLatchPin"] = hw.srLatchPin;
  doc["srDataPin"] = hw.srDataPin;
  doc["srInvertOutputs"] = hw.srInvertOutputs;
  JsonArray arr = doc["rowPins"].to<JsonArray>();
  for (int i = 0; i < NUM_ROWS; i++) arr.add(hw.rowPins[i]);
  String output;
  serializeJson(doc, output);
  request->send(200, "application/json", output);
}

void WiFiManagerESP32::handleHardwareConfig(AsyncWebServerRequest* request) {
  HardwareConfig config = boardDriver->getHardwareConfig();
  bool changed = false;

  auto getPin = [&](const char* name, uint8_t& pin) {
    if (request->hasArg(name)) {
      int val = request->arg(name).toInt();
      if (val >= 0 && val <= 39) { // ESP32 GPIO range
        if ((uint8_t)val != pin) {
          pin = (uint8_t)val;
          changed = true;
        }
      }
    }
  };

  getPin("ledPin", config.ledPin);
  getPin("srClkPin", config.srClkPin);
  getPin("srLatchPin", config.srLatchPin);
  getPin("srDataPin", config.srDataPin);

  if (request->hasArg("srInvertOutputs") && (request->arg("srInvertOutputs") == "1") != config.srInvertOutputs) {
    config.srInvertOutputs = !config.srInvertOutputs;
    changed = true;
  }

  for (int i = 0; i < NUM_ROWS; i++) {
    String key = "rowPin" + String(i);
    getPin(key.c_str(), config.rowPins[i]);
  }

  if (changed) {
    boardDriver->saveHardwareConfig(config);
    request->send(200, "text/plain", "Hardware config saved. Rebooting...");
    // Defer reboot to a separate task so the HTTP response is fully sent first
    xTaskCreate(
        [](void*) {
          delay(500);
          ESP.restart();
          vTaskDelete(NULL);
        },
        "hw_reboot", 2048, NULL, 1, NULL);
  } else {
    request->send(200, "text/plain", "No changes detected");
  }
}

LichessConfig WiFiManagerESP32::getLichessConfig() {
  LichessConfig config;
  config.apiToken = lichessToken;
  return config;
}

void WiFiManagerESP32::setGameStatus(const GameStatusData& gs) {
  bool meaningfulChange =
      gs.turn         != _gameStatus.turn         ||
      gs.inCheck      != _gameStatus.inCheck      ||
      gs.gameOver     != _gameStatus.gameOver     ||
      gs.winnerColor  != _gameStatus.winnerColor  ||
      gs.whiteFlagged != _gameStatus.whiteFlagged ||
      gs.blackFlagged != _gameStatus.blackFlagged;
  _gameStatus = gs;
  // Broadcast the fresh turn/check/clock the instant they change, so the web
  // app reflects them right after the move instead of one move later. The
  // payload also carries the current FEN (already updated by the move's
  // updateBoardState call), so position and status stay in sync. Clock-only
  // changes don't broadcast — the client interpolates those locally.
  if (meaningfulChange && boardEvents.count() > 0) {
    boardEvents.send(getBoardUpdateJSON().c_str(), "board", millis());
  }
}

void WiFiManagerESP32::updateBoardState(const String& fen, float evaluation) {
  currentFen = fen;
  boardEvaluation = evaluation;
  // Keep the turn indicator in sync with the position being pushed. The FEN's
  // side-to-move field is authoritative and arrives the instant the move is
  // applied (via the early push in applyMove), so the web clock switches to the
  // opponent immediately — instead of after the ~1 s LED animation, when the
  // next loop-top setGameStatus() would otherwise be the first to carry the new
  // turn. Without this the mover's clock kept ticking through the animation.
  int sp = fen.indexOf(' ');
  if (sp > 0 && sp + 1 < (int)fen.length()) {
    char t = fen.charAt(sp + 1);
    if (t == 'w' || t == 'b') _gameStatus.turn = t;
  }
  // Broadcast to all connected SSE clients (board.html). Cheap because
  // updates fire ~once per half-move, not per polling tick.
  if (boardEvents.count() > 0) boardEvents.send(getBoardUpdateJSON().c_str(), "board", millis());
}

bool WiFiManagerESP32::getPendingBoardEdit(String& fenOut) {
  if (hasPendingEdit) {
    fenOut = pendingFenEdit;
    return true;
  }
  return false;
}

void WiFiManagerESP32::clearPendingEdit() {
  currentFen = pendingFenEdit;
  hasPendingEdit = false;
  if (boardEvents.count() > 0) boardEvents.send(getBoardUpdateJSON().c_str(), "board", millis());
}

bool WiFiManagerESP32::getPendingHighlight(String& from, String& targets) {
  if (!hasPendingHighlight) return false;
  from = pendingHlFrom;
  targets = pendingHlTargets;
  hasPendingHighlight = false;
  return true;
}

bool WiFiManagerESP32::getPendingResign(char& resignColor) {
  if (hasPendingResign) {
    resignColor = pendingResignColor;
    return true;
  }
  return false;
}

bool WiFiManagerESP32::getPendingDraw() {
  return hasPendingDraw;
}

void WiFiManagerESP32::clearPendingResign() {
  hasPendingResign = false;
  pendingResignColor = '?';
}

void WiFiManagerESP32::clearPendingDraw() {
  hasPendingDraw = false;
}

bool WiFiManagerESP32::getPendingManualEnd(char& winner) {
  if (!hasPendingManualEnd) return false;
  winner = pendingManualWinner;
  hasPendingManualEnd = false;
  pendingManualWinner = '?';
  return true;
}

void WiFiManagerESP32::handlePromotion(AsyncWebServerRequest* request) {
  if (!promotion.pending) {
    request->send(400, "text/plain", "No promotion pending");
    return;
  }
  if (request->hasArg("piece")) {
    String piece = request->arg("piece");
    piece.toLowerCase();
    if (piece == "q" || piece == "r" || piece == "b" || piece == "n") {
      promotion.choice = piece.charAt(0);
      Serial.printf("Promotion choice received from web: %c\n", (char)promotion.choice);
      request->send(200, "text/plain", "OK");
    } else {
      request->send(400, "text/plain", "Invalid piece (use 'q', 'r', 'b', or 'n')");
    }
  } else {
    request->send(400, "text/plain", "Missing 'piece' parameter");
  }
}

void WiFiManagerESP32::startPromotionWait(char color) {
  promotion.color = color;
  promotion.choice = ' ';
  promotion.pending = true;
  Serial.printf("Promotion wait started for %s\n", color == 'w' ? "White" : "Black");
}

void WiFiManagerESP32::clearPromotion() {
  promotion.reset();
}

bool WiFiManagerESP32::isWebClientConnected() const {
  // Consider web client connected if it polled within the last 2 seconds
  return lastBoardPollTime > 0 && (millis() - lastBoardPollTime < 2000);
}

void WiFiManagerESP32::checkPendingWiFi() {
  // Auto-reconnect: if we were connected to a network and lost it, try to reconnect
  if (connectedProfileIndex >= 0 && WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi connection lost, attempting reconnect...");
    connectedProfileIndex = -1;
    if (!connectToSavedProfile()) startAPFallback();
  }

  if (pendingWiFi.action == NONE) return;
  PendingAction action = pendingWiFi.action;
  pendingWiFi.action = NONE;

  switch (action) {
    case SCAN_NETWORKS:
      performScan();
      break;

    case DELETE_PROFILE: {
      int idx = pendingWiFi.profileIndex;
      if (idx >= 0 && idx < profileCount) {
        bool deletingConnected = (idx == connectedProfileIndex);
        Serial.printf("Deleting WiFi profile %d: %s\n", idx, profiles[idx].ssid.c_str());
        // If deleting the connected profile, disconnect
        if (deletingConnected) {
          WiFi.disconnect(false, true);
          connectedProfileIndex = -1;
        } else if (connectedProfileIndex > idx) {
          connectedProfileIndex--;
        }
        // Shift remaining profiles up
        for (int i = idx; i < profileCount - 1; i++)
          profiles[i] = profiles[i + 1];
        profileCount--;
        saveProfiles();
        if (deletingConnected && !connectToSavedProfile()) startAPFallback();
      }
      break;
    }

    case CONNECT_SAVED: {
      int idx = pendingWiFi.profileIndex;
      if (idx >= 0 && idx < profileCount) {
        if (tryConnectProfile(idx)) {
          promoteProfile(idx);
          saveProfiles();
        } else {
          if (!connectToSavedProfile()) startAPFallback();
        }
      }
      break;
    }

    case CONNECT_NEW: {
      String ssid = pendingWiFi.newSSID;
      String password = pendingWiFi.newPassword;
      bool connected = false;
      if (pendingWiFi.newChannel != 0 && !scanAllChannels)
        connected = tryConnect(ssid, password, pendingWiFi.newBssid, pendingWiFi.newChannel);
      if (!connected)
        connected = tryConnect(ssid, password);

      if (connected) {
        // Check if this SSID already exists in profiles (update it)
        int existingIdx = -1;
        for (int i = 0; i < profileCount; i++) {
          if (profiles[i].ssid == ssid) {
            existingIdx = i;
            break;
          }
        }

        WiFiProfile newProfile;
        newProfile.ssid = ssid;
        newProfile.password = password;
        uint8_t* connBssid = WiFi.BSSID();
        if (connBssid) {
          memcpy(newProfile.bssid, connBssid, 6);
          newProfile.channel = WiFi.channel();
          newProfile.hasBssid = true;
        }

        if (existingIdx >= 0) {
          // Update existing and promote to top
          profiles[existingIdx] = newProfile;
          promoteProfile(existingIdx);
        } else {
          // Add new profile at top, shift others down
          if (profileCount >= MAX_WIFI_PROFILES) profileCount = MAX_WIFI_PROFILES - 1;
          for (int i = profileCount; i > 0; i--)
            profiles[i] = profiles[i - 1];
          profiles[0] = newProfile;
          profileCount++;
        }
        connectedProfileIndex = 0;
        saveProfiles();
        Serial.println("New WiFi profile saved and connected!");
      } else {
        Serial.println("Failed to connect to new network, trying saved profiles...");
        if (!connectToSavedProfile()) startAPFallback();
      }
      break;
    }

    default:
      break;
  }
}

bool WiFiManagerESP32::ensureConnected() {
  if (WiFi.status() == WL_CONNECTED) return true;
  Serial.println("WiFi not connected, attempting reconnect...");
  if (connectToSavedProfile()) return true;
  startAPFallback();
  return false;
}

void WiFiManagerESP32::startMDNS() {
  MDNS.end();
  if (MDNS.begin(MDNS_HOSTNAME)) {
    MDNS.addService("http", "tcp", HTTP_PORT);
    Serial.println("mDNS started: http://" MDNS_HOSTNAME ".local");
  } else {
    Serial.println("mDNS failed to start");
  }
}

void WiFiManagerESP32::handleGamesRequest(AsyncWebServerRequest* request) {
  if (request->hasArg("id")) {
    String idStr = request->arg("id");

    // GET /games?id=live1 — return live moves file directly
    if (idStr == "live1") {
      if (!MoveHistory::quietExists("/games/live.bin")) {
        request->send(404, "text/plain", "No live game");
        return;
      }
      AsyncWebServerResponse* response = request->beginResponse(LittleFS, "/games/live.bin", "application/octet-stream", true);
      request->send(response);
      return;
    }

    // GET /games?id=live2 — return live FEN table file directly
    if (idStr == "live2") {
      if (!MoveHistory::quietExists("/games/live_fen.bin")) {
        request->send(404, "text/plain", "No live FEN table");
        return;
      }
      AsyncWebServerResponse* response = request->beginResponse(LittleFS, "/games/live_fen.bin", "application/octet-stream", true);
      request->send(response);
      return;
    }

    // GET /games?id=N — return binary of game N
    int id = idStr.toInt();
    if (id <= 0) {
      request->send(400, "text/plain", "Invalid game id");
      return;
    }

    String path = MoveHistory::gamePath(id);
    if (!MoveHistory::quietExists(path.c_str())) {
      request->send(404, "text/plain", "Game not found");
      return;
    }
    // Serve file directly from LittleFS
    AsyncWebServerResponse* response = request->beginResponse(LittleFS, path, "application/octet-stream", true);
    request->send(response);
  } else {
    // GET /games — return JSON list of all saved games
    request->send(200, "application/json", moveHistory->getGameListJSON());
  }
}

void WiFiManagerESP32::handleDeleteGame(AsyncWebServerRequest* request) {
  if (!request->hasArg("id")) {
    request->send(400, "text/plain", "Missing id parameter");
    return;
  }

  int id = request->arg("id").toInt();
  if (id <= 0) {
    request->send(400, "text/plain", "Invalid game id");
    return;
  }

  if (moveHistory->deleteGame(id))
    request->send(200, "text/plain", "OK");
  else
    request->send(404, "text/plain", "Game not found");
}

// ========== OTA Update Handlers ==========

void WiFiManagerESP32::handleOtaStatus(AsyncWebServerRequest* request) {
  if (lastUpdateInfo.version.isEmpty()) {
    request->send(400, "text/plain", "No update info available.");
    return;
  }

  JsonDocument doc;
  doc["version"] = FIRMWARE_VERSION;
  doc["autoUpdate"] = autoOtaEnabled;
  doc["available"] = lastUpdateInfo.available;
  doc["latestVersion"] = lastUpdateInfo.version;
  doc["hasFirmware"] = lastUpdateInfo.firmwareUrl.length() > 0;
  doc["hasWebAssets"] = lastUpdateInfo.webAssetsUrl.length() > 0;
  String output;
  serializeJson(doc, output);
  request->send(200, "application/json", output);
}

void WiFiManagerESP32::handleOtaSettings(AsyncWebServerRequest* request) {
  if (request->hasArg("autoUpdate")) {
    autoOtaEnabled = request->arg("autoUpdate") == "1";
    if (ChessUtils::ensureNvsInitialized()) {
      Preferences p;
      p.begin("ota", false);
      p.putBool("autoUpdate", autoOtaEnabled);
      p.end();
    }
    Serial.printf("OTA: Auto-update %s\n", autoOtaEnabled ? "enabled" : "disabled");
    request->send(200, "text/plain", "OK");
  } else {
    request->send(400, "text/plain", "Missing parameter");
  }
}

// Parameters passed to the OTA apply task via heap allocation (avoids static variable race conditions)
struct OtaApplyParams {
  OtaUpdateInfo info;
  OtaUpdater* updater;
};

void WiFiManagerESP32::handleOtaApply(AsyncWebServerRequest* request) {
  if (!lastUpdateInfo.available) {
    request->send(400, "text/plain", "No update available. Check for updates first.");
    return;
  }

  request->send(200, "text/plain", "Updating... The board will reboot when complete.");

  // Run update in a separate task to not block the web server response.
  // Heap-allocate params so the info survives after this function returns.
  auto* params = new OtaApplyParams{lastUpdateInfo, &otaUpdater};
  lastUpdateInfo.available = false; // Prevent concurrent apply requests

  xTaskCreate(
      [](void* param) {
        auto* p = static_cast<OtaApplyParams*>(param);
        delay(500); // Give time for the HTTP response to be sent
        p->updater->applyUpdate(p->info);
        delete p;
        vTaskDelete(NULL);
      },
      "ota_apply", 8192, params, 1, NULL);
}

void WiFiManagerESP32::onFirmwareUploadBody(AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
  // Can't use applyFirmwareFromStream() here — ESPAsyncWebServer delivers the body in async chunks,
  // not as a Stream. We call Update.begin/write/end incrementally across chunk callbacks instead.
  static std::atomic<bool>* stopFlag = nullptr;
  if (index == 0) {
    if (stopFlag == nullptr)
      stopFlag = boardDriver->startWaitingAnimation();
    Serial.printf("OTA: Firmware upload started (%d bytes)\n", total);
    if (!Update.begin(total, U_FLASH)) {
      Serial.printf("OTA: Not enough space: %s\n", Update.errorString());
      return;
    }
  }
  if (Update.isRunning()) {
    if (Update.write(data, len) != len) {
      Serial.printf("OTA: Write failed: %s\n", Update.errorString());
      Update.abort();
    }
  }
  if (index + len == total) {
    if (stopFlag) {
      stopFlag->store(true);
      stopFlag = nullptr;
    }
    if (!Update.isRunning()) {
      // Update.begin() failed or a write error aborted the update
      request->send(500, "text/plain", "Firmware update failed");
    } else if (Update.end(true)) {
      Serial.println("OTA: Firmware upload complete, rebooting...");
      request->send(200, "text/plain", "Firmware updated! Rebooting...");
      boardDriver->flashBoardAnimation(LedColors::Blue, 2);
      delay(500);
      ESP.restart();
    } else {
      Serial.printf("OTA: Finalize failed: %s\n", Update.errorString());
      request->send(500, "text/plain", String("Firmware update failed: ") + Update.errorString());
    }
  }
}

void WiFiManagerESP32::onWebAssetsUploadBody(AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
  // Stream the TAR straight to LittleFS: the old path buffered the whole archive
  // to a temp file first, but tar + extracted files together blow past the 320 KB
  // partition. OtaUpdater's streaming parser reassembles 512-byte blocks across
  // async chunks and writes each entry directly, so only the extracted files
  // ever occupy the FS.
  static std::atomic<bool>* stopFlag = nullptr;
  if (index == 0) {
    if (stopFlag == nullptr)
      stopFlag = boardDriver->startWaitingAnimation();
    Serial.printf("OTA: Web assets upload started (%d bytes)\n", total);
    otaUpdater.beginWebStream();
  }
  otaUpdater.feedWebStream(data, len);
  if (index + len == total) {
    int files = 0;
    bool success = otaUpdater.endWebStream(&files);
    if (success)
      request->send(200, "text/plain", "Web assets updated successfully!");
    else
      request->send(500, "text/plain", "Web assets update failed");
    if (stopFlag) {
      stopFlag->store(true);
      stopFlag = nullptr;
    }
  }
}

void WiFiManagerESP32::loadProfiles() {
  profileCount = 0;
  connectedProfileIndex = -1;

  prefs.begin(NVS_WIFI_NAMESPACE, false);
  profileCount = prefs.getUChar("count", 0);
  if (profileCount > MAX_WIFI_PROFILES) profileCount = MAX_WIFI_PROFILES;
  scanAllChannels = prefs.getBool("scanAll", false);

  for (int i = 0; i < profileCount; i++) {
    String idx = String(i);
    profiles[i].ssid = prefs.getString(("ssid" + idx).c_str(), "");
    profiles[i].password = prefs.getString(("pass" + idx).c_str(), "");
    profiles[i].hasBssid = prefs.getBool(("hasBss" + idx).c_str(), false);
    profiles[i].channel = prefs.getUChar(("chan" + idx).c_str(), 0);
    if (profiles[i].hasBssid) {
      size_t len = prefs.getBytes(("bssid" + idx).c_str(), profiles[i].bssid, 6);
      if (len != 6) {
        profiles[i].hasBssid = false;
        memset(profiles[i].bssid, 0, 6);
      }
    }
    Serial.printf("  Profile %d: SSID=%s, hasBSSID=%s, channel=%d\n", i, profiles[i].ssid.c_str(), profiles[i].hasBssid ? "yes" : "no", profiles[i].channel);
  }
  prefs.end();
  Serial.printf("Loaded %d WiFi profile(s) from NVS\n", profileCount);
}

void WiFiManagerESP32::saveProfiles(bool saveAllProfiles) {
  if (!ChessUtils::ensureNvsInitialized()) return;

  prefs.begin(NVS_WIFI_NAMESPACE, false);
  prefs.putBool("scanAll", scanAllChannels);
  if (!saveAllProfiles) {
    prefs.end();
    return;
  }

  prefs.putUChar("count", (uint8_t)profileCount);
  for (int i = 0; i < MAX_WIFI_PROFILES; i++) {
    String idx = String(i);
    if (i < profileCount) {
      prefs.putString(("ssid" + idx).c_str(), profiles[i].ssid);
      prefs.putString(("pass" + idx).c_str(), profiles[i].password);
      prefs.putBool(("hasBss" + idx).c_str(), profiles[i].hasBssid);
      prefs.putUChar(("chan" + idx).c_str(), profiles[i].channel);
      if (profiles[i].hasBssid)
        prefs.putBytes(("bssid" + idx).c_str(), profiles[i].bssid, 6);
    } else {
      if (prefs.isKey(("ssid" + idx).c_str())) prefs.remove(("ssid" + idx).c_str());
      if (prefs.isKey(("pass" + idx).c_str())) prefs.remove(("pass" + idx).c_str());
      if (prefs.isKey(("hasBss" + idx).c_str())) prefs.remove(("hasBss" + idx).c_str());
      if (prefs.isKey(("chan" + idx).c_str())) prefs.remove(("chan" + idx).c_str());
      if (prefs.isKey(("bssid" + idx).c_str())) prefs.remove(("bssid" + idx).c_str());
    }
  }
  prefs.end();
}

void WiFiManagerESP32::promoteProfile(int index) {
  if (index <= 0 || index >= profileCount) return;
  WiFiProfile promoted = profiles[index];
  for (int i = index; i > 0; i--)
    profiles[i] = profiles[i - 1];
  profiles[0] = promoted;
  connectedProfileIndex = 0;
}

bool WiFiManagerESP32::waitForConnection(int maxAttempts) {
  for (int i = 0; i < maxAttempts; i++) {
    boardDriver->showConnectingAnimation();
    wl_status_t st = WiFi.status();
    Serial.printf("  Attempt %d/%d - Status: %d\n", i + 1, maxAttempts, st);
    if (st == WL_CONNECTED) return true;
    if (i >= 3 && (st == WL_CONNECT_FAILED || st == WL_NO_SSID_AVAIL)) break;
  }
  return false;
}

bool WiFiManagerESP32::tryConnect(const String& ssid, const String& password, const uint8_t* bssid, uint8_t channel) {
  bool isFast = (bssid != nullptr && channel > 0);
  if (isFast)
    Serial.printf("  Fast connect: SSID=%s, Password=%s, Channel=%d, BSSID=%02X:%02X:%02X:%02X:%02X:%02X\n", ssid.c_str(), password.c_str(), channel, bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5]);
  else
    Serial.printf("  Standard connect: SSID=%s, Password=%s\n", ssid.c_str(), password.c_str());

  stopCaptivePortal();
  WiFi.disconnect(false, true);
  delay(100);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.setHostname("PristonChess");
  WiFi.mode(WIFI_STA);

  int maxAttempts = isFast ? 5 : 10;
  if (!isFast && scanAllChannels) {
    WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
    WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
    maxAttempts = 25;
  }

  if (isFast) {
    WiFi.begin(ssid.c_str(), password.c_str(), channel, bssid);
  } else {
    WiFi.begin(ssid.c_str(), password.c_str());
  }

  if (waitForConnection(maxAttempts)) {
    startMDNS();
    if (!otaChecked) {
      lastUpdateInfo = otaUpdater.checkForUpdate();
      otaChecked = true;
    }
    return true;
  }
  return false;
}

bool WiFiManagerESP32::tryConnectProfile(int index) {
  if (index < 0 || index >= profileCount) return false;
  WiFiProfile& p = profiles[index];

  // Try fast BSSID+channel connect first (skip if scanAllChannels is enabled)
  if (p.hasBssid && !scanAllChannels) {
    if (tryConnect(p.ssid, p.password, p.bssid, p.channel)) {
      Serial.println("  Fast connect succeeded!");
      connectedProfileIndex = index;
      return true;
    }
    Serial.println("  Fast connect failed, trying standard...");
  }

  // Standard connect (scans for the SSID)
  if (tryConnect(p.ssid, p.password)) {
    // Update cached BSSID and channel
    uint8_t* connBssid = WiFi.BSSID();
    if (connBssid && (!p.hasBssid || memcmp(p.bssid, connBssid, 6) != 0 || p.channel != WiFi.channel())) {
      memcpy(p.bssid, connBssid, 6);
      p.channel = WiFi.channel();
      p.hasBssid = true;
      Serial.printf("  Cached BSSID=%02X:%02X:%02X:%02X:%02X:%02X, Channel=%d\n", p.bssid[0], p.bssid[1], p.bssid[2], p.bssid[3], p.bssid[4], p.bssid[5], p.channel);
    }
    connectedProfileIndex = index;
    return true;
  }

  connectedProfileIndex = -1;
  Serial.printf("  Failed to connect to %s\n", p.ssid.c_str());
  return false;
}

bool WiFiManagerESP32::connectToSavedProfile() {
  for (int i = 0; i < profileCount; i++) {
    if (tryConnectProfile(i)) {
      promoteProfile(i);
      saveProfiles();
      return true;
    }
  }
  return false;
}

bool WiFiManagerESP32::startAPFallback() {
  Serial.println("Starting AP fallback...");
  WiFi.mode(WIFI_AP);
  if (!WiFi.softAPConfig(AP_IP, AP_GATEWAY, AP_SUBNET))
    Serial.println("ERROR: Failed to configure AP IP settings!");
  bool apOk = WiFi.softAP(AP_SSID, AP_PASSWORD);
  if (!apOk)
    Serial.println("ERROR: Failed to create Access Point!");
  startMDNS();
  connectedProfileIndex = -1;
  startCaptivePortal();
  pendingWiFi.action = SCAN_NETWORKS;
  return apOk;
}

void WiFiManagerESP32::startCaptivePortal() {
  if (dnsTaskHandle != nullptr) return;
  // Start DNS server for captive portal - resolves all domains to this AP's IP
  dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
  dnsServer.start(53, "*", WiFi.softAPIP());
  Serial.println("Captive portal DNS started, IP: " + WiFi.softAPIP().toString());
  xTaskCreate(dnsTask, "DNS_Task", 2048, this, 5, &dnsTaskHandle);
}

void WiFiManagerESP32::stopCaptivePortal() {
  if (dnsTaskHandle == nullptr) return;
  vTaskDelete(dnsTaskHandle);
  dnsTaskHandle = nullptr;
  dnsServer.stop();
  Serial.println("Captive portal DNS stopped");
}

void WiFiManagerESP32::dnsTask(void* param) {
  WiFiManagerESP32* manager = static_cast<WiFiManagerESP32*>(param);
  while (true) {
    manager->dnsServer.processNextRequest();
    vTaskDelay(pdMS_TO_TICKS(25));
  }
  vTaskDelete(nullptr);
}

void WiFiManagerESP32::pendingWiFiBackgroundTask(void* param) {
  WiFiManagerESP32* manager = static_cast<WiFiManagerESP32*>(param);
  while (true) {
    manager->checkPendingWiFi();
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  vTaskDelete(nullptr);
}

void WiFiManagerESP32::performScan() {
  static bool scanInProgress = false;
  if (scanInProgress) return;
  scanInProgress = true;
  Serial.println("Starting WiFi scan...");
  int n = WiFi.scanNetworks(false, false);
  Serial.printf("Scan found %d networks\n", n);
  if (n > 0) {
    if (scanResults) {
      delete[] scanResults;
      scanResults = nullptr;
    }
    scanResultCount = 0;
    scanResults = new ScannedNetwork[n];
    scanResultCount = n;
    for (int i = 0; i < n; i++) {
      scanResults[i].ssid = WiFi.SSID(i);
      scanResults[i].rssi = WiFi.RSSI(i);
      scanResults[i].channel = WiFi.channel(i);
      scanResults[i].encryptionType = (uint8_t)WiFi.encryptionType(i);
      uint8_t* bssid = WiFi.BSSID(i);
      if (bssid)
        memcpy(scanResults[i].bssid, bssid, 6);
      else
        memset(scanResults[i].bssid, 0, 6);
    }
  }
  WiFi.scanDelete();
  scanInProgress = false;
}