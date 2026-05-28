#ifndef WIFI_MANAGER_ESP32_H
#define WIFI_MANAGER_ESP32_H

#include "board_driver.h"
#include "ota_updater.h"
#include "stockfish_settings.h"
#include <Arduino.h>
#include <AsyncTCP.h>
#include <DNSServer.h>
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <WiFi.h>

// Forward declarations
struct LichessConfig;
class MoveHistory;
class ChessLearning;

// ---------------------------
// WiFi Configuration
// ---------------------------
#define AP_SSID "PristonChess"
#define AP_PASSWORD "chess123"
#define HTTP_PORT 80
#define MDNS_HOSTNAME "pristonchess"
#define MAX_WIFI_PROFILES 3

struct GameStatusData {
  char     turn         = '?';
  bool     gameOver     = false;
  char     winnerColor  = '?';
  char     endReason    = ' ';
  bool     inCheck      = false;
  char     opening[24]  = "";
  uint32_t clockWhiteMs = 0;
  uint32_t clockBlackMs = 0;
  uint32_t clockLimitMs = 0;
  bool     clockTicking = false;
  bool     whiteFlagged = false;
  bool     blackFlagged = false;
};
#define NVS_WIFI_NAMESPACE "wifiProfiles"

// ---------------------------
// WiFi Profile (saved network)
// ---------------------------
struct WiFiProfile {
  String ssid;
  String password;
  uint8_t bssid[6]; // MAC address for fast reconnect
  uint8_t channel;  // Channel for fast reconnect
  bool hasBssid;    // Whether bssid/channel are valid cached values

  WiFiProfile() : channel(0), hasBssid(false) { memset(bssid, 0, sizeof(bssid)); }
};

// ---------------------------
// Scanned network info (sent to frontend)
// ---------------------------
struct ScannedNetwork {
  String ssid;
  int32_t rssi;
  uint8_t bssid[6];
  uint8_t channel;
  uint8_t encryptionType; // wifi_auth_mode_t cast to uint8_t
};

// ---------------------------
// WiFi Manager Class for ESP32
// ---------------------------
class WiFiManagerESP32 {
 private:
  AsyncWebServer server;
  AsyncEventSource _events{"/events"};  // SSE push of /board-update JSON to clients
  uint32_t _lastEventPush = 0;
  void pushStateToClients();            // throttled broadcast to connected SSE clients
  DNSServer dnsServer;

  TaskHandle_t pendingWiFiTaskHandle = nullptr;
  static void pendingWiFiBackgroundTask(void* param);
  TaskHandle_t dnsTaskHandle = nullptr;
  static void dnsTask(void* param);
  void startCaptivePortal();
  void stopCaptivePortal();

  Preferences prefs;
  String gameMode;
  int    activeGameMode = 0;
  bool   simManualMode  = false;  // mode 5 sub-mode: manual (web-driven) vs auto
  String lichessToken;

  // Saved WiFi profiles (up to MAX_WIFI_PROFILES, index 0 = most recently connected)
  WiFiProfile profiles[MAX_WIFI_PROFILES];
  int profileCount;
  bool scanAllChannels;
  int connectedProfileIndex; // Index of currently connected profile, or -1

  // Scan results (populated by deferred scan task)
  ScannedNetwork* scanResults;
  int scanResultCount;

  BotConfig botConfig = {StockfishSettings::medium(), true};

  // Human-vs-Human player selection (which profile plays which color). Set
  // by the web UI when mode=1 is selected. timeLimit is the per-player
  // countdown in ms (0 = no time limit / count up).
  String   hvhWhiteId;
  String   hvhBlackId;
  uint32_t hvhTimeLimitMs = 10UL * 60UL * 1000UL;

  // Learning-mode configuration. Set by the web UI when mode=6 is selected
  // (POST /gameselect with gamemode=6&openingIdx=N&level=L[&color=w/b]).
  // bindLearningInstance() is called by main.cpp after constructing the
  // ChessLearning so the web handler can issue restart requests.
  int            learnOpeningIdx = 0;
  int            learnLevel = 1;
  char           learnPlayerColor = ' ';  // ' ' = use opening default
  ChessLearning* activeLearning = nullptr;

  MoveHistory* moveHistory;
  BoardDriver* boardDriver;
  String currentFen;
  float boardEvaluation;

  // Board edit storage (pending edits from web interface)
  String pendingFenEdit;
  volatile bool hasPendingEdit;

  // Pending move highlight from manual sim (squares to light on the LEDs)
  String pendingHlFrom;
  String pendingHlTargets;
  volatile bool hasPendingHighlight = false;

  // Pending resign/draw/manual-end from web interface
  volatile bool hasPendingResign;
  volatile bool hasPendingDraw;
  volatile bool hasPendingManualEnd = false;
  char pendingResignColor;        // 'w' or 'b' — the side resigning
  char pendingManualWinner = '?'; // 'w' / 'b' / 'd' for manual game end

  // Promotion state for web-based piece selection
  struct PromotionState {
    volatile bool pending; // True while waiting for web client to choose a piece
    volatile char choice;  // Piece chosen by web client ('q','r','b','n') or ' ' if none yet
    char color;            // 'w' or 'b' — color of the promoting pawn
    void reset() {
      pending = false;
      choice = ' ';
      color = ' ';
    }
  };
  PromotionState promotion;

  // Game status (updated by main.cpp every loop, included in /board-update)
  GameStatusData _gameStatus;

  // Web client heartbeat (tracks whether board.html is actively polling)
  unsigned long lastBoardPollTime; // millis() of last /board-update GET request

  // Deferred WiFi actions (set by web handler, processed by worker task)
  enum PendingAction {
    NONE,
    CONNECT_SAVED,
    CONNECT_NEW,
    DELETE_PROFILE,
    SCAN_NETWORKS
  };
  struct PendingWiFi {
    volatile PendingAction action;
    int profileIndex;    // For CONNECT_SAVED / DELETE_PROFILE
    String newSSID;      // For CONNECT_NEW
    String newPassword;  // For CONNECT_NEW
    uint8_t newBssid[6]; // For CONNECT_NEW (from scan)
    uint8_t newChannel;  // For CONNECT_NEW (from scan)
    void reset() {
      action = NONE;
      profileIndex = -1;
      newSSID = "";
      newPassword = "";
      memset(newBssid, 0, 6);
      newChannel = 0;
    }
  };
  PendingWiFi pendingWiFi;

  // NVS persistence for WiFi profiles
  void loadProfiles();
  void saveProfiles(bool saveAllProfiles = true);
  void promoteProfile(int index);

  // WiFi connection helpers
  bool waitForConnection(int maxAttempts);
  bool tryConnect(const String& ssid, const String& password, const uint8_t* bssid = nullptr, uint8_t channel = 0);
  bool tryConnectProfile(int index);
  bool connectToSavedProfile(); // Try all saved profiles, promote winner; returns true if connected
  void startAPFallback();
  void performScan();

  // Web interface methods
  String getWiFiInfoJSON();
  String getBoardUpdateJSON();
  String getLichessInfoJSON();
  String getBoardSettingsJSON();
  void handleBoardEditSuccess(AsyncWebServerRequest* request);
  void handleSimHighlight(AsyncWebServerRequest* request);
  void handleHighlightColors(AsyncWebServerRequest* request);
  void handlePromotion(AsyncWebServerRequest* request);
  void handleConnectWiFi(AsyncWebServerRequest* request);
  void handleGameSelection(AsyncWebServerRequest* request);
  void handleSaveLichessToken(AsyncWebServerRequest* request);
  void handleBoardSettings(AsyncWebServerRequest* request);
  void handleBoardCalibration(AsyncWebServerRequest* request);
  void handleResign(AsyncWebServerRequest* request);
  void handleDraw(AsyncWebServerRequest* request);
  void getHardwareConfigJSON(AsyncWebServerRequest* request);
  void handleHardwareConfig(AsyncWebServerRequest* request);
  void handleDebugState(AsyncWebServerRequest* request);
  void handleDebugTestLeds(AsyncWebServerRequest* request);
  void handleGamesRequest(AsyncWebServerRequest* request);
  void handleDeleteGame(AsyncWebServerRequest* request);
  // OTA update handlers
  void handleOtaStatus(AsyncWebServerRequest* request);
  void handleOtaSettings(AsyncWebServerRequest* request);
  void handleOtaApply(AsyncWebServerRequest* request);
  // ESPAsyncWebServer body handlers receive data in chunks via callbacks (data, len, index, total),
  // not as a continuous Stream. This means we can't reuse the Stream-based OtaUpdater methods directly.
  // For firmware: we call Update.begin/write/end incrementally across chunks.
  // For web assets: we buffer the TAR to a temp file first, then pass it as a Stream to the TAR parser
  // (the TAR format requires sequential 512-byte header reads that can't be split across async chunks).
  void onFirmwareUploadBody(AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total);
  void onWebAssetsUploadBody(AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total);
  void startMDNS();

  // OTA state
  OtaUpdater otaUpdater;
  OtaUpdateInfo lastUpdateInfo;
  bool otaChecked;
  bool autoOtaEnabled;
  // Temporary file for web asset TAR upload (needed because the TAR parser requires a seekable Stream)
  File otaTarFile;

 public:
  WiFiManagerESP32(BoardDriver* boardDriver, MoveHistory* moveHistory);
  void begin();

  // OTA update support
  OtaUpdater& getOtaUpdater() { return otaUpdater; }
  bool isAutoOtaEnabled() const { return autoOtaEnabled; }

  // Game selection via web
  int getSelectedGameMode() const { return gameMode.toInt(); }
  void resetGameSelection() { gameMode = "0"; };
  // Currently active game mode (mirrored from main.cpp). Used by
  // /board-update so the web client can tell whether a game is running.
  void setActiveGameMode(int mode) { activeGameMode = mode; }
  bool getSimManual() const { return simManualMode; }
  void setGameStatus(const GameStatusData& gs) { _gameStatus = gs; pushStateToClients(); }
  // Bot configuration
  BotConfig getBotConfig() { return botConfig; }
  // Human-vs-Human player profile IDs (white / black) for stats logging.
  String   getHvHWhiteId() const { return hvhWhiteId; }
  String   getHvHBlackId() const { return hvhBlackId; }
  uint32_t getHvHTimeLimitMs() const { return hvhTimeLimitMs; }
  // Learning-mode configuration (filled by /gameselect, consumed by main.cpp).
  int  getLearnOpeningIdx()  const { return learnOpeningIdx; }
  int  getLearnLevel()       const { return learnLevel; }
  char getLearnPlayerColor() const { return learnPlayerColor; }
  void bindLearningInstance(ChessLearning* l) { activeLearning = l; }
  // Lichess configuration
  LichessConfig getLichessConfig();
  String getLichessToken() { return lichessToken; }
  // Board state management (FEN-based)
  void updateBoardState(const String& fen, float evaluation = 0.0f);
  String getCurrentFen() const { return currentFen; }
  float getEvaluation() const { return boardEvaluation; }
  // Board edit management (FEN-based)
  bool getPendingBoardEdit(String& fenOut);
  void clearPendingEdit();
  bool getPendingHighlight(String& from, String& targets);
  // Resign/Draw management (from web interface)
  bool getPendingResign(char& resignColor);
  bool getPendingDraw();
  bool getPendingManualEnd(char& winner);
  void clearPendingResign();
  void clearPendingDraw();
  // Promotion management (from web interface)
  void startPromotionWait(char color);
  bool isPromotionPending() const { return promotion.pending; }
  char getPromotionChoice() const { return promotion.choice; }
  void clearPromotion();
  // Web client connection check
  bool isWebClientConnected() const;
  // Check if WiFi is connected (re-attempts if not)
  bool ensureConnected();
  // Processes deferred WiFi actions (called by pendingWiFiBackgroundTask)
  void checkPendingWiFi();
};

#endif // WIFI_MANAGER_ESP32_H
