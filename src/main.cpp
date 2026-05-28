#include "board_driver.h"
#include "chess_bot.h"
#include "chess_engine.h"
#include "chess_learning.h"
#include "chess_lichess.h"
#include "chess_moves.h"
#include "chess_utils.h"
#include "clock_state.h"
#include "elo_fetcher.h"
#include "led_colors.h"
#include "move_history.h"
#include "ota_updater.h"
#include "profiles/profiles.h"
#include "sensor_test.h"
#include "simulation_mode.h"
#include "version.h"
#include "wifi_manager_esp32.h"
#include <LittleFS.h>
#include <time.h>

// ---------------------------
// Game State and Configuration
// ---------------------------

enum GameMode {
  MODE_SELECTION = 0,
  MODE_CHESS_MOVES = 1,
  MODE_BOT = 2,
  MODE_LICHESS = 3,
  MODE_SENSOR_TEST = 4,
  MODE_SIMULATION = 5,
  MODE_LEARNING = 6
};
// Forward-declared globals (definitions further down) — needed early so the
// helper functions below can reference currentMode without re-ordering.
extern GameMode currentMode;

BotConfig botConfig = {StockfishSettings::medium(), true};
LichessConfig lichessConfig = {""};

BoardDriver boardDriver;
ChessEngine chessEngine;
MoveHistory moveHistory;
WiFiManagerESP32 wifiManager(&boardDriver, &moveHistory);
ChessMoves* chessMoves = nullptr;
ChessBot* chessBot = nullptr;
ChessLichess* chessLichess = nullptr;
SensorTest* sensorTest = nullptr;
SimulationMode* simulation = nullptr;
ChessLearning* chessLearning = nullptr;

// Player profile registry — restored from the pre-OpenChess PristonChess
// codebase. Fixed roster (Benny / Rene / Flo / Philipp) with NVS-persisted
// stats. Used for Human-vs-Human stats + leaderboard on the e-paper idle
// screen. See src/profiles/profiles.cpp.
Profiles profiles;

// Human-vs-Human current-game player IDs (set when MODE_CHESS_MOVES starts
// via the web UI). Used at game-end to bump the right profile stats.
String hvhWhitePlayerId;
String hvhBlackPlayerId;
bool   hvhResultLogged = false;

// Rolling bot tip — set by ChessBot before it makes a Stockfish request,
// shown on the e-paper status line until the bot's move is applied.
static char _botHintBuf[40] = "";
static const char* const BOT_TIPS[] = {
  "Tipp: Springer entwickeln",
  "Tipp: Rochade nicht vergessen",
  "Tipp: Zentrum kontrollieren",
  "Tipp: Bauernkette stuetzen",
  "Tipp: Doppelter Angriff?",
  "Tipp: Koenig sicher stellen",
  "Tipp: Tuerme verbinden",
  "Tipp: Bauernzug ueberlegen",
  "Tipp: Aktive Figuren tauschen",
  "Tipp: Diagonalen nutzen",
};
static uint8_t _botTipIdx = 0;
void setBotHint(const char* h) {
  if (!h) { _botHintBuf[0] = '\0'; return; }
  strncpy(_botHintBuf, h, sizeof(_botHintBuf) - 1);
  _botHintBuf[sizeof(_botHintBuf) - 1] = '\0';
}
void setBotHintRotating() {
  setBotHint(BOT_TIPS[_botTipIdx]);
  _botTipIdx = (uint8_t)((_botTipIdx + 1) %
                         (sizeof(BOT_TIPS) / sizeof(BOT_TIPS[0])));
}
const char* displayGetBotHint() { return _botHintBuf; }

static void logHvHResultIfNeeded() {
  if (hvhResultLogged) return;
  if (chessMoves == nullptr) return;
  if (!chessMoves->isGameOver()) return;

  // Prefer the result that ChessGame itself recorded — covers checkmate,
  // stalemate, resign, draw-by-agreement, manual end, and the various
  // automatic draws.
  char winner = chessMoves->getWinnerColor();
  if (winner != 'w' && winner != 'b' && winner != 'd') {
    // Fallback: derive from engine state.
    const auto& board = chessMoves->getBoard();
    char turn = chessMoves->getCurrentTurn();
    winner = 'd';
    if (chessEngine.isCheckmate(board, turn)) {
      winner = (turn == 'w') ? 'b' : 'w';
    }
  }
  uint16_t plies = (uint16_t)(chessEngine.getFullmoveClock() * 2);
  if (!hvhWhitePlayerId.isEmpty() || !hvhBlackPlayerId.isEmpty()) {
    profiles.logHvHResult(hvhWhitePlayerId.c_str(),
                          hvhBlackPlayerId.c_str(),
                          winner, plies);
    Serial.printf("[HVH] Result logged — winner=%c white=%s black=%s plies=%u\n",
                  winner, hvhWhitePlayerId.c_str(),
                  hvhBlackPlayerId.c_str(), (unsigned)plies);
  }
  hvhResultLogged = true;
}

// Trigger a manual game-end with a chosen winner ('w' / 'b' / 'd') on
// whichever game subclass is currently active. Used by the flag-fall path
// below so the e-paper winner splash + stats logging fire on time loss too.
static void endActiveGame(char winner, char reason = 'M') {
  if (currentMode == MODE_CHESS_MOVES && chessMoves) chessMoves->endGameManually(winner, reason);
  else if (currentMode == MODE_BOT && chessBot)       chessBot->endGameManually(winner, reason);
  else if (currentMode == MODE_LICHESS && chessLichess) chessLichess->endGameManually(winner, reason);
}

// Chess clock — main.cpp owns the timestamps, board's active color drives
// which side ticks. Visible on the e-paper. Configured per game mode (e.g.
// sim sets a 5-minute time limit).
ClockState     chessClock;
unsigned long  chessClockLastMs = 0;

static void tickChessClock(char activeTurn) {
  unsigned long now = millis();
  uint32_t dt = (uint32_t)(now - chessClockLastMs);
  chessClockLastMs = now;
  if (!chessClock.ticking || (activeTurn != 'w' && activeTurn != 'b')) return;

  uint32_t& ms      = (activeTurn == 'w') ? chessClock.whiteMs      : chessClock.blackMs;
  bool&     flagged = (activeTurn == 'w') ? chessClock.whiteFlagged : chessClock.blackFlagged;
  bool flagJustFell = false;
  if (chessClock.timeLimitMs == 0) {
    ms += dt;
  } else if (!flagged) {
    if (ms <= dt) { ms = 0; flagged = true; flagJustFell = true; }
    else          { ms -= dt; }
  }
  // On flag-fall: end the game with the opposite color as winner so the
  // e-paper triggers its winner splash + stats get logged.
  if (flagJustFell) {
    if (currentMode == MODE_SIMULATION) {
      // Sim mode has no winner / loser — restart the clock and keep the
      // demo running instead of locking the e-paper into a flagged state.
      Serial.println("[CLOCK] sim clock looped (flag-fall ignored)");
      chessClock.resetTo(chessClock.timeLimitMs);
      return;
    }
    chessClock.ticking = false;
    char winner = (activeTurn == 'w') ? 'b' : 'w';
    Serial.printf("[CLOCK] %s ran out of time — %s wins\n",
                  activeTurn == 'w' ? "white" : "black",
                  winner == 'w' ? "white" : "black");
    endActiveGame(winner, /*reason=*/'T');
  }
}

GameMode currentMode = MODE_SELECTION;
bool modeInitialized = false;
bool resumingGame = false;
bool resetGameSelection = true;
std::atomic<bool>* idleRainbowStop = nullptr;

void showGameSelection();
void handleGameSelection();
void handleBotConfigSelection();
void initializeSelectedMode(GameMode mode);

void setup() {
  Serial.begin(115200);
  delay(3000);
  Serial.println();
  Serial.println("================================================");
  Serial.println("         OpenChess Starting Up");
  Serial.println("         Firmware version: " FIRMWARE_VERSION);
  Serial.println("================================================");
  if (!ChessUtils::ensureNvsInitialized())
    Serial.println("WARNING: NVS init failed (Preferences may not work)");
  if (!LittleFS.begin(true))
    Serial.println("ERROR: LittleFS mount failed!");
  else
    Serial.println("LittleFS mounted successfully");
  moveHistory.begin();
  profiles.begin();
  boardDriver.beginHardware();
  wifiManager.begin();

  // NOTE: boardDriver.checkCalibration() is intentionally NOT called at
  // boot any more. It blocks forever waiting for an "empty board" if the
  // Hall-sensor pins are floating (which they are until magnets are wired).
  // The idle e-paper screen + the web dashboard remain reachable, and
  // calibration is triggered on-demand from initializeSelectedMode() when
  // the user actually starts a sensor-dependent game mode.

#ifdef SIMULATION_MODE
  // Build-flag override: skip calibration (which blocks on "empty board"
  // sensor reads), skip live-game recovery, skip the menu — go straight into
  // the scripted display/engine test. Useful for verifying the e-paper
  // without hooking up sensors or shift registers.
  Serial.println("[SIM] SIMULATION_MODE build flag set — bypassing calibration + menu");
  currentMode = MODE_SIMULATION;
  // Pre-configure the chess clock to a simulated 5-minute time limit so the
  // e-paper has something interesting to render during playback.
  chessClock.resetTo(5UL * 60UL * 1000UL);
  chessClock.ticking = true;
  chessClockLastMs   = millis();
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  return;
#endif

  Serial.println();
  // Kick off NTP time sync (non-blocking, will resolve in background)
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");

  // Auto-resume is intentionally disabled — boot always lands on the idle
  // screen / game selection. Without real sensors a half-finished game
  // would otherwise lock the device in the chess mode. Any leftover live
  // game on flash gets discarded.
  if (moveHistory.hasLiveGame()) {
    Serial.println("[BOOT] discarding prior live game (auto-resume disabled)");
    moveHistory.discardLiveGame();
  }

  boardDriver.fillUpAnimation(LedColors::Green, 50);
  showGameSelection();
}

// Resolve the active game pointer based on the current mode. Used by
// loop() AND by displayForceTick() (called from ChessBot before it kicks
// off a blocking Stockfish request).
static ChessGame* _activeGameForCurrentMode() {
  switch (currentMode) {
    case MODE_CHESS_MOVES: return chessMoves;
    case MODE_BOT:         return chessBot;
    case MODE_LICHESS:     return chessLichess;
    case MODE_SIMULATION:  return simulation;
    case MODE_LEARNING:    return chessLearning;
    default:               return nullptr;
  }
}

void loop() {
  // Refresh the e-paper status display with the active game (if any). The
  // display has its own throttling + dirty-check, so calling every iteration
  // is cheap.
  ChessGame* activeGame = _activeGameForCurrentMode();

  char activeTurn = (activeGame != nullptr) ? activeGame->getCurrentTurn() : '?';
  tickChessClock(activeTurn);
  // Mirror the active mode into the WiFi manager so /board-update can tell
  // the web client whether a game is running (used by the "Kein Spiel läuft"
  // banner on board.html).
  wifiManager.setActiveGameMode(static_cast<int>(currentMode));

  // Push game status (turn, check, opening, clocks) to web UI
  {
    GameStatusData gs;
    if (activeGame && currentMode != MODE_SELECTION) {
      gs.turn        = activeGame->getCurrentTurn();
      gs.gameOver    = activeGame->isGameOver();
      gs.winnerColor = activeGame->getWinnerColor();
      gs.endReason   = activeGame->getEndReason();
      if (!gs.gameOver)
        gs.inCheck = chessEngine.isKingInCheck(activeGame->getBoard(), gs.turn);
      const char* op = activeGame->getOpeningName();
      if (op) strncpy(gs.opening, op, sizeof(gs.opening) - 1);
    }
    gs.clockWhiteMs = chessClock.whiteMs;
    gs.clockBlackMs = chessClock.blackMs;
    gs.clockLimitMs = chessClock.timeLimitMs;
    gs.clockTicking = chessClock.ticking;
    gs.whiteFlagged = chessClock.whiteFlagged;
    gs.blackFlagged = chessClock.blackFlagged;
    wifiManager.setGameStatus(gs);
  }

  // chess.com ELO refresh — spawned ONCE per boot as a dedicated FreeRTOS
  // task so the HTTPS handshakes don't block the main loop and trip
  // TG1WDT_SYS_RESET.
  {
    static bool chesscomTaskSpawned = false;
    if (!chesscomTaskSpawned && WiFi.isConnected() &&
        currentMode == MODE_SELECTION) {
      auto chesscomFetchTask = [](void*) {
        delay(2000); // let WiFi/web settle
        Serial.println("[CHESSCOM] background ELO fetch starting");
        EloFetcher::refreshAllProfiles();
        Serial.println("[CHESSCOM] background ELO fetch done");
        vTaskDelete(NULL);
      };
      xTaskCreate(chesscomFetchTask, "chesscom_fetch",
                  /*stack=*/24576, NULL, /*prio=*/1, NULL);
      chesscomTaskSpawned = true;
    }
  }

  // Check for pending board edits from WiFi (FEN-based)
  String editFen;
  if (wifiManager.getPendingBoardEdit(editFen)) {
    Serial.println("Applying board edit from WiFi interface...");

    if (currentMode == MODE_CHESS_MOVES && modeInitialized && chessMoves != nullptr) {
      chessMoves->setBoardStateFromFEN(editFen);
      Serial.println("Board edit applied to Chess Moves mode");
    } else if (currentMode == MODE_BOT && modeInitialized && chessBot != nullptr) {
      chessBot->setBoardStateFromFEN(editFen);
      Serial.println("Board edit applied to Chess Bot mode");
    } else if (currentMode == MODE_LICHESS && modeInitialized && chessLichess != nullptr) {
      chessLichess->setBoardStateFromFEN(editFen);
      Serial.println("Board edit applied to Lichess mode");
    } else if (currentMode == MODE_SIMULATION && simulation != nullptr) {
      simulation->setBoardStateFromFEN(editFen);
      simulation->renderBoardLEDs();
      Serial.println("Board edit applied to Simulation (manual) mode");
    } else {
      Serial.println("Warning: Board edit received but no active game mode");
    }

    wifiManager.clearPendingEdit();
  }

  // Manual-sim move highlight: light the picked-up square + its legal targets.
  String hlFrom, hlTargets;
  if (wifiManager.getPendingHighlight(hlFrom, hlTargets)) {
    if (currentMode == MODE_SIMULATION && simulation != nullptr)
      simulation->renderHighlights(hlFrom, hlTargets);
  }

  // Check for pending resign from WiFi
  char resignColor;
  if (wifiManager.getPendingResign(resignColor)) {
    Serial.printf("Processing resign from web UI: %c resigns\n", resignColor);
    if (currentMode == MODE_CHESS_MOVES && modeInitialized && chessMoves != nullptr) {
      chessMoves->resignGame(resignColor);
    } else if (currentMode == MODE_BOT && modeInitialized && chessBot != nullptr) {
      chessBot->resignGame(resignColor);
    } else if (currentMode == MODE_LICHESS && modeInitialized && chessLichess != nullptr) {
      chessLichess->resignGame(resignColor);
    } else {
      Serial.println("Warning: Resign received but no active game mode");
    }
    wifiManager.clearPendingResign();
  }

  // Check for pending draw from WiFi
  if (wifiManager.getPendingDraw()) {
    Serial.println("Processing draw from web UI");
    if (currentMode == MODE_CHESS_MOVES && modeInitialized && chessMoves != nullptr) {
      chessMoves->drawGame();
    } else if (currentMode == MODE_BOT && modeInitialized && chessBot != nullptr) {
      chessBot->drawGame();
    } else if (currentMode == MODE_LICHESS && modeInitialized && chessLichess != nullptr) {
      chessLichess->drawGame();
    } else {
      Serial.println("Warning: Draw received but no active game mode");
    }
    wifiManager.clearPendingDraw();
  }

  // Check for pending manual game end from WiFi (board.html "Spiel beenden")
  char manualWinner;
  if (wifiManager.getPendingManualEnd(manualWinner)) {
    Serial.printf("Processing manual end from web UI: winner=%c\n", manualWinner);
    if (currentMode == MODE_CHESS_MOVES && modeInitialized && chessMoves != nullptr) {
      chessMoves->endGameManually(manualWinner);
    } else if (currentMode == MODE_BOT && modeInitialized && chessBot != nullptr) {
      chessBot->endGameManually(manualWinner);
    } else if (currentMode == MODE_LICHESS && modeInitialized && chessLichess != nullptr) {
      chessLichess->endGameManually(manualWinner);
    } else if (currentMode == MODE_LEARNING && modeInitialized && chessLearning != nullptr) {
      chessLearning->endGameManually(manualWinner);
    } else if (currentMode == MODE_SIMULATION) {
      // A demo has no "winner" to record — treat manual end as an abort and
      // return to game selection (mode 5 otherwise self-restarts forever).
      Serial.println("Manual end during simulation — aborting demo, back to selection");
      showGameSelection();
    } else {
      Serial.println("Warning: Manual end received but no active game mode");
    }
  }

  // Check for WiFi game selection
  int selectedMode = wifiManager.getSelectedGameMode();
  if (selectedMode > 0) {
    Serial.printf("WiFi game selection detected: %d\n", selectedMode);
    switch (selectedMode) {
      case 1: {
        currentMode      = MODE_CHESS_MOVES;
        hvhWhitePlayerId = wifiManager.getHvHWhiteId();
        hvhBlackPlayerId = wifiManager.getHvHBlackId();
        hvhResultLogged  = false;
        // Wire the configured per-player time limit into the chess clock.
        // 0 means "no limit" (clock counts up rather than down).
        uint32_t limitMs = wifiManager.getHvHTimeLimitMs();
        chessClock.resetTo(limitMs);
        chessClock.ticking = true;
        chessClockLastMs   = millis();
        break;
      }
      case 2:
        currentMode = MODE_BOT;
        botConfig = wifiManager.getBotConfig();
        break;
      case 3:
        currentMode = MODE_LICHESS;
        lichessConfig = wifiManager.getLichessConfig();
        break;
      case 4:
        currentMode = MODE_SENSOR_TEST;
        break;
      case 5:
        // Simulation: doesn't require sensors. Trigger via web UI
        //   POST /gameselect?gamemode=5
        currentMode = MODE_SIMULATION;
        chessClock.resetTo(5UL * 60UL * 1000UL);
        chessClock.ticking = true;
        chessClockLastMs   = millis();
        break;
      case 6:
        // Learning: drilled openings. Clock counts up (no time pressure).
        currentMode = MODE_LEARNING;
        chessClock.resetTo(0);
        chessClock.ticking = false;
        chessClockLastMs   = millis();
        break;
      default:
        Serial.println("Invalid game mode selected via WiFi");
        selectedMode = 0;
        break;
    }
    if (selectedMode > 0) {
      modeInitialized = false;
      wifiManager.resetGameSelection();
      // Propagate the new mode to the web client immediately so the "no
      // game running" banner clears even if the upcoming initializeSelectedMode()
      // call takes a while (waitForBoardSetup etc.).
      wifiManager.setActiveGameMode(static_cast<int>(currentMode));
      if (idleRainbowStop) {
        idleRainbowStop->store(true);
        idleRainbowStop = nullptr;
      }
      boardDriver.clearAllLEDs();
    }
  }

  if (currentMode == MODE_SELECTION) {
    // Restart idle animation if the type or color was changed via web UI
    if (boardDriver.consumeAnimationRestartRequest()) {
      if (idleRainbowStop) {
        idleRainbowStop->store(true);
        idleRainbowStop = nullptr;
      }
      idleRainbowStop = boardDriver.startIdleAnimation();
    }
    handleGameSelection();
    return;
  }
  // Game mode selected
  if (!modeInitialized) {
    initializeSelectedMode(currentMode);
    modeInitialized = true;
    delay(1); // HACK: Ensure any starting animations acquire the LED mutex before proceeding
  }

  switch (currentMode) {
    case MODE_CHESS_MOVES:
      if (chessMoves != nullptr) {
        static unsigned long postGameUntilMs = 0;
        if (chessMoves->isGameOver()) {
          if (postGameUntilMs == 0) {
            // First tick after game-over: log stats and hold the mode for a
            // few seconds so the e-paper winner splash stays visible before
            // we bounce back to game selection.
            logHvHResultIfNeeded();
            postGameUntilMs = millis() + 7000;
          }
          if (millis() >= postGameUntilMs) {
            postGameUntilMs = 0;
            showGameSelection();
          }
        } else {
          postGameUntilMs = 0;
          chessMoves->update();
        }
      }
      break;
    case MODE_BOT:
      if (chessBot != nullptr) {
        if (chessBot->isGameOver())
          showGameSelection();
        else
          chessBot->update();
      }
      break;
    case MODE_LICHESS:
      if (chessLichess != nullptr) {
        if (chessLichess->isGameOver())
          showGameSelection();
        else
          chessLichess->update();
      }
      break;
    case MODE_SENSOR_TEST:
      if (sensorTest != nullptr) {
        if (sensorTest->isComplete())
          showGameSelection();
        else
          sensorTest->update();
      }
      break;
    case MODE_SIMULATION:
      // Self-restarting playback — never bounces back to game selection.
      if (simulation != nullptr) simulation->update();
      break;
    case MODE_LEARNING:
      if (chessLearning != nullptr) {
        static unsigned long learnPostMs = 0;
        if (chessLearning->isGameOver()) {
          if (learnPostMs == 0) {
            // Hold the final celebration / failure splash briefly before
            // returning to the menu.
            learnPostMs = millis() + 4500;
          }
          if (millis() >= learnPostMs) {
            learnPostMs = 0;
            showGameSelection();
          }
        } else {
          learnPostMs = 0;
          chessLearning->update();
        }
      }
      break;
    default:
      showGameSelection();
      break;
  }

  delay(SENSOR_READ_DELAY_MS);
}

void showGameSelection() {
  currentMode = MODE_SELECTION;
  modeInitialized = false;
  resetGameSelection = true;
  if (idleRainbowStop) {
    idleRainbowStop->store(true);
    idleRainbowStop = nullptr;
  }
  idleRainbowStop = boardDriver.startIdleAnimation();
  Serial.println("=============== Game Selection Mode ===============");
  Serial.println("Four LEDs are lit in the center of the board:");
  Serial.println("  Blue:   Chess Moves (Human vs Human)");
  Serial.println("  Green:  Chess Bot (Human vs AI)");
  Serial.println("  Yellow: Lichess (Play online games)");
  Serial.println("  Red:    Sensor Test");
  Serial.println("Place any chess piece on a LED to select that mode");
  Serial.println("===================================================");
}

void handleGameSelection() {
  boardDriver.readSensors();
  bool currState[4] = {boardDriver.getSensorState(1, 3), boardDriver.getSensorState(1, 4), boardDriver.getSensorState(2, 3), boardDriver.getSensorState(2, 4)};

  struct SelectorState {
    int emptyCount;
    int occupiedCount;
    bool readyForSelection;
  };
  const int DEBOUNCE_CYCLES = (DEBOUNCE_MS / SENSOR_READ_DELAY_MS) + 2;
  static SelectorState selectorStates[4] = {};

  if (resetGameSelection) {
    for (int i = 0; i < 4; ++i) {
      selectorStates[i].emptyCount = 0;
      selectorStates[i].occupiedCount = 0;
      selectorStates[i].readyForSelection = false;
    }
    resetGameSelection = false;
  }
  for (int i = 0; i < 4; ++i) {
    if (!currState[i]) {
      if (selectorStates[i].emptyCount < DEBOUNCE_CYCLES)
        selectorStates[i].emptyCount++;
      selectorStates[i].occupiedCount = 0;
      if (selectorStates[i].emptyCount >= DEBOUNCE_CYCLES)
        selectorStates[i].readyForSelection = true;
    } else {
      selectorStates[i].emptyCount = 0;
      if (selectorStates[i].readyForSelection) {
        if (selectorStates[i].occupiedCount < DEBOUNCE_CYCLES)
          selectorStates[i].occupiedCount++;
      } else {
        selectorStates[i].occupiedCount = 0;
      }
    }
  }

  // Check for valid rising edge (empty for DEBOUNCE_CYCLES, then occupied for DEBOUNCE_CYCLES)
  for (int i = 0; i < 4; ++i) {
    if (selectorStates[i].readyForSelection && selectorStates[i].occupiedCount >= DEBOUNCE_CYCLES) {
      switch (i) {
        case 0:
          Serial.println("Mode: 'Chess Moves' selected!");
          currentMode = MODE_CHESS_MOVES;
          modeInitialized = false;
          boardDriver.clearAllLEDs();
          break;
        case 1:
          Serial.println("Mode: 'Chess Bot' Selected! Showing bot configuration...");
          currentMode = MODE_BOT;
          modeInitialized = false;
          boardDriver.clearAllLEDs();
          handleBotConfigSelection();
          break;
        case 2:
          Serial.println("Mode: 'Lichess' Selected!");
          currentMode = MODE_LICHESS;
          modeInitialized = false;
          boardDriver.clearAllLEDs();
          lichessConfig = wifiManager.getLichessConfig();
          break;
        case 3:
          Serial.println("Mode: 'Sensor Test' Selected!");
          currentMode = MODE_SENSOR_TEST;
          modeInitialized = false;
          boardDriver.clearAllLEDs();
          break;
      }
      break;
    }
  }

  delay(SENSOR_READ_DELAY_MS);
}

void initializeSelectedMode(GameMode mode) {
  if (idleRainbowStop) {
    idleRainbowStop->store(true);
    idleRainbowStop = nullptr;
  }
  if (resumingGame)
    resumingGame = false;
  else
    moveHistory.discardLiveGame(); // Discard any incomplete live game that wasn't properly finished or resumed (finishGame already removes live files for completed games)

  // Sensor-dependent modes need a calibrated row/column mapping. Simulation
  // mode runs on synthetic input so no calibration is required (and would
  // block forever on the "empty board" check without real hardware).
#ifndef SKIP_CALIBRATION
  if (mode != MODE_SIMULATION) {
    boardDriver.checkCalibration();
  }
#else
  if (mode != MODE_SIMULATION) {
    Serial.println("[SKIP_CALIBRATION] sensor mode entered without calibration "
                   "— set up real Hall sensors then remove the build flag");
  }
#endif

  switch (mode) {
    case MODE_CHESS_MOVES:
      Serial.println("Starting 'Chess Moves'...");
      if (chessMoves != nullptr)
        delete chessMoves;
      chessMoves = new ChessMoves(&boardDriver, &chessEngine, &wifiManager, &moveHistory);
      chessMoves->begin();
      break;
    case MODE_BOT:
      Serial.printf("Starting 'Chess Bot' (Depth: %d, Player is %s)...\n", botConfig.stockfishSettings.depth, botConfig.playerIsWhite ? "White" : "Black");
      if (chessBot != nullptr)
        delete chessBot;
      chessBot = new ChessBot(&boardDriver, &chessEngine, &wifiManager, &moveHistory, botConfig);
      chessBot->begin();
      break;
    case MODE_LICHESS:
      Serial.println("Starting 'Lichess Mode'...");
      if (chessLichess != nullptr)
        delete chessLichess;
      chessLichess = new ChessLichess(&boardDriver, &chessEngine, &wifiManager, lichessConfig);
      chessLichess->begin();
      break;
    case MODE_SENSOR_TEST:
      Serial.println("Starting 'Sensor Test'...");
      if (sensorTest != nullptr)
        delete sensorTest;
      sensorTest = new SensorTest(&boardDriver);
      sensorTest->begin();
      break;
    case MODE_SIMULATION:
      Serial.println("Starting 'Simulation Mode'...");
      if (simulation != nullptr)
        delete simulation;
      simulation = new SimulationMode(&boardDriver, &chessEngine, &wifiManager, &moveHistory);
      simulation->setManual(wifiManager.getSimManual());
      simulation->begin();
      break;
    case MODE_LEARNING: {
      Serial.println("Starting 'Learning Mode'...");
      if (chessLearning != nullptr)
        delete chessLearning;
      chessLearning = new ChessLearning(&boardDriver, &chessEngine, &wifiManager, /*moveHistory=*/nullptr);
      int      idx   = wifiManager.getLearnOpeningIdx();
      int      level = wifiManager.getLearnLevel();
      char     color = wifiManager.getLearnPlayerColor();
      chessLearning->configure(idx, level, color);
      wifiManager.bindLearningInstance(chessLearning);
      chessLearning->begin();
      break;
    }
    default:
      showGameSelection();
      break;
  }
}

void handleBotConfigSelection() {
  Serial.println("====== Bot Configuration Selection ======");
  Serial.println("Select Bot Color:");
  Serial.println("- Rank 6: Bot is Black");
  Serial.println("- Rank 3: Bot is White");
  Serial.println("Select Difficulty:");
  Serial.println("- File B: Easy");
  Serial.println("- File D: Medium");
  Serial.println("- File F: Hard");
  Serial.println("- File H: Expert");
  Serial.println("Example: Place piece at Rank 3, File D = White Bot Medium");

  boardDriver.acquireLEDs();
  // Easy (col 1) - Green
  boardDriver.setSquareLED(0, 1, LedColors::Green);
  boardDriver.setSquareLED(3, 1, LedColors::Green);

  // Medium (col 3) - Orange/Gold
  boardDriver.setSquareLED(0, 3, LedColors::Yellow);
  boardDriver.setSquareLED(3, 3, LedColors::Yellow);

  // Hard (col 5) - Red
  boardDriver.setSquareLED(0, 5, LedColors::Red);
  boardDriver.setSquareLED(3, 5, LedColors::Red);

  // Expert (col 7) - Purple
  boardDriver.setSquareLED(0, 7, LedColors::Purple);
  boardDriver.setSquareLED(3, 7, LedColors::Purple);

  boardDriver.showLEDs();
  boardDriver.releaseLEDs();

  // Wait for selection
  Serial.println("Waiting for bot configuration selection...");

  static bool prevState[2][8] = {};
  bool firstLoop = true;

  while (true) {
    boardDriver.readSensors();

    for (int rowIdx = 0; rowIdx < 2; ++rowIdx) {
      int row = (rowIdx == 0) ? 0 : 3;
      for (int col : {1, 3, 5, 7}) {
        bool curr = boardDriver.getSensorState(row, col);
        // Only accept selection if square was previously empty and is now occupied
        if (!firstLoop && !prevState[rowIdx][col] && curr) {
          botConfig.playerIsWhite = (row == 2);
          const char* colorName = botConfig.playerIsWhite ? "White" : "Black";
          if (col == 1) {
            botConfig.stockfishSettings = StockfishSettings::easy();
            Serial.printf("Configuration: Play as %s, Easy difficulty\n", colorName);
          } else if (col == 3) {
            botConfig.stockfishSettings = StockfishSettings::medium();
            Serial.printf("Configuration: Play as %s, Medium difficulty\n", colorName);
          } else if (col == 5) {
            botConfig.stockfishSettings = StockfishSettings::hard();
            Serial.printf("Configuration: Play as %s, Hard difficulty\n", colorName);
          } else if (col == 7) {
            botConfig.stockfishSettings = StockfishSettings::expert();
            Serial.printf("Configuration: Play as %s, Expert difficulty\n", colorName);
          }
          firstLoop = true;
          boardDriver.clearAllLEDs();
          return;
        }
        prevState[rowIdx][col] = curr;
      }
    }

    firstLoop = false;
    delay(SENSOR_READ_DELAY_MS);
  }
}