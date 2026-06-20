#include "chess_game.h"
#include "chess_utils.h"
#include "move_history.h"
#include "openings.h"
#include "wifi_manager_esp32.h"
#include <string.h>
#include "serial_tee.h"  // must be last: redefines Serial -> tee

const char ChessGame::INITIAL_BOARD[8][8] = {
    {'r', 'n', 'b', 'q', 'k', 'b', 'n', 'r'}, // row 0 = rank 8 (Black pieces, top row)
    {'p', 'p', 'p', 'p', 'p', 'p', 'p', 'p'}, // row 1 = rank 7 (Black pawns)
    {' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '}, // row 2 = rank 6
    {' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '}, // row 3 = rank 5
    {' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '}, // row 4 = rank 4
    {' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '}, // row 5 = rank 3
    {'P', 'P', 'P', 'P', 'P', 'P', 'P', 'P'}, // row 6 = rank 2 (White pawns)
    {'R', 'N', 'B', 'Q', 'K', 'B', 'N', 'R'}  // row 7 = rank 1 (White pieces, bottom row)
};

ChessGame::ChessGame(BoardDriver* bd, ChessEngine* ce, WiFiManagerESP32* wm, MoveHistory* mh) : boardDriver(bd), chessEngine(ce), wifiManager(wm), moveHistory(mh), currentTurn('w'), gameOver(false), replaying(false), stopAnimation(nullptr) {}

LedRGB ChessGame::getPlayerLedColor(char color) const {
  if (_pcSet)  // per-game player colours (HvH from profiles)
    return (color == 'w') ? _pcWhite : _pcBlack;
  return (color == 'w') ? boardDriver->animPathColorWhite()
                        : boardDriver->animPathColorBlack();
}

// --- Colour derivation helpers for player-colour-tinted move highlights ---
static inline LedRGB _ledScale(LedRGB c, float f) {   // dim toward black
  return LedRGB{(uint8_t)(c.r * f), (uint8_t)(c.g * f), (uint8_t)(c.b * f)};
}
static inline LedRGB _ledLighten(LedRGB c, float f) { // blend toward white
  return LedRGB{(uint8_t)(c.r + (255 - c.r) * f),
                (uint8_t)(c.g + (255 - c.g) * f),
                (uint8_t)(c.b + (255 - c.b) * f)};
}
static inline LedRGB _ledBlend(LedRGB a, LedRGB b, float f) {  // blend a → b by f
  return LedRGB{(uint8_t)((float)a.r + ((float)b.r - (float)a.r) * f),
                (uint8_t)((float)a.g + ((float)b.g - (float)a.g) * f),
                (uint8_t)((float)a.b + ((float)b.b - (float)a.b) * f)};
}

void ChessGame::setPlayerNames(const char* white, const char* black) {
  _playerNameWhite[0] = '\0';
  _playerNameBlack[0] = '\0';
  if (white && *white) {
    strncpy(_playerNameWhite, white, sizeof(_playerNameWhite) - 1);
    _playerNameWhite[sizeof(_playerNameWhite) - 1] = '\0';
  }
  if (black && *black) {
    strncpy(_playerNameBlack, black, sizeof(_playerNameBlack) - 1);
    _playerNameBlack[sizeof(_playerNameBlack) - 1] = '\0';
  }
}

ChessGame::~ChessGame() {
  if (stopAnimation) {
    stopAnimation->store(true);
    stopAnimation = nullptr;
  }
}

void ChessGame::_updateOpeningName() {
    // Longest-prefix match against the OPENINGS table. We compare the
    // table entry as a prefix of the current move history (followed by
    // either end-of-string or a space, so "e2e4" doesn't match "e2e45").
    // Require at least 2 full moves (= 4 plies = 3 spaces) before
    // showing anything — saying "Englisch" after a single c2c4 reply
    // feels premature.
    const char* hist = _moveHistoryUci.c_str();
    size_t histLen   = _moveHistoryUci.length();
    int    spaces    = 0;
    for (size_t i = 0; i < histLen; i++) {
        if (hist[i] == ' ') spaces++;
    }
    if (spaces < 3) { _openingName[0] = '\0'; return; }

    const char* best = nullptr;
    size_t bestLen = 0;
    for (size_t i = 0; i < OPENINGS_COUNT; i++) {
        const char* mv = OPENINGS[i].moves;
        size_t mlen = strlen(mv);
        if (mlen > histLen) continue;
        if (strncmp(hist, mv, mlen) != 0) continue;
        // Either the history ends right here, or the next char is a space.
        if (mlen != histLen && hist[mlen] != ' ') continue;
        if (mlen > bestLen) { best = OPENINGS[i].name; bestLen = mlen; }
    }
    if (best) strlcpy(_openingName, best, sizeof(_openingName));
    else      _openingName[0] = '\0';
}

void ChessGame::initializeBoard() {
  currentTurn = 'w';
  gameOver = false;
  _glowFadePending = true;   // fade the resting glow in on the first render of the game
  // Clear leftover end-of-game metadata so the web UI doesn't keep showing
  // the previous winner on the first frame of a new game.
  _winnerColor = '?';
  _endReason   = ' ';
  _moveHistoryUci = "";
  _openingName[0] = '\0';
  memcpy(board, INITIAL_BOARD, sizeof(INITIAL_BOARD));
  chessEngine->reset();
  chessEngine->recordPosition(board, currentTurn);
  wifiManager->updateBoardState(ChessUtils::boardToFEN(board, currentTurn, chessEngine), ChessUtils::evaluatePosition(board));
}

void ChessGame::waitForBoardSetup(const char targetBoard[8][8], bool showFirework) {
#ifdef SKIP_CALIBRATION
  // No real Hall sensors connected yet — bypass the "place pieces in
  // starting position" guard entirely so the game can start and the
  // web UI will render the chess layout.
  (void)targetBoard;
  (void)showFirework;
  Serial.println("[SKIP_CALIBRATION] skipping waitForBoardSetup");
  return;
#endif
  // Quick check: if the board already matches, return immediately
  boardDriver->readSensors();
  bool allCorrect = true;
  for (int row = 0; row < 8 && allCorrect; row++) {
    for (int col = 0; col < 8; col++) {
      if ((targetBoard[row][col] != ' ') != boardDriver->getSensorState(row, col)) {
        allCorrect = false;
        break;
      }
    }
  }
  if (allCorrect) {
    if (showFirework)
      boardDriver->fireworkAnimation();
    return;
  }

  Serial.println("Set up the board in the required position...");
  boardDriver->acquireLEDs();
  boardDriver->clearAllLEDs(false);
  while (!allCorrect) {
    // Escape hatch: if the user resigned / aborted from the web, stop waiting for
    // a physical setup that may never come (e.g. a dead Hall-sensor column would
    // otherwise block here forever and the main loop could never process the end).
    if (wifiManager->isEndRequested()) {
      Serial.println("End requested from web — aborting board-setup wait");
      boardDriver->clearAllLEDs();
      boardDriver->releaseLEDs();
      return;
    }
    boardDriver->readSensors();
    allCorrect = true;

    // Check every square
    for (int row = 0; row < 8; row++) {
      for (int col = 0; col < 8; col++) {
        bool shouldHavePiece = (targetBoard[row][col] != ' ');
        bool hasPiece = boardDriver->getSensorState(row, col);
        if (shouldHavePiece != hasPiece) {
          allCorrect = false;
          break;
        }
      }
      if (!allCorrect)
        break;
    }

    // Update LED display to show required setup
    for (int row = 0; row < 8; row++) {
      for (int col = 0; col < 8; col++) {
        bool shouldHavePiece = (targetBoard[row][col] != ' ');
        bool hasPiece = boardDriver->getSensorState(row, col);

        if (shouldHavePiece && !hasPiece) {
          // Need to place a piece here - show where pieces should go
          if (ChessUtils::isWhitePiece(targetBoard[row][col]))
            boardDriver->setSquareLED(row, col, ChessUtils::colorLed('w'));
          else
            boardDriver->setSquareLED(row, col, ChessUtils::colorLed('b'));
        } else if (!shouldHavePiece && hasPiece) {
          // Need to remove a piece from here - show in red
          boardDriver->setSquareLED(row, col, LedColors::Red);
        } else {
          // Correct state - no LED
          boardDriver->setSquareLED(row, col, LedColors::Off);
        }
      }
    }
    boardDriver->showLEDs();

    delay(SENSOR_READ_DELAY_MS);
  }
  boardDriver->releaseLEDs();

  Serial.println("Board setup complete!");
  if (showFirework)
    boardDriver->fireworkAnimation();
}

// Shiny-aware resting glow for one square: the side to move gets a constant
// base highlight plus a moving glint (during the sweep phase); the opponent is
// dimmed. Cycle = sweep (shinySweepMs) then pause (shinyPauseMs). Reads millis()
// so successive calls animate.
LedRGB ChessGame::shinyGlowAt(int row, int col) {
  char p = board[row][col];
  if (p == ' ' || p == '\0') return LedColors::Off;
  char side = ChessUtils::isWhitePiece(p) ? 'w' : 'b';
  LedRGB glow = getPlayerLedColor(side);
  if (gameOver) return glow;
  // Cross-fade on turn switch: the new active side's shiny fades IN while the
  // old active side's fades OUT over TRANSITION_MS, so the indicator never hard-
  // switches. act = how "active" this side currently is (0..1).
  const float TRANSITION_MS = 520.0f;
  float trans = (float)((uint32_t)millis() - _shinyTurnSwitchMs) / TRANSITION_MS;
  if (trans > 1.0f) trans = 1.0f;
  float act = (side == currentTurn) ? trans : (1.0f - trans);
  if (act <= 0.001f) return glow;   // fully settled inactive side → plain glow

  uint32_t shSweep = boardDriver->getShinySweepMs();
  uint32_t shCycle = shSweep + boardDriver->getShinyPauseMs();
  uint32_t shPhase = (uint32_t)millis() % shCycle;
  bool  shining = (shPhase < shSweep);
  float add = 0.0f;
  if (shining) {
    // The band sweeps from OFF one edge to OFF the other (-0.3 → 1.3) WITHOUT
    // wrapping, so the glint is ~0 at both ends of the sweep. That removes the
    // hard flicker at the sweep⇄pause boundary that showed up with mismatched
    // sweep/pause values.
    float sweep = -0.3f + (float)shPhase / (float)shSweep * 1.6f;
    float pos = (float)(row + col) / 14.0f;
    float d = fabsf(pos - sweep);
    add = expf(-(d * d) * 32.0f) * 0.85f;
  }
  return _ledBlend(glow, boardDriver->getShinyColor(), (0.12f + add) * act);
}

void ChessGame::renderBoardLEDs() {
  boardDriver->acquireLEDs();
  LedRGB target[8][8];
  for (int row = 0; row < 8; row++)
    for (int col = 0; col < 8; col++)
      target[row][col] = shinyGlowAt(row, col);

  if (_glowFadePending) {
    // Game start: fade the player-colour glow IN from black instead of snapping
    // it on, so the sides light up smoothly after the start/load animation.
    _glowFadePending = false;
    const int STEPS = 24;
    for (int s = 1; s <= STEPS; s++) {
      float t = (float)s / STEPS;
      for (int row = 0; row < 8; row++)
        for (int col = 0; col < 8; col++) {
          LedRGB c = target[row][col];
          boardDriver->setSquareLED(row, col, LedRGB{(uint8_t)(c.r * t), (uint8_t)(c.g * t), (uint8_t)(c.b * t)});
        }
      boardDriver->showLEDs();
      delay(18);                       // ~430 ms total fade-in
    }
  } else {
    for (int row = 0; row < 8; row++)
      for (int col = 0; col < 8; col++)
        boardDriver->setSquareLED(row, col, target[row][col]);
    boardDriver->showLEDs();
  }
  boardDriver->releaseLEDs();
}

void ChessGame::renderRestingFrame() {
  renderBoardLEDs();
  // While the glint is actually sweeping, render extra frames in a tight burst so
  // a fast sweep (e.g. 200 ms) stays smooth instead of stuttering at the ~20 fps
  // main-loop rate. The board is static during the pause, so single frames there
  // are fine. Capped so a zero-pause (continuous) sweep still returns to poll
  // sensors. No sensor reads here — pickup detection resumes on the next tick.
  unsigned long guard = millis() + 250;
  while (boardDriver->isShinySweeping() && millis() < guard) {
    delay(15);
    renderBoardLEDs();
  }
}

void ChessGame::renderBoardLEDsWave(int originRow, int originCol) {
  boardDriver->acquireLEDs();
  // Snapshot the current (pickup-dimmed) state and the SHINY resting state — the
  // wave settles straight into the turn-indicator look (glint already in place),
  // so the continuous render afterwards just keeps the glint moving instead of a
  // separate "brighten everything, THEN start the shiny" step.
  LedRGB start[8][8], target[8][8];
  for (int row = 0; row < 8; row++)
    for (int col = 0; col < 8; col++) {
      target[row][col] = shinyGlowAt(row, col);
      start[row][col] = boardDriver->getLEDColor(row, col);
    }
  // Expand a ring from the placed square; each cell ramps from its dimmed start
  // to full glow as the wave front sweeps past it.
  const float maxD = 12.0f;
  const int STEPS = 16;
  for (int s = 1; s <= STEPS; s++) {
    float front = (float)s / STEPS * maxD;
    for (int row = 0; row < 8; row++)
      for (int col = 0; col < 8; col++) {
        float dr = row - originRow, dc = col - originCol;
        float d = sqrtf(dr * dr + dc * dc);
        float k = front - d;                 // distance past the wave front
        k = (k <= 0.0f) ? 0.0f : (k >= 1.6f ? 1.0f : k / 1.6f);   // ramp over ~1.6 cells
        LedRGB a = start[row][col], b = target[row][col];
        boardDriver->setSquareLED(row, col, LedRGB{
            (uint8_t)((float)a.r + ((float)b.r - (float)a.r) * k),
            (uint8_t)((float)a.g + ((float)b.g - (float)a.g) * k),
            (uint8_t)((float)a.b + ((float)b.b - (float)a.b) * k)});
      }
    boardDriver->showLEDs();
    delay(20);                               // ~320 ms ripple
  }
  for (int row = 0; row < 8; row++)
    for (int col = 0; col < 8; col++)
      boardDriver->setSquareLED(row, col, target[row][col]);
  boardDriver->showLEDs();
  boardDriver->releaseLEDs();
}

void ChessGame::previewReachable(int srcR, int srcC, int moves[][2], int moveCount, char piece) {
  if (moveCount <= 0) return;
  char   side = ChessUtils::isWhitePiece(piece) ? 'w' : 'b';
  LedRGB base = getPlayerLedColor(side);
  LedRGB srcColor = _ledLighten(base, 0.5f);
  LedRGB pointCol = _pcSet ? base : LedColors::White;

  const int MAXO = 28, MAXL = 8;
  if (moveCount > MAXO) moveCount = MAXO;
  int pLen[MAXO];
  int pR[MAXO][MAXL], pC[MAXO][MAXL];
  for (int i = 0; i < moveCount; i++) {
    int n = 0, ddr = moves[i][0] - srcR, ddc = moves[i][1] - srcC;
    int adr = abs(ddr), adc = abs(ddc), sr = (ddr > 0) - (ddr < 0), sc = (ddc > 0) - (ddc < 0);
    pR[i][n] = srcR; pC[i][n++] = srcC;
    if ((adr == 1 && adc == 2) || (adr == 2 && adc == 1)) {
      if (adr > adc) { pR[i][n]=srcR+sr; pC[i][n++]=srcC; pR[i][n]=srcR+2*sr; pC[i][n++]=srcC; }
      else           { pR[i][n]=srcR; pC[i][n++]=srcC+sc; pR[i][n]=srcR; pC[i][n++]=srcC+2*sc; }
      pR[i][n] = moves[i][0]; pC[i][n++] = moves[i][1];
    } else {
      int steps = (adr > adc) ? adr : adc;
      for (int s = 1; s <= steps && n < MAXL; s++) { pR[i][n]=srcR+sr*s; pC[i][n++]=srcC+sc*s; }
    }
    pLen[i] = n;
  }

  auto tgtCol  = [&](int i) -> LedRGB {
    char d = board[moves[i][0]][moves[i][1]];
    return (d != ' ' && d != '\0') ? LedColors::Red : (_pcSet ? base : LedColors::White);
  };
  auto floorOf = [&](int i) -> LedRGB { return _ledScale(tgtCol(i), 0.14f); };
  auto baseAt  = [&](int r, int c) -> LedRGB {
    for (int i = 0; i < moveCount; i++) if (moves[i][0] == r && moves[i][1] == c) return floorOf(i);
    char d = board[r][c];
    if (d != ' ' && d != '\0') return getPlayerLedColor(ChessUtils::isWhitePiece(d) ? 'w' : 'b');
    return LedColors::Off;
  };

  // Staggered model: option i starts at i*stagger ms; each point runs over its
  // path at CELL_MS per cell; all options run concurrently. The target flashes
  // briefly on arrival, then settles to its dim floor.
  int stagger = boardDriver->getNoobBudgetMs(); if (stagger < 10) stagger = 10;
  const int CELL_MS = 45, HOLD_MS = 250;

  boardDriver->acquireLEDs();
  unsigned long start = millis();
  bool done = false;
  while (!done) {
    if (wifiManager && wifiManager->isEndRequested()) break;
    long t = (long)(millis() - start);
    if (t > 9000) break;                         // safety cap
    for (int r = 0; r < 8; r++) for (int c = 0; c < 8; c++) boardDriver->setSquareLED(r, c, baseAt(r, c));
    boardDriver->setSquareLED(srcR, srcC, srcColor);
    done = true;
    for (int i = 0; i < moveCount; i++) {
      long ot = t - (long)i * stagger;
      if (ot < 0) { done = false; continue; }    // not started yet
      int last = pLen[i] - 1;
      int cell = ot / CELL_MS;
      if (cell < last) {                         // point still travelling
        boardDriver->setSquareLED(pR[i][cell], pC[i][cell], pointCol);
        done = false;
      } else if (ot - (long)last * CELL_MS < HOLD_MS) {   // just arrived → brief flash
        boardDriver->setSquareLED(moves[i][0], moves[i][1], _ledLighten(tgtCol(i), 0.35f));
        done = false;
      }
    }
    boardDriver->showLEDs();
    delay(16);
  }
  boardDriver->releaseLEDs();
}

void ChessGame::applyMove(int fromRow, int fromCol, int toRow, int toCol, char promotion, bool isRemoteMove) {
  char piece = board[fromRow][fromCol];
  char capturedPiece = board[toRow][toCol];

  // Append the move in UCI notation ("e2e4") and re-evaluate the opening
  // name. Cap the buffer at 12 plies so it doesn't grow without bound.
  if (_moveHistoryUci.length() < 70) {
    char uci[6];
    snprintf(uci, sizeof(uci), "%c%d%c%d",
             (char)('a' + fromCol), 8 - fromRow,
             (char)('a' + toCol),   8 - toRow);
    if (_moveHistoryUci.length() > 0) _moveHistoryUci += ' ';
    _moveHistoryUci += uci;
    _updateOpeningName();
  }

  bool isCastling = ChessUtils::isCastlingMove(fromRow, fromCol, toRow, toCol, piece);
  bool isEnPassantCapture = ChessUtils::isEnPassantMove(fromRow, fromCol, toRow, toCol, piece, capturedPiece);
  int enPassantCapturedPawnRow = ChessUtils::getEnPassantCapturedPawnRow(toRow, piece);
  if (toupper(piece) == 'P' && abs(toRow - fromRow) == 2) {
    int enPassantRow = (fromRow + toRow) / 2;
    chessEngine->setEnPassantTarget(enPassantRow, fromCol);
  } else {
    chessEngine->clearEnPassantTarget();
  }
  if (isEnPassantCapture) {
    capturedPiece = board[enPassantCapturedPawnRow][toCol];
    board[enPassantCapturedPawnRow][toCol] = ' ';
  }

  chessEngine->updateHalfmoveClock(piece, capturedPiece);

  board[toRow][toCol] = piece;
  board[fromRow][fromCol] = ' ';

  // Push the new position to the web/app IMMEDIATELY — BEFORE the ~1s of LED
  // animations below. The web server runs on its own task, so board.html gets
  // the move (SSE) right away instead of waiting out the LED cinematic. The turn
  // is flipped here (the move is done); updateGameStatus() refines it with
  // check/mate a moment later. Skipped for castling/promotion (their final
  // position isn't set until further down — the later push handles those) and
  // for replay.
  if (!replaying && !isCastling && !chessEngine->isPawnPromotion(piece, toRow) && wifiManager) {
    char nextTurn = (currentTurn == 'w') ? 'b' : 'w';
    wifiManager->updateBoardState(ChessUtils::boardToFEN(board, nextTurn, chessEngine),
                                  ChessUtils::evaluatePosition(board));
  }

  Serial.printf("%s %s: %c %c%d -> %c%d\n", isRemoteMove ? "Remote" : "Player", isCastling ? "castling" : (isEnPassantCapture ? "en passant" : (capturedPiece != ' ' ? "capture" : "move")), piece, (char)('a' + fromCol), 8 - fromRow, (char)('a' + toCol), 8 - toRow);

  // Trace the piece's path on the LEDs for any move the player didn't make
  // physically (bot, lichess, simulation/replay). Wait for the trail to
  // finish before the physical-move guidance overrides the LEDs. The trail
  // colour + speed comes from the per-player chess-animation settings, hence
  // we pass the moving piece's side.
  char mover = ChessUtils::isWhitePiece(piece) ? 'w' : 'b';
  // CAPTURE: fire the red shock wave FIRST so it starts instantly — it used to be
  // queued AFTER the travel walk, which read as a pause before the "death" wave.
  // The walk then plays on top of it.
  if (capturedPiece != ' ')
    boardDriver->captureAnimation(toRow, toCol);
  // Walk-TRAIL (piece source→dest) plays for EVERY move now, including physical
  // HvH/bot player moves: the user wants a walk animation from the origin square
  // to the destination square whenever a piece is placed. Castling keeps its own
  // dedicated handling. The trail colour + speed come from the moving side's
  // chess-animation settings.
  bool wantTrail = !isCastling;
  bool wantPulse = (isRemoteMove || replaying || animateLocalMoves) && !isCastling;
  if (wantTrail) {
    // Walk in the moving side's colour. When per-profile player colours are set
    // (HvH) the walk matches the piece glow; otherwise pass useColor=false so the
    // board falls back to its animPath colour (and the sim blue/green override).
    boardDriver->movePathAnimation(fromRow, fromCol, toRow, toCol, mover,
                                   getPlayerLedColor(mover), hasPlayerColors());
    boardDriver->waitForAnimationQueue(3000);
  }
  // Soft pulse on the destination — clear "the piece landed here" confirmation.
  if (wantPulse)
    boardDriver->cellPulse(toRow, toCol, getPlayerLedColor(mover));

  if (isRemoteMove && !isCastling && !replaying)
    waitForRemoteMoveCompletion(fromRow, fromCol, toRow, toCol, capturedPiece != ' ', isEnPassantCapture, enPassantCapturedPawnRow);

  if (isCastling)
    applyCastling(fromRow, fromCol, toRow, toCol, piece, isRemoteMove);

  updateCastlingRightsAfterMove(fromRow, fromCol, toRow, toCol, piece, capturedPiece);

  // The capture wave already fired up top (instant). For a non-capture, confirm
  // the landing with the heartbeat.
  if (capturedPiece == ' ' && !replaying)
    confirmSquareCompletion(toRow, toCol);

  if (chessEngine->isPawnPromotion(piece, toRow)) {
    if (!replaying) boardDriver->promotionAnimation(toRow, toCol);
    // If promotion piece is already specified (from bot, lichess, replay), use it
    if (promotion != ' ' && promotion != '\0') {
      promotion = ChessUtils::isWhitePiece(piece) ? toupper(promotion) : tolower(promotion);
    } else if (!replaying && !isRemoteMove) {
      promotion = waitForPromotionChoice(piece);
    } else {
      // Remote move without specified promotion, default to queen
      promotion = ChessUtils::isWhitePiece(piece) ? 'Q' : 'q';
    }
    board[toRow][toCol] = promotion;
    Serial.printf("Pawn promoted to %c\n", promotion);
  }

  if (moveHistory && moveHistory->isRecording())
    moveHistory->addMove(fromRow, fromCol, toRow, toCol, promotion);
}

bool ChessGame::tryPlayerMove(char playerColor, int& fromRow, int& fromCol, int& toRow, int& toCol) {
  // Blink a square RED continuously while an INVALID pickup is held up — the
  // wrong side's piece, or a piece that has no legal move — and keep blinking
  // (never stopping) until the piece is set back down on that square (or the
  // game is ended from the web). Returns once the square is occupied again.
  auto blinkUntilReturned = [&](int r, int c) {
    bool on = false;
    while (true) {
      if (wifiManager && wifiManager->isEndRequested()) break;
      boardDriver->readSensors();
      if (boardDriver->getSensorState(r, c)) break;   // piece is back on the square
      on = !on;
      boardDriver->setSquareLED(r, c, on ? LedColors::Red : LedColors::Off);
      boardDriver->showLEDs();
      boardDriver->updateSensorPrev();
      delay(140);                                      // ~280 ms blink period
    }
    boardDriver->clearAllLEDs();
  };

  for (int row = 0; row < 8; row++)
    for (int col = 0; col < 8; col++) {
      // Continue if nothing was picked up from this square
      if (!boardDriver->getSensorPrev(row, col) || boardDriver->getSensorState(row, col))
        continue;

      char piece = board[row][col];

      // Skip empty squares
      if (piece == ' ')
        continue;

      // Check if it's the correct player's piece
      if (ChessUtils::getPieceColor(piece) != playerColor) {
        Serial.printf("Wrong turn! It's %s's turn to move.\n", ChessUtils::colorName(playerColor));
        if (wifiManager) wifiManager->pushSoundEvent('W'); // wrong-turn pickup → error sound
        blinkUntilReturned(row, col);                      // blink red until put back
        continue;
      }

      Serial.printf("Piece pickup from %c%d\n", (char)('a' + col), 8 - row);
      if (wifiManager) wifiManager->pushSoundEvent('P', piece); // piece lifted → pickup sound (piece type for per-figure sounds)

      // Generate possible moves
      int moveCount = 0;
      int moves[28][2];
      chessEngine->getPossibleMoves(board, row, col, moveCount, moves);

      // Picked-up piece has nowhere to go (pinned / blocked / stalemated): blink
      // the square red until it is set back down, then keep waiting for a move.
      if (moveCount == 0) {
        Serial.printf("No legal move for piece at %c%d — blink until returned\n", (char)('a' + col), 8 - row);
        if (wifiManager) wifiManager->pushSoundEvent('W');
        blinkUntilReturned(row, col);
        continue;
      }

      // Highlight colours. With per-game player colours (HvH) the source/target/
      // capture cues are derived from the moving player's colour — source = full,
      // empty target = dimmed, capture = brighter (toward white). Otherwise the
      // globally-configured hl colours are used (bot/lichess unchanged).
      LedRGB base       = getPlayerLedColor(playerColor);
      LedRGB srcColor   = _pcSet ? base : boardDriver->hlSourceColor();
      LedRGB capColor   = LedColors::Red;  // capturable piece's square: ALWAYS red, regardless of player colour

      // --- Looping pickup reveal (runs while the piece is held up) ---
      bool  dimOthers = boardDriver->getDimOthersOnPickup();
      float dimF = dimOthers ? (boardDriver->getDimOthersPct() / 100.0f) : 1.0f;
      // Pass 1: distance (Chebyshev / king-distance) of each target from the
      // picked-up piece. The reveal radiates OUTWARD ring by ring.
      LedRGB tgtFin[28];
      int    epPawnRow[28];
      int    tgtDist[28];
      bool   tgtCap[28];
      bool   tgtIsPath[28];   // reveal-only L-path cell (knight) — NOT a legal target
      bool   reached[28] = {false};   // true once the reveal wave has reached this square at least once
      int    maxDist = 0, minDist = 99;
      for (int i = 0; i < moveCount; i++) {
        int r = moves[i][0], c = moves[i][1];
        bool ep = ChessUtils::isEnPassantMove(row, col, r, c, piece, board[r][c]);
        tgtIsPath[i] = false;
        tgtCap[i]    = !(board[r][c] == ' ' && !ep);
        epPawnRow[i] = ep ? ChessUtils::getEnPassantCapturedPawnRow(r, piece) : -1;
        int dr = r - row, dc = c - col;
        if (dr < 0) dr = -dr;
        if (dc < 0) dc = -dc;
        tgtDist[i] = (dr > dc) ? dr : dc;
        if (tgtDist[i] > maxDist) maxDist = tgtDist[i];
        if (tgtDist[i] < minDist) minDist = tgtDist[i];
      }
      // Pass 2: brightness GRADIENT by distance — the farthest reachable square
      // is the brightest (full), the closest the dimmest. Empty targets use the
      // player colour, captures stay red; both scale with distance.
      for (int i = 0; i < moveCount; i++) {
        float gf = (maxDist > minDist) ? (float)(tgtDist[i] - minDist) / (maxDist - minDist) : 1.0f;
        float bri = 0.25f + 0.75f * gf;   // 0.25 (nearest) .. 1.0 (farthest)
        LedRGB raw = tgtCap[i] ? capColor : (_pcSet ? base : boardDriver->hlTargetColor());
        tgtFin[i] = LedRGB{(uint8_t)(raw.r * bri), (uint8_t)(raw.g * bri), (uint8_t)(raw.b * bri)};
      }
      // Reveal ring bounds. For most pieces this is just the target min/max
      // distance. KNIGHTS additionally trace their L-path: the squares the jump
      // leaps over (mid + corner of each move) are appended as reveal-only "path"
      // cells, so the pickup shows the knight walking over them — overwriting any
      // piece in the way. Path cells are NOT legal targets (tgtIsPath marks them
      // so placement detection ignores them). L-step distance (mid=1, corner=2,
      // target=3) makes the wave walk source→mid→corner→target.
      // "Noob" mode reveals each possible move ONE AFTER ANOTHER (see below), so
      // it does NOT append the simultaneous L-path cells — it traces each move's
      // own path in turn instead.
      bool noob = boardDriver->getRevealSequential();
      int revealMin = minDist, revealMax = maxDist;
      if (toupper(piece) == 'N' && !noob) {
        for (int i = 0; i < moveCount; i++) tgtDist[i] = 3;      // targets sit at L-distance 3
        LedRGB pathCol = _ledScale(base, 0.28f);                 // strongly dimmed: the jump PATH must read clearly weaker than the reachable TARGET squares
        int baseCount = moveCount;
        for (int i = 0; i < baseCount; i++) {
          int ddr = moves[i][0] - row, ddc = moves[i][1] - col;
          int sr2 = (ddr > 0) - (ddr < 0), sc2 = (ddc > 0) - (ddc < 0);
          int cells[2][3];
          if (abs(ddr) > abs(ddc)) {                             // long leg = rows
            cells[0][0] = row + sr2;     cells[0][1] = col; cells[0][2] = 1;   // mid
            cells[1][0] = row + 2 * sr2; cells[1][1] = col; cells[1][2] = 2;   // corner
          } else {                                               // long leg = cols
            cells[0][0] = row; cells[0][1] = col + sc2;     cells[0][2] = 1;   // mid
            cells[1][0] = row; cells[1][1] = col + 2 * sc2; cells[1][2] = 2;   // corner
          }
          for (int k = 0; k < 2; k++) {
            bool dup = false;
            for (int j = 0; j < moveCount; j++)
              if (moves[j][0] == cells[k][0] && moves[j][1] == cells[k][1]) { dup = true; break; }
            if (dup || moveCount >= 28) continue;
            moves[moveCount][0] = cells[k][0]; moves[moveCount][1] = cells[k][1];
            tgtDist[moveCount]  = cells[k][2];
            tgtFin[moveCount]   = pathCol;
            tgtCap[moveCount]   = false;
            epPawnRow[moveCount] = -1;
            tgtIsPath[moveCount] = true;
            moveCount++;
          }
        }
        revealMin = 1;
        revealMax = 3;
      }
      // Slider ghosts: for a rook/bishop/queen the ray continues PAST the first
      // blocker — those "would-be reachable" squares light at a configurable tiny
      // brightness (rayGhostPct, default 1%) so the whole line stays visible.
      // Ghost cells are reveal-only (tgtIsPath) and never legal targets. Skipped
      // in noob mode (one move at a time) and when rayGhostPct == 0.
      int ghostVal = boardDriver->getRayGhostVal();   // raw 0–255 brightness
      char up = toupper(piece);
      bool isSlider = (up == 'R' || up == 'B' || up == 'Q');
      if (isSlider && !noob && ghostVal > 0) {
        float gf = ghostVal / 255.0f;
        LedRGB ghostCol = _ledScale(_pcSet ? base : boardDriver->hlTargetColor(), gf);
        static const int rookDirs[4][2]   = {{0,1},{0,-1},{1,0},{-1,0}};
        static const int bishopDirs[4][2] = {{1,1},{1,-1},{-1,1},{-1,-1}};
        for (int set = 0; set < 2; set++) {
          if (set == 0 && up == 'B') continue;    // bishop: no straight rays
          if (set == 1 && up == 'R') continue;    // rook: no diagonal rays
          const int (*dirs)[2] = (set == 0) ? rookDirs : bishopDirs;
          for (int d = 0; d < 4; d++) {
            bool blocked = false;
            for (int k = 1; k < 8 && moveCount < 28; k++) {
              int r = row + dirs[d][0] * k, c = col + dirs[d][1] * k;
              if (r < 0 || r >= 8 || c < 0 || c >= 8) break;
              if (!blocked) {
                if (board[r][c] != ' ' && board[r][c] != '\0') blocked = true;  // first blocker
                continue;                                                        // up to & incl. blocker → handled as legal move / capture
              }
              bool dup = false;                    // squares BEYOND the blocker → ghost
              for (int j = 0; j < moveCount; j++)
                if (moves[j][0] == r && moves[j][1] == c) { dup = true; break; }
              if (dup) continue;
              moves[moveCount][0] = r; moves[moveCount][1] = c;
              tgtDist[moveCount]  = k;
              tgtFin[moveCount]   = ghostCol;
              tgtCap[moveCount]   = false;
              epPawnRow[moveCount] = -1;
              tgtIsPath[moveCount] = true;
              if (k > revealMax) revealMax = k;
              moveCount++;
            }
          }
        }
      }
      // Dimmed resting colour of a square (its look while NOT highlighted).
      auto backdropCol = [&](int r, int c) -> LedRGB {
        char dp = board[r][c];
        if (dp == ' ' || dp == '\0') return LedColors::Off;
        LedRGB gc = getPlayerLedColor(ChessUtils::isWhitePiece(dp) ? 'w' : 'b');
        return LedRGB{(uint8_t)(gc.r * dimF), (uint8_t)(gc.g * dimF), (uint8_t)(gc.b * dimF)};
      };
      // "Bing" onset for a revealed square: instead of a flat fade-in, the cell
      // snaps up to a bright overshoot (toward white) then eases down to its
      // final colour — like a struck LED. t = 0..1 over the ring's reveal.
      auto bingColor = [&](LedRGB fin, float t) -> LedRGB {
        LedRGB over = _ledLighten(fin, 0.70f);
        if (t < 0.35f) {                        // attack: rise quickly to the overshoot
          float a = t / 0.35f;
          return LedRGB{(uint8_t)(over.r * a), (uint8_t)(over.g * a), (uint8_t)(over.b * a)};
        }
        float d = (t - 0.35f) / 0.65f;          // decay: overshoot → final colour
        return LedRGB{(uint8_t)((float)over.r + ((float)fin.r - (float)over.r) * d),
                      (uint8_t)((float)over.g + ((float)fin.g - (float)over.g) * d),
                      (uint8_t)((float)over.b + ((float)fin.b - (float)over.b) * d)};
      };
      // A legal TARGET square must stay LEACHTLY visible across the looping
      // reveal — it never fully extinguishes after the first pass (knight
      // included). Reveal-only path / ghost cells have no floor (they may go to
      // their backdrop). The floor is a dim tint of the target's own colour.
      auto floorCol = [&](int i) -> LedRGB {
        // Reveal-only cells (slider ghosts behind a blocker, knight L-path) dip to
        // a fraction of their reveal colour — NOT to the backdrop — so EVERY square
        // behind the blocker (empty OR occupied) stays lit uniformly AND keeps
        // pulsing with the loop. Legal targets settle to a dim tint of their colour.
        if (tgtIsPath[i]) return _ledScale(tgtFin[i], 0.30f);
        return _ledScale(tgtFin[i], 0.14f);
      };
      auto maxLed = [](LedRGB a, LedRGB b) -> LedRGB {
        return LedRGB{(uint8_t)(a.r > b.r ? a.r : b.r),
                      (uint8_t)(a.g > b.g ? a.g : b.g),
                      (uint8_t)(a.b > b.b ? a.b : b.b)};
      };
      const int DIM_STEPS = 16, REV_STEPS = 10;
      int slotMs = boardDriver->getRevealRingMs();   // time per distance ring (configurable)
      int  animPhase = 1;   // start REVEALING immediately (instant); the background dim
                            // ramps CONCURRENTLY below — no waiting for the dim first.
      int  animStep = 0, animDist = revealMin;
      int  bgDim = 0;       // background-dim ramp step, advanced once per reveal frame
      // Snapshot the board's CURRENT look (the shiny turn-indicator render) so the
      // pickup dim fades FROM it — without this the opponent pieces jumped to full
      // glow on the first dim frame ("kurz hell") before dimming down.
      LedRGB preDim[8][8];
      for (int pr = 0; pr < 8; pr++)
        for (int pc = 0; pc < 8; pc++)
          preDim[pr][pc] = boardDriver->getLEDColor(pr, pc);
      // Reveal targets (incl. ghost / knight-path cells) are owned by the reveal —
      // the background dim must skip them (and the source square).
      bool isTgt[8][8] = {};
      for (int i = 0; i < moveCount; i++) isTgt[moves[i][0]][moves[i][1]] = true;
      unsigned long animHoldUntil = 0;

      // --- "Noob" sequential reveal state. Show one possible move at a time,
      // tracing its path from the source (the knight's L, a slider's ray), hold,
      // fade, then the next — looping while the piece is held. npPath* holds the
      // active move's path cells (source first, target last).
      int  noobIdx = 0;
      int  npPathR[14], npPathC[14], npLen = 0, npCursor = 0;
      auto computeMovePath = [&](int tr2, int tc2, int* pR, int* pC) -> int {
        int n = 0, ddr = tr2 - row, ddc = tc2 - col;
        int adr2 = abs(ddr), adc2 = abs(ddc);
        int sr2 = (ddr > 0) - (ddr < 0), sc2 = (ddc > 0) - (ddc < 0);
        pR[n] = row; pC[n++] = col;                                   // source
        if ((adr2 == 1 && adc2 == 2) || (adr2 == 2 && adc2 == 1)) {   // knight L
          if (adr2 > adc2) { pR[n]=row+sr2;   pC[n++]=col;
                             pR[n]=row+2*sr2; pC[n++]=col; }
          else             { pR[n]=row; pC[n++]=col+sc2;
                             pR[n]=row; pC[n++]=col+2*sc2; }
          pR[n] = tr2; pC[n++] = tc2;
        } else {                                                      // slider / king / pawn ray
          int steps = (adr2 > adc2) ? adr2 : adc2;
          for (int s = 1; s <= steps && n < 13; s++) { pR[n]=row+sr2*s; pC[n++]=col+sc2*s; }
        }
        return n;
      };
      // The fast-moving "point" in noob mode — player colour if set, else white.
      LedRGB pointCol = _pcSet ? base : LedColors::White;

      // Wait for piece placement - handle both normal moves and captures. The
      // reveal animation advances one frame per loop and re-loops while the piece
      // stays lifted; sensors are polled every iteration so placement (and the
      // instant cancel when set back on the origin square) is caught right away.
      int targetRow = -1, targetCol = -1;
      bool piecePlaced = false;

      // Instant first frame: light the source highlight and clear the move targets
      // to their backdrop NOW, so the walking reveal begins immediately on pickup
      // (the others are still full-bright here and dim down over the next frames).
      boardDriver->setSquareLED(row, col, srcColor);
      for (int i = 0; i < moveCount; i++)
        boardDriver->setSquareLED(moves[i][0], moves[i][1], backdropCol(moves[i][0], moves[i][1]));
      boardDriver->showLEDs();

      while (!piecePlaced) {
        // Escape hatch: a web resign/abort must end the game even mid-pickup,
        // otherwise this blocks until the piece is physically placed.
        if (wifiManager->isEndRequested()) {
          Serial.println("End requested from web — aborting player move");
          boardDriver->clearAllLEDs();
          return false;
        }
        boardDriver->readSensors();

        // Draw gesture can be initiated while waiting for this move to complete
        if (toupper(piece) == 'K' && checkPhysicalResignOrDraw()) {
          boardDriver->clearAllLEDs();
          return false;
        }

        // First check if the original piece was placed back
        if (boardDriver->getSensorState(row, col)) {
          targetRow = row;
          targetCol = col;
          piecePlaced = true;
          break;
        }

        // Then check all squares for a regular move or capture initiation
        for (int r2 = 0; r2 < 8; r2++) {
          for (int c2 = 0; c2 < 8; c2++) {
            // Skip the original square which was already checked
            if (r2 == row && c2 == col)
              continue;

            // Check if this would be a legal move
            bool isLegalMove = false;
            for (int i = 0; i < moveCount; i++)
              if (!tgtIsPath[i] && moves[i][0] == r2 && moves[i][1] == c2) {
                isLegalMove = true;
                break;
              }

            // If not a legal move, no need to check further
            if (!isLegalMove)
              continue;

            // For capture moves: detect when the target square is empty (captured piece removed)
            // This works whether the piece was just removed or was already removed before pickup
            bool isEnPassantCapture = ChessUtils::isEnPassantMove(row, col, r2, c2, piece, board[r2][c2]);
            int enPassantCapturedPawnRow = ChessUtils::getEnPassantCapturedPawnRow(r2, piece);
            auto isCapturedPiecePickedUp = [&]() -> bool {
              if (isEnPassantCapture)
                return !boardDriver->getSensorState(enPassantCapturedPawnRow, c2);
              else
                return !boardDriver->getSensorState(r2, c2);
            };
            if ((board[r2][c2] != ' ' || isEnPassantCapture) && isCapturedPiecePickedUp()) {
              Serial.printf("Capture initiated at %c%d\n", (char)('a' + c2), 8 - r2);
              // Store the target square and wait for the capturing piece to be placed there
              targetRow = r2;
              targetCol = c2;
              piecePlaced = true;
              if (isEnPassantCapture)
                boardDriver->setSquareLED(enPassantCapturedPawnRow, c2, LedColors::Off);
              // Blink the capture square to indicate waiting for piece placement
              boardDriver->blinkSquare(r2, c2, LedColors::Red, 1, false);
              // Wait for the capturing piece to be placed (or returned to origin to cancel)
              while (!boardDriver->getSensorState(r2, c2)) {
                boardDriver->readSensors();
                // Allow cancellation by placing the piece back to its original position
                if (boardDriver->getSensorState(row, col)) {
                  Serial.println("Capture cancelled");
                  targetRow = row;
                  targetCol = col;
                  break;
                }
                delay(SENSOR_READ_DELAY_MS);
              }
              break;
            }

            // For normal non-capture moves: detect when a piece is placed on an empty square
            if ((board[r2][c2] == ' ' && !isEnPassantCapture) && boardDriver->getSensorState(r2, c2)) {
              targetRow = r2;
              targetCol = c2;
              piecePlaced = true;
              break;
            }
          }
        }

        if (piecePlaced) break;

        // Background dim — runs CONCURRENTLY with the reveal: the non-source,
        // non-target pieces ramp from their pre-pickup look toward the dim level
        // over DIM_STEPS frames, so the dimming happens AT THE SAME TIME as the
        // walking reveal (the reveal starts instantly; no waiting for the dim).
        if (bgDim <= DIM_STEPS) {
          float dt = (float)bgDim / DIM_STEPS;
          for (int dr = 0; dr < 8; dr++)
            for (int dc = 0; dc < 8; dc++) {
              if ((dr == row && dc == col) || isTgt[dr][dc]) continue;
              char dp = board[dr][dc];
              LedRGB to;
              if (dp == ' ' || dp == '\0') {
                to = LedColors::Off;
              } else {
                char dpSide = ChessUtils::isWhitePiece(dp) ? 'w' : 'b';
                LedRGB gc = getPlayerLedColor(dpSide);
                // Noob mode keeps ALL other pieces at full player-colour glow (only
                // the reachable squares dim down). Normal mode dims them: opponent
                // pieces to the configured level, the mover's OWN pieces brighter
                // (2× the dim, capped) so own > opponent without anyone vanishing.
                float df = noob ? 1.0f
                  : (dpSide == playerColor)
                      ? (dimF * 2.0f > 1.0f ? 1.0f : dimF * 2.0f)
                      : dimF;
                to = LedRGB{(uint8_t)(gc.r * df), (uint8_t)(gc.g * df), (uint8_t)(gc.b * df)};
              }
              LedRGB fr = preDim[dr][dc];
              boardDriver->setSquareLED(dr, dc, LedRGB{
                  (uint8_t)((float)fr.r + ((float)to.r - (float)fr.r) * dt),
                  (uint8_t)((float)fr.g + ((float)to.g - (float)fr.g) * dt),
                  (uint8_t)((float)fr.b + ((float)to.b - (float)fr.b) * dt)});
            }
          bgDim++;
        }

        // --- advance the looping pickup reveal by one frame ---
        if (animPhase == 0) {                 // (dead: animPhase starts at 1 now) dim+source

          animStep++;
          float t = (float)animStep / DIM_STEPS;
          for (int dr = 0; dr < 8; dr++)
            for (int dc = 0; dc < 8; dc++) {
              if (dr == row && dc == col) continue;
              char dp = board[dr][dc];
              // Fade from the square's CURRENT colour (preDim) to its dim target —
              // occupied → glow×dimF, empty → off — so nothing flashes brighter first.
              LedRGB from = preDim[dr][dc];
              LedRGB to;
              if (dp == ' ' || dp == '\0') {
                to = LedColors::Off;
              } else {
                LedRGB gc = getPlayerLedColor(ChessUtils::isWhitePiece(dp) ? 'w' : 'b');
                to = LedRGB{(uint8_t)(gc.r * dimF), (uint8_t)(gc.g * dimF), (uint8_t)(gc.b * dimF)};
              }
              boardDriver->setSquareLED(dr, dc, LedRGB{
                  (uint8_t)((float)from.r + ((float)to.r - (float)from.r) * t),
                  (uint8_t)((float)from.g + ((float)to.g - (float)from.g) * t),
                  (uint8_t)((float)from.b + ((float)to.b - (float)from.b) * t)});
            }
          {
            LedRGB sf = preDim[row][col];   // fade source from its current look → srcColor (no dip to black)
            boardDriver->setSquareLED(row, col, LedRGB{
                (uint8_t)((float)sf.r + ((float)srcColor.r - (float)sf.r) * t),
                (uint8_t)((float)sf.g + ((float)srcColor.g - (float)sf.g) * t),
                (uint8_t)((float)sf.b + ((float)srcColor.b - (float)sf.b) * t)});
          }
          boardDriver->showLEDs();
          if (animStep >= DIM_STEPS) {
            boardDriver->setSquareLED(row, col, srcColor);
            for (int i = 0; i < moveCount; i++)
              boardDriver->setSquareLED(moves[i][0], moves[i][1], backdropCol(moves[i][0], moves[i][1]));
            animPhase = 1; animDist = revealMin; animStep = 0;
          }
          delay(22);
        } else if (noob) {
          // "Noob": ALL reachable squares stay faintly lit (dim floor), and a
          // single bright POINT (player colour / white) races along one possible
          // path after another. The path's target flashes brighter as the point
          // arrives, then settles back to its dim floor. Speed = revealRingMs.
          if (moveCount == 0) { delay(SENSOR_READ_DELAY_MS); }
          else {
            // Colour a square returns to once the point has passed it: legal
            // targets keep their dim floor, pure path cells go to the backdrop.
            auto baseAt = [&](int rr, int cc) -> LedRGB {
              for (int i = 0; i < moveCount; i++)
                if (!tgtIsPath[i] && moves[i][0] == rr && moves[i][1] == cc) return floorCol(i);
              // Non-target square: keep an occupied piece at full player glow (noob
              // leaves all other pieces lit); empty cells go dark.
              char dp = board[rr][cc];
              if (dp != ' ' && dp != '\0')
                return getPlayerLedColor(ChessUtils::isWhitePiece(dp) ? 'w' : 'b');
              return LedColors::Off;
            };
            if (npLen == 0) {                 // begin tracing the next move
              npLen = computeMovePath(moves[noobIdx][0], moves[noobIdx][1], npPathR, npPathC);
              npCursor = 1;
              boardDriver->setSquareLED(row, col, srcColor);
              for (int i = 0; i < moveCount; i++)        // assert the dim baseline on EVERY target
                if (!tgtIsPath[i]) boardDriver->setSquareLED(moves[i][0], moves[i][1], floorCol(i));
            }
            if (npCursor > 1) {               // restore the cell just behind the point
              int pr = npPathR[npCursor - 1], pc2 = npPathC[npCursor - 1];
              boardDriver->setSquareLED(pr, pc2, baseAt(pr, pc2));
            }
            bool atTarget = (npCursor >= npLen - 1);
            int hr = npPathR[npCursor], hc = npPathC[npCursor];
            boardDriver->setSquareLED(hr, hc, atTarget ? _ledLighten(tgtFin[noobIdx], 0.35f) : pointCol);
            if (atTarget && epPawnRow[noobIdx] >= 0)
              boardDriver->setSquareLED(epPawnRow[noobIdx], moves[noobIdx][1], LedColors::Purple);
            boardDriver->showLEDs();
            // Budget timing: every option takes the same total time, so the point
            // moves faster on long paths than short ones. Per-cell = budget / steps.
            int npSteps = (npLen > 1) ? (npLen - 1) : 1;
            int stepMs = boardDriver->getNoobBudgetMs() / npSteps;
            if (stepMs < 5) stepMs = 5;
            delay(stepMs);
            if (atTarget) {                   // target reached → settle to floor, go to next path
              reached[noobIdx] = true;
              boardDriver->setSquareLED(hr, hc, floorCol(noobIdx));
              noobIdx = (noobIdx + 1) % moveCount;
              npLen = 0;
              if (noobIdx == 0) { boardDriver->showLEDs(); delay(boardDriver->getRevealPauseMs()); }
            } else { npCursor++; }
          }
        } else if (animPhase == 1) {          // reveal RING by RING — all squares at the
          if (animDist > revealMax) {         // same distance from the piece light together
            animPhase = 2; animHoldUntil = millis() + boardDriver->getRevealPauseMs();  // configurable pause
            delay(SENSOR_READ_DELAY_MS);
          } else {
            bool any = false;
            for (int i = 0; i < moveCount; i++) if (tgtDist[i] == animDist) { any = true; break; }
            if (!any) { animDist++; }         // no target at this distance → skip the ring
            else {
              animStep++;
              float t = (float)animStep / REV_STEPS;
              for (int i = 0; i < moveCount; i++)
                if (tgtDist[i] == animDist) {
                  // Real targets get the bright "bing" onset; reveal-only path /
                  // ghost cells just fade in plainly (a bing would over-brighten a
                  // 1 % ghost into a white flash).
                  LedRGB fin = tgtFin[i];
                  LedRGB c = tgtIsPath[i]
                    ? LedRGB{(uint8_t)(fin.r * t), (uint8_t)(fin.g * t), (uint8_t)(fin.b * t)}
                    : maxLed(bingColor(fin, t), floorCol(i));   // targets never dip below their floor
                  boardDriver->setSquareLED(moves[i][0], moves[i][1], c);
                }
              boardDriver->showLEDs();
              if (animStep >= REV_STEPS) {
                for (int i = 0; i < moveCount; i++)
                  if (tgtDist[i] == animDist) {
                    boardDriver->setSquareLED(moves[i][0], moves[i][1], tgtFin[i]);
                    if (epPawnRow[i] >= 0) boardDriver->setSquareLED(epPawnRow[i], moves[i][1], LedColors::Purple);
                    reached[i] = true;          // wave has now reached this square
                  }
                animDist++; animStep = 0;
              }
              delay(slotMs / REV_STEPS);
            }
          }
        } else if (animPhase == 2) {          // hold, then start the fade-out
          if (millis() >= animHoldUntil) { animPhase = 3; animStep = 0; }
          else { delay(12); }
        } else {                              // phase 3: targets fade OUT to their FLOOR (never fully off), then re-reveal
          const int FADE_STEPS = 12;
          animStep++;
          float t = (float)animStep / FADE_STEPS;   // 0 -> 1
          for (int i = 0; i < moveCount; i++) {
            LedRGB a = tgtFin[i], b = floorCol(i);   // legal targets settle to a dim floor; path/ghost to backdrop
            boardDriver->setSquareLED(moves[i][0], moves[i][1], LedRGB{
                (uint8_t)((float)a.r + ((float)b.r - (float)a.r) * t),
                (uint8_t)((float)a.g + ((float)b.g - (float)a.g) * t),
                (uint8_t)((float)a.b + ((float)b.b - (float)a.b) * t)});
          }
          boardDriver->showLEDs();
          if (animStep >= FADE_STEPS) {
            for (int i = 0; i < moveCount; i++)
              boardDriver->setSquareLED(moves[i][0], moves[i][1], floorCol(i));
            animPhase = 1; animDist = revealMin; animStep = 0;
          }
          delay(30);   // ~360 ms gentle fade-out
        }

        // Capture-target red pulse: once the reveal wave has reached a capturable
        // square it pulses red CONTINUOUSLY (overlaid on whatever the reveal phase
        // drew) until the piece is placed — a constant "you can take this here".
        // For en passant the pulse sits on the captured pawn's square.
        {
          float rp = 0.30f + 0.70f * (0.5f + 0.5f * sinf((float)millis() * 0.0065f));
          bool anyCap = false;
          for (int i = 0; i < moveCount; i++)
            if (tgtCap[i] && reached[i]) {
              int pr = (epPawnRow[i] >= 0) ? epPawnRow[i] : moves[i][0];
              boardDriver->setSquareLED(pr, moves[i][1], LedRGB{(uint8_t)(255 * rp), 0, 0});
              anyCap = true;
            }
          if (anyCap) boardDriver->showLEDs();
        }
      }

      if (targetRow == row && targetCol == col) {
        Serial.println("Pickup cancelled");
        boardDriver->clearAllLEDs();
        return false;
      }

      bool legalMove = false;
      for (int i = 0; i < moveCount; i++)
        if (!tgtIsPath[i] && moves[i][0] == targetRow && moves[i][1] == targetCol) {
          legalMove = true;
          break;
        }

      if (!legalMove) {
        Serial.println("Illegal move, reverting");
        boardDriver->clearAllLEDs();
        return false;
      }

      fromRow = row;
      fromCol = col;
      toRow = targetRow;
      toCol = targetCol;

      // Repaint the resting board (full glow) instead of blanking the LEDs. A
      // bare clearAllLEDs() here flashed the entire board black for one frame
      // between the pickup-reveal ending and the placement walk starting — the
      // "alle LEDs gehen kurz aus" the user saw. It also left the walk's base
      // snapshot all-black, so the other pieces stayed dark during the walk.
      // board[][] is still the pre-move position here, so the source square
      // stays lit until applyMove animates the piece across to the destination.
      // Re-light the pickup-dimmed pieces as a WAVE rippling out from the square
      // just placed on, instead of snapping the glow back on.
      // For a CAPTURE, skip the ~320 ms re-illumination ripple — applyMove fires
      // the red shock wave instantly, and the ripple would just delay it. A quick
      // repaint gives the walk/shock a clean base. Normal moves keep the ripple.
      if (board[targetRow][targetCol] != ' ' && board[targetRow][targetCol] != '\0')
        renderBoardLEDs();
      else
        renderBoardLEDsWave(targetRow, targetCol);
      return true;
    }

  return false;
}

void ChessGame::advanceTurn() {
  chessEngine->incrementFullmoveClock(currentTurn);
  currentTurn = (currentTurn == 'w') ? 'b' : 'w';
  _shinyTurnSwitchMs = millis();   // start the shiny cross-fade to the new side
  chessEngine->recordPosition(board, currentTurn);
}

void ChessGame::updateGameStatus() {
  advanceTurn();

  if (chessEngine->isCheckmate(board, currentTurn)) {
    char winnerColor = (currentTurn == 'w') ? 'b' : 'w';
    Serial.printf("CHECKMATE! %s wins!\n", ChessUtils::colorName(winnerColor));
    // New mate animation: losing pieces blink red 3x, then the board fills
    // rank-by-rank from the winner's side in the winner's player colour,
    // followed by a "<Name> GEWINNT" scroll. The name comes from whatever
    // setPlayerNames() was called with at game start (HvH profile name,
    // bot config, sim default). Empty falls back to "WEISS / SCHWARZ".
    LedRGB winnerLed = getPlayerLedColor(winnerColor);
    LedRGB loserLed  = getPlayerLedColor(currentTurn);
    const char* winnerName = getPlayerName(winnerColor);
    boardDriver->checkmateAnimation(board, winnerColor, winnerLed, loserLed,
                                    (winnerName && *winnerName) ? winnerName : nullptr);
    gameOver = true;
    _winnerColor = winnerColor;
    _endReason   = 'C';
    if (moveHistory) moveHistory->finishGame(RESULT_CHECKMATE, winnerColor);
    return;
  }

  if (chessEngine->isStalemate(board, currentTurn)) {
    Serial.println("STALEMATE! Game is a draw.");
    boardDriver->fireworkAnimation(LedColors::Cyan);
    gameOver = true;
    _winnerColor = 'd';
    _endReason   = 'S';
    if (moveHistory) moveHistory->finishGame(RESULT_STALEMATE, 'd');
    return;
  }

  if (chessEngine->isFiftyMoveRule()) {
    Serial.println("DRAW by 50-move rule! No captures or pawn moves in the last 50 moves.");
    boardDriver->fireworkAnimation(LedColors::Cyan);
    gameOver = true;
    _winnerColor = 'd';
    _endReason   = '5';
    if (moveHistory) moveHistory->finishGame(RESULT_DRAW_50, 'd');
    return;
  }

  if (chessEngine->isThreefoldRepetition()) {
    Serial.println("DRAW by threefold repetition! Same position occurred 3 times.");
    boardDriver->fireworkAnimation(LedColors::Cyan);
    gameOver = true;
    _winnerColor = 'd';
    _endReason   = '3';
    if (moveHistory) moveHistory->finishGame(RESULT_DRAW_3FOLD, 'd');
    return;
  }

  if (chessEngine->isInsufficientMaterial(board)) {
    Serial.println("DRAW by insufficient material! Neither side can checkmate.");
    boardDriver->fireworkAnimation(LedColors::Cyan);
    gameOver = true;
    _winnerColor = 'd';
    _endReason   = 'I';
    if (moveHistory) moveHistory->finishGame(RESULT_DRAW_INSUFFICIENT, 'd');
    return;
  }

  if (chessEngine->isKingInCheck(board, currentTurn)) {
    Serial.printf("%s is in CHECK!\n", ChessUtils::colorName(currentTurn));
    boardDriver->clearAllLEDs(false);

    // Red border pulse first to alert the player from the periphery, then the
    // king-square blink directs the eye to the actual threat.
    boardDriver->checkBorderFlash();

    int kingRow = -1;
    int kingCol = -1;
    if (chessEngine->findKingPosition(board, currentTurn, kingRow, kingCol))
      boardDriver->blinkSquare(kingRow, kingCol, LedColors::Yellow);
  }

  Serial.printf("It's %s's turn !\n", ChessUtils::colorName(currentTurn));

  // Verify the physical board matches the expected state after each turn
  if (!replaying)
    waitForBoardSetup(board, false);
}

void ChessGame::setBoardStateFromFEN(const String& fen) {
  ChessUtils::fenToBoard(fen, board, currentTurn, chessEngine);
  chessEngine->recordPosition(board, currentTurn);
  if (moveHistory && moveHistory->isRecording())
    moveHistory->addFen(fen);
  wifiManager->updateBoardState(ChessUtils::boardToFEN(board, currentTurn, chessEngine), ChessUtils::evaluatePosition(board));
  Serial.println("Board state set from FEN: " + fen);
  ChessUtils::printBoard(board);
  // Guide the user to set up the physical board to match the new position
  if (!replaying)
    waitForBoardSetup(board, false);
}

char ChessGame::waitForPromotionChoice(char piece) {
  if (!wifiManager->isWebClientConnected())
    return ChessUtils::isWhitePiece(piece) ? 'Q' : 'q';

  wifiManager->startPromotionWait(ChessUtils::getPieceColor(piece));
  unsigned long promotionStart = millis();
  while (wifiManager->isPromotionPending() && wifiManager->getPromotionChoice() == ' ') {
    if (millis() - promotionStart >= PROMOTION_TIMEOUT_MS) {
      Serial.println("Promotion timeout - defaulting to queen");
      break;
    }
    delay(25);
  }

  char promotion = wifiManager->getPromotionChoice();
  wifiManager->clearPromotion();
  boardDriver->clearAllLEDs();

  if (promotion != ' ')
    return ChessUtils::isWhitePiece(piece) ? toupper(promotion) : tolower(promotion);
  return ChessUtils::isWhitePiece(piece) ? 'Q' : 'q';
}

void ChessGame::resignGame(char resigningColor) {
  if (gameOver) return;
  char winnerColor = (resigningColor == 'w') ? 'b' : 'w';
  Serial.printf("RESIGNATION! %s resigns. %s wins!\n", ChessUtils::colorName(resigningColor), ChessUtils::colorName(winnerColor));
  boardDriver->fireworkAnimation(ChessUtils::colorLed(winnerColor));
  gameOver = true;
  _winnerColor = winnerColor;
  _endReason   = 'R';
  if (moveHistory) moveHistory->finishGame(RESULT_RESIGNATION, winnerColor);
}

void ChessGame::drawGame() {
  if (gameOver) return;
  Serial.println("DRAW by mutual agreement!");
  boardDriver->fireworkAnimation(LedColors::Cyan);
  gameOver = true;
  _winnerColor = 'd';
  _endReason   = 'D';
  if (moveHistory) moveHistory->finishGame(RESULT_DRAW_AGREEMENT, 'd');
}

void ChessGame::endGameManually(char winner, char reason) {
  if (gameOver) return;
  if (winner != 'w' && winner != 'b' && winner != 'd') return;
  if (reason != 'M' && reason != 'T') reason = 'M';
  Serial.printf("MANUAL END: winner=%c reason=%c\n", winner, reason);
  gameOver = true;
  _winnerColor = winner;
  _endReason   = reason;
  // Persist as a resignation entry (winner-color preserved) so the live
  // game file closes cleanly and stays in the games archive.
  if (moveHistory) {
    uint8_t result = (winner == 'd') ? RESULT_DRAW_AGREEMENT
                                     : RESULT_RESIGNATION;
    moveHistory->finishGame(result, winner);
  }
  if (boardDriver) {
    if (winner == 'd') boardDriver->fireworkAnimation(LedColors::Cyan);
    else               boardDriver->fireworkAnimation(ChessUtils::colorLed(winner));
  }
}

bool ChessGame::checkPhysicalResignOrDraw() {
  if (gameOver) return false;
  if (!boardDriver->isCalibrated()) return false;

  int wKingRow = -1, wKingCol = -1, bKingRow = -1, bKingCol = -1;
  chessEngine->findKingPosition(board, 'w', wKingRow, wKingCol);
  chessEngine->findKingPosition(board, 'b', bKingRow, bKingCol);
  if (wKingRow < 0 || bKingRow < 0) return false;
  if (boardDriver->getSensorState(wKingRow, wKingCol) || boardDriver->getSensorState(bKingRow, bKingCol))
    return false;

  Serial.println("Both kings lifted! Confirming draw gesture...");

  // Temporarily stop any running animation to free the LED mutex
  bool hadAnimation = (stopAnimation != nullptr);
  if (hadAnimation) {
    stopAnimation->store(true);
    stopAnimation = nullptr;
  }

  constexpr unsigned long DRAW_HOLD_MS = 2000;
  constexpr int PROGRESS_STEPS = 8;

  boardDriver->acquireLEDs();
  boardDriver->clearAllLEDs(false);

  unsigned long start = millis();
  int shownProgress = -1;
  while (millis() - start < DRAW_HOLD_MS) {
    boardDriver->readSensors();

    if (boardDriver->getSensorState(wKingRow, wKingCol) || boardDriver->getSensorState(bKingRow, bKingCol)) {
      boardDriver->clearAllLEDs();
      boardDriver->releaseLEDs();
      if (hadAnimation)
        stopAnimation = boardDriver->startThinkingAnimation();
      Serial.println("Draw gesture aborted (a king was placed back)");
      return false;
    }

    unsigned long elapsed = millis() - start;
    int progress = ((elapsed + 1) * PROGRESS_STEPS) / DRAW_HOLD_MS;
    if (progress > PROGRESS_STEPS)
      progress = PROGRESS_STEPS;

    if (progress != shownProgress) {
      boardDriver->clearAllLEDs(false);
      for (int i = 0; i < progress; i++) {
        boardDriver->setSquareLED(7 - i, 3, LedColors::Cyan);
        boardDriver->setSquareLED(i, 4, LedColors::Cyan);
      }
      boardDriver->showLEDs();
      shownProgress = progress;
    }

    delay(SENSOR_READ_DELAY_MS);
  }

  boardDriver->clearAllLEDs();
  boardDriver->releaseLEDs();
  drawGame();
  return true;
}

void ChessGame::updateCastlingRightsAfterMove(int fromRow, int fromCol, int toRow, int toCol, char movedPiece, char capturedPiece) {
  uint8_t rights = chessEngine->getCastlingRights();

  // King moved => lose both rights for that color
  if (movedPiece == 'K')
    rights &= ~(0x01 | 0x02);
  else if (movedPiece == 'k')
    rights &= ~(0x04 | 0x08);

  // Rook moved from corner => lose that side's right
  if (movedPiece == 'R') {
    if (fromRow == 7 && fromCol == 7) rights &= ~0x01;
    if (fromRow == 7 && fromCol == 0) rights &= ~0x02;
  } else if (movedPiece == 'r') {
    if (fromRow == 0 && fromCol == 7) rights &= ~0x04;
    if (fromRow == 0 && fromCol == 0) rights &= ~0x08;
  }

  // Rook captured on corner => lose that side's right
  if (capturedPiece == 'R') {
    if (toRow == 7 && toCol == 7) rights &= ~0x01;
    if (toRow == 7 && toCol == 0) rights &= ~0x02;
  } else if (capturedPiece == 'r') {
    if (toRow == 0 && toCol == 7) rights &= ~0x04;
    if (toRow == 0 && toCol == 0) rights &= ~0x08;
  }

  chessEngine->setCastlingRights(rights);
}

void ChessGame::applyCastling(int kingFromRow, int kingFromCol, int kingToRow, int kingToCol, char kingPiece, bool waitForKingCompletion) {
  int deltaCol = kingToCol - kingFromCol;
  if (kingFromRow != kingToRow) return;
  if (deltaCol != 2 && deltaCol != -2) return;

  int rookFromCol = (deltaCol == 2) ? 7 : 0;
  int rookToCol = (deltaCol == 2) ? 5 : 3;
  char rookPiece = (kingPiece >= 'a' && kingPiece <= 'z') ? 'r' : 'R';

  // Update board state
  board[kingToRow][rookToCol] = rookPiece;
  board[kingToRow][rookFromCol] = ' ';

  // Skip all LED prompts and physical waits during replay
  if (replaying) return;

  boardDriver->acquireLEDs();

  if (waitForKingCompletion) {
    // Handle LED prompts and wait for king move
    Serial.printf("Castling: please move king from %c%d to %c%d\n", (char)('a' + kingFromCol), 8 - kingFromRow, (char)('a' + kingToCol), 8 - kingToRow);

    boardDriver->clearAllLEDs(false);
    boardDriver->setSquareLED(kingFromRow, kingFromCol, LedColors::Cyan);
    boardDriver->setSquareLED(kingToRow, kingToCol, LedColors::White);
    boardDriver->showLEDs();

    // Wait for king to be lifted from its original square
    while (boardDriver->getSensorState(kingFromRow, kingFromCol)) {
      boardDriver->readSensors();
      delay(SENSOR_READ_DELAY_MS);
    }

    // Wait for king to be placed on destination square
    boardDriver->clearAllLEDs(false);
    boardDriver->setSquareLED(kingToRow, kingToCol, LedColors::White);
    boardDriver->showLEDs();

    while (!boardDriver->getSensorState(kingToRow, kingToCol)) {
      boardDriver->readSensors();
      delay(SENSOR_READ_DELAY_MS);
    }

    boardDriver->clearAllLEDs();
  }

  // Handle LED prompts and wait for rook move
  Serial.printf("Castling: please move rook from %c%d to %c%d\n", (char)('a' + rookFromCol), 8 - kingToRow, (char)('a' + rookToCol), 8 - kingToRow);

  // Wait for rook to be lifted from its original square
  boardDriver->clearAllLEDs(false);
  boardDriver->setSquareLED(kingToRow, rookFromCol, LedColors::Cyan);
  boardDriver->setSquareLED(kingToRow, rookToCol, LedColors::White);
  boardDriver->showLEDs();

  while (boardDriver->getSensorState(kingToRow, rookFromCol)) {
    boardDriver->readSensors();
    delay(SENSOR_READ_DELAY_MS);
  }

  // Wait for rook to be placed on destination square
  boardDriver->clearAllLEDs(false);
  boardDriver->setSquareLED(kingToRow, rookToCol, LedColors::White);
  boardDriver->showLEDs();

  while (!boardDriver->getSensorState(kingToRow, rookToCol)) {
    boardDriver->readSensors();
    delay(SENSOR_READ_DELAY_MS);
  }

  boardDriver->clearAllLEDs();
  boardDriver->releaseLEDs();
}

void ChessGame::confirmSquareCompletion(int row, int col) {
  // Confirm the placed move with a quick "heartbeat" (lub-dub) on the target in
  // the moving side's colour — was a single hard blink. The just-placed piece
  // sits on (row,col); derive the side from it. The peak is the player colour
  // brightened toward white so the beat pops above the resting glow, then it
  // fades back to the glow.
  char p = board[row][col];
  char side = (p != ' ' && p != '\0' && !ChessUtils::isWhitePiece(p)) ? 'b' : 'w';
  boardDriver->heartbeatSquare(row, col, _ledLighten(getPlayerLedColor(side), 0.55f));
}