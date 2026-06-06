#include "simulation_mode.h"
#include "chess_utils.h"
#include "clock_state.h"
#include "wifi_manager_esp32.h"

// Owned by main.cpp.
extern ClockState    chessClock;
extern unsigned long chessClockLastMs;

// Library of short scripted games. The sim cycles through them at random
// so each playback is a different mating pattern. All coordinates are
// (row, col) with row 0 = rank 8 (black home) and col 0 = file a.

// 1. Fool's Mate — 1.f3 e5 2.g4 Qh4#
static const SimulationMode::ScriptedMove _SCRIPT_FOOLS[] = {
    {6, 5, 5, 5, ' '}, {1, 4, 3, 4, ' '},
    {6, 6, 4, 6, ' '}, {0, 3, 4, 7, ' '},
};

// 2. Scholar's Mate — 1.e4 e5 2.Bc4 Nc6 3.Qh5 Nf6?? 4.Qxf7#
static const SimulationMode::ScriptedMove _SCRIPT_SCHOLAR[] = {
    {6, 4, 4, 4, ' '}, {1, 4, 3, 4, ' '},
    {7, 5, 4, 2, ' '}, {0, 1, 2, 2, ' '},
    {7, 3, 3, 7, ' '}, {0, 6, 2, 5, ' '},
    {3, 7, 1, 5, ' '},                       // Qxf7#
};

// 3. Légal-style mini — 1.e4 e5 2.Nf3 Nc6 3.Bc4 d6 4.Nc3 Bg4 5.Nxe5! Bxd1??
//    6.Bxf7+ Ke7 7.Nd5#
static const SimulationMode::ScriptedMove _SCRIPT_LEGAL[] = {
    {6, 4, 4, 4, ' '}, {1, 4, 3, 4, ' '},
    {7, 6, 5, 5, ' '}, {0, 1, 2, 2, ' '},
    {7, 5, 4, 2, ' '}, {1, 3, 2, 3, ' '},
    {7, 1, 5, 2, ' '}, {0, 2, 4, 6, ' '},
    {5, 5, 3, 4, ' '}, {4, 6, 7, 3, ' '},   // Bxd1??
    {4, 2, 1, 5, ' '}, {0, 4, 1, 4, ' '},   // Bxf7+ Ke7
    {5, 2, 3, 3, ' '},                       // Nd5#
};

// 4. Quick g6 trap — 1.e4 g6 2.d4 Bg7 3.Bc4 b6?? 4.Qh5 (threat) Nc6 5.Qxf7#
static const SimulationMode::ScriptedMove _SCRIPT_GPRUNE[] = {
    {6, 4, 4, 4, ' '}, {1, 6, 2, 6, ' '},
    {6, 3, 4, 3, ' '}, {0, 5, 1, 6, ' '},
    {7, 5, 4, 2, ' '}, {1, 1, 2, 1, ' '},
    {7, 3, 3, 7, ' '}, {0, 1, 2, 2, ' '},
    {3, 7, 1, 5, ' '},                       // Qxf7#
};

const SimulationMode::ScriptedGame SimulationMode::SCRIPTS[] = {
    {_SCRIPT_FOOLS,   sizeof(_SCRIPT_FOOLS)   / sizeof(_SCRIPT_FOOLS[0]),   "Narrenmatt"},
    {_SCRIPT_SCHOLAR, sizeof(_SCRIPT_SCHOLAR) / sizeof(_SCRIPT_SCHOLAR[0]), "Schaeferzug"},
    {_SCRIPT_LEGAL,   sizeof(_SCRIPT_LEGAL)   / sizeof(_SCRIPT_LEGAL[0]),   "Légal"},
    {_SCRIPT_GPRUNE,  sizeof(_SCRIPT_GPRUNE)  / sizeof(_SCRIPT_GPRUNE[0]),  "g6-Falle"},
};
const size_t SimulationMode::SCRIPT_COUNT =
    sizeof(SCRIPTS) / sizeof(SCRIPTS[0]);

SimulationMode::SimulationMode(BoardDriver* bd, ChessEngine* ce, WiFiManagerESP32* wm, MoveHistory* mh)
    : ChessGame(bd, ce, wm, mh), current(nullptr), step(0), lastTickMs(0),
      finished(false), seed(0) {}

void SimulationMode::begin() {
  // SIM forces a fixed blue/green player palette for every per-player
  // animation (pickup walk, post-move trail, mate fill, cell pulse). Clears
  // when the user leaves sim — done from main.cpp's initializeSelectedMode.
  boardDriver->setSimColorsActive(true);
  if (manual) {
    // Interactive sandbox: stand the pieces up in the start position and let
    // the web client drive every move. No scripted playback, no clock pressure.
    Serial.println("[SIM] manual mode — interactive board, no scripted playback");
    initializeBoard();
    replaying          = true;     // suppress physical-move waits / LED move anims
    current            = nullptr;  // no script → update() idles
    finished           = true;
    step               = 0;
    chessClock.resetTo(5UL * 60UL * 1000UL);
    chessClock.ticking = false;
    chessClockLastMs   = millis();
    wifiManager->updateBoardState(
        ChessUtils::boardToFEN(board, currentTurn, chessEngine),
        ChessUtils::evaluatePosition(board));
    renderBoardLEDs();
    return;
  }

  // Rotate through the script library — randomized so we don't always play
  // the same mating pattern. Seed mixes in millis() for variety across boots.
  seed = seed * 1664525u + 1013904223u + (uint32_t)millis();
  size_t pick = seed % SCRIPT_COUNT;
  // Avoid replaying the same script twice in a row.
  if (current != nullptr && SCRIPTS[pick].moves == current->moves) {
    pick = (pick + 1) % SCRIPT_COUNT;
  }
  current = &SCRIPTS[pick];
  Serial.printf("[SIM] Starting scripted playback: %s (%u moves)\n",
                current->name, (unsigned)current->length);

  initializeBoard();
  replaying  = true; // suppresses physical-move waits + LED animations inside applyMove
  step       = 0;
  finished   = false;
  lastTickMs = millis();
  // Reset the shared chess clock so each playback starts from full time.
  chessClock.resetTo(5UL * 60UL * 1000UL);
  chessClock.ticking = true;
  chessClockLastMs   = millis();
  wifiManager->updateBoardState(
      ChessUtils::boardToFEN(board, currentTurn, chessEngine),
      ChessUtils::evaluatePosition(board));
  renderBoardLEDs();
}

void SimulationMode::update() {
  unsigned long now = millis();
  if (current == nullptr) return;

  if (!finished) {
    if (now - lastTickMs < MOVE_INTERVAL_MS) return;
    lastTickMs = now;

    const ScriptedMove& m = current->moves[step];
    Serial.printf("[SIM] %s step %u: %c%d -> %c%d\n",
                  current->name, (unsigned)step,
                  (char)('a' + m.fromCol), 8 - m.fromRow,
                  (char)('a' + m.toCol),   8 - m.toRow);

    applyMove(m.fromRow, m.fromCol, m.toRow, m.toCol, m.promotion, /*isRemoteMove=*/false);
    // updateGameStatus advances the turn AND runs the end-of-game checks
    // (checkmate, stalemate, draw rules). Calling it instead of advanceTurn
    // lets the simulation trigger the same checkmate-spiral / check-border /
    // capture animations a real game would — needed so the user can test
    // those visuals without setting up a full physical game.
    updateGameStatus();
    // Mirror the new position to the LEDs first — before the (potentially slower)
    // eval + FEN build — so the physical board keeps pace with the web view.
    renderBoardLEDs();
    wifiManager->updateBoardState(
        ChessUtils::boardToFEN(board, currentTurn, chessEngine),
        ChessUtils::evaluatePosition(board));

    ++step;
    if (step >= current->length) {
      Serial.printf("[SIM] %s done — holding final position for showcase\n",
                    current->name);
      finished = true;
    }
    return;
  }

  // Showcase phase: hold the final (checkmate) state for a while, then
  // pick a new variation and play again.
  if (now - lastTickMs >= RESTART_DELAY_MS) {
    Serial.println("[SIM] restarting playback (rotating script)");
    begin();
  }
}

void SimulationMode::renderBoardLEDs() {
  boardDriver->acquireLEDs();
  for (int row = 0; row < 8; row++) {
    for (int col = 0; col < 8; col++) {
      char p = board[row][col];
      if (p == ' ' || p == '\0')
        boardDriver->setSquareLED(row, col, LedColors::Off);
      else
        boardDriver->setSquareLED(
            row, col, getPlayerLedColor(ChessUtils::isWhitePiece(p) ? 'w' : 'b'));
    }
  }
  boardDriver->showLEDs();
  boardDriver->releaseLEDs();
}

// Parse algebraic square ("e2") → board (row,col) with row 0 = rank 8.
static bool simParseSquare(const String& s, int& row, int& col) {
  if (s.length() < 2) return false;
  char f = s.charAt(0), r = s.charAt(1);
  if (f < 'a' || f > 'h' || r < '1' || r > '8') return false;
  col = f - 'a';
  row = 8 - (r - '0');
  return true;
}

void SimulationMode::renderHighlights(const String& fromSq, const String& targetsCsv) {
  // Signal any in-flight pickup highlight (initial walk OR replay loop) to
  // exit BEFORE we touch the LED mutex. Without this, acquireLEDs() below
  // blocks until the replay's own queue-check fires — and the replay won't
  // see its abort condition until the queue gets a new job, which we can't
  // queue until we hold the mutex. Classic deadlock.
  boardDriver->abortPickupHighlight();
  // First, repaint the static piece base — this is what the multi-path
  // animation will snapshot and overlay its cyan trails onto.
  boardDriver->acquireLEDs();
  for (int row = 0; row < 8; row++)
    for (int col = 0; col < 8; col++) {
      char p = board[row][col];
      boardDriver->setSquareLED(row, col,
          (p == ' ' || p == '\0') ? LedColors::Off
                                  : getPlayerLedColor(ChessUtils::isWhitePiece(p) ? 'w' : 'b'));
    }
  boardDriver->showLEDs();
  boardDriver->releaseLEDs();

  // Empty source = "clear pickup" → just leave the base picture; no animation.
  int srcR, srcC;
  if (!simParseSquare(fromSq, srcR, srcC)) return;

  // Parse the comma-separated target list and record the capture flag per
  // target (a piece sitting on the destination = capture).
  uint8_t tR[28], tC[28], tCap[28];
  int n = 0;
  int start = 0;
  while (targetsCsv.length() > 0 && start < (int)targetsCsv.length() && n < 28) {
    int comma = targetsCsv.indexOf(',', start);
    String tok = (comma < 0) ? targetsCsv.substring(start) : targetsCsv.substring(start, comma);
    tok.trim();
    int r, c;
    if (simParseSquare(tok, r, c)) {
      tR[n] = (uint8_t)r;
      tC[n] = (uint8_t)c;
      tCap[n] = (board[r][c] != ' ' && board[r][c] != '\0') ? 1 : 0;
      n++;
    }
    if (comma < 0) break;
    start = comma + 1;
  }

  if (n > 0) {
    // Per-player styling: pick the side of the piece on the source square.
    char piece = board[srcR][srcC];
    char player = ChessUtils::isWhitePiece(piece) ? 'w' : 'b';
    boardDriver->multiPathHighlight(srcR, srcC, tR, tC, tCap, n, player);
  } else {
    // No targets (e.g. blocked piece) — just show the source highlight on top
    // of the base picture so the user gets visual feedback for their click.
    boardDriver->acquireLEDs();
    boardDriver->setSquareLED(srcR, srcC, boardDriver->hlSourceColor());
    boardDriver->showLEDs();
    boardDriver->releaseLEDs();
  }
}
