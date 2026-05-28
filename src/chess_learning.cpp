#include "chess_learning.h"
#include "chess_utils.h"
#include "led_colors.h"
#include "wifi_manager_esp32.h"
#include <Arduino.h>
#include <Preferences.h>
#include <string.h>

static constexpr const char* LEARN_NVS_NAMESPACE = "learn";

static int _findOpeningIdxById(const char* id) {
  if (!id) return -1;
  for (size_t i = 0; i < LEARN_OPENINGS_COUNT; i++) {
    if (strcmp(LEARN_OPENINGS[i].id, id) == 0) return (int)i;
  }
  return -1;
}

// NVS-Keys sind auf 15 Zeichen begrenzt. Eröffnungs-IDs sind teils länger
// (z.B. "queens_gambit_declined"), darum hashen wir sie zu einem stabilen
// kurzen Key. Kollisionen sind bei 12 Eröffnungen praktisch unmöglich.
static void _masteryKey(const char* openingId, int level, char* out, size_t outLen) {
  uint32_t h = 2166136261u;
  for (const char* p = openingId; *p; ++p) {
    h ^= (uint8_t)*p;
    h *= 16777619u;
  }
  snprintf(out, outLen, "m%08x_%d", (unsigned)h, level);
}

uint8_t ChessLearning::loadMastery(const char* openingId, int level) {
  if (!openingId || level < 1 || level > 3) return 0;
  if (!ChessUtils::ensureNvsInitialized()) return 0;
  Preferences prefs;
  if (!prefs.begin(LEARN_NVS_NAMESPACE, true)) return 0;
  char key[24];
  _masteryKey(openingId, level, key, sizeof(key));
  uint8_t value = prefs.getUChar(key, 0);
  prefs.end();
  if (value > MASTERY_MAX) value = MASTERY_MAX;
  return value;
}

void ChessLearning::saveMastery(const char* openingId, int level, uint8_t value) {
  if (!openingId || level < 1 || level > 3) return;
  if (!ChessUtils::ensureNvsInitialized()) return;
  if (value > MASTERY_MAX) value = MASTERY_MAX;
  Preferences prefs;
  if (!prefs.begin(LEARN_NVS_NAMESPACE, false)) return;
  char key[24];
  _masteryKey(openingId, level, key, sizeof(key));
  prefs.putUChar(key, value);
  prefs.end();
}

uint8_t ChessLearning::bumpMastery(const char* openingId, int level) {
  uint8_t cur = loadMastery(openingId, level);
  if (cur < MASTERY_MAX) cur++;
  saveMastery(openingId, level, cur);
  return cur;
}

ChessLearning::ChessLearning(BoardDriver* bd, ChessEngine* ce, WiFiManagerESP32* wm, MoveHistory* mh)
    : ChessGame(bd, ce, wm, mh),
      _openingIdx(0),
      _level(1),
      _playerColor('w'),
      _totalPlies(0),
      _curPlie(0),
      _mistakes(0),
      _restartRequested(false),
      _lessonCompleted(false),
      _hintsShown(false),
      _inErrorState(false) {}

void ChessLearning::configure(int openingIdx, int level, char playerColor) {
  if (openingIdx < 0 || (size_t)openingIdx >= LEARN_OPENINGS_COUNT) openingIdx = 0;
  if (level < 1) level = 1;
  if (level > 3) level = 3;
  _openingIdx = openingIdx;
  _level = level;
  if (playerColor == 'w' || playerColor == 'b') {
    _playerColor = playerColor;
  } else {
    _playerColor = LEARN_OPENINGS[_openingIdx].playerColor;
  }
}

uint8_t ChessLearning::getLessonType() const {
  return LEARN_OPENINGS[_openingIdx].type;
}

void ChessLearning::begin() {
  Serial.printf("[LEARN] Starting lesson: %s (level %d, player=%c)\n",
                LEARN_OPENINGS[_openingIdx].name, _level, _playerColor);
  _resetLesson();
  // Build the board and wait until the player has set it up physically.
  // Openings start from the initial position; FINISH/TACTIC from a start FEN.
  initializeBoard();
  const char* sf = LEARN_OPENINGS[_openingIdx].startFen;
  if (sf && sf[0]) setBoardStateFromFEN(sf);
  waitForBoardSetup(board, /*showFirework=*/true);
  wifiManager->updateBoardState(
      ChessUtils::boardToFEN(board, currentTurn, chessEngine),
      ChessUtils::evaluatePosition(board));
}

void ChessLearning::update() {
  if (gameOver) return;

  if (_restartRequested) {
    Serial.println("[LEARN] Restart requested via web");
    _restartRequested = false;
    boardDriver->clearAllLEDs();
    _resetLesson();
    initializeBoard();
    const char* sf = LEARN_OPENINGS[_openingIdx].startFen;
    if (sf && sf[0]) setBoardStateFromFEN(sf);
    waitForBoardSetup(board, /*showFirework=*/false);
    wifiManager->updateBoardState(
        ChessUtils::boardToFEN(board, currentTurn, chessEngine),
        ChessUtils::evaluatePosition(board));
    return;
  }

  if (_curPlie >= _totalPlies) {
    // Lesson complete — bump mastery and end the mode.
    if (!_lessonCompleted) {
      _lessonCompleted = true;
      uint8_t newMastery = bumpMastery(LEARN_OPENINGS[_openingIdx].id, _level);
      Serial.printf("[LEARN] Lesson complete! Mistakes=%d, mastery=%u\n",
                    _mistakes, (unsigned)newMastery);
      boardDriver->clearAllLEDs();
      boardDriver->fireworkAnimation(LedColors::Green);
      gameOver = true;
      _winnerColor = 'd';
      _endReason = 'L';  // 'L' = Learning completed
    }
    return;
  }

  if (!_hintsShown) {
    _showHintsForCurrentMove();
    _hintsShown = true;
  }

  // Poll once, decide non-blockingly: the main loop calls us again next
  // iteration so setActiveGameMode etc. keep ticking and the web UI stays
  // responsive while we wait for the player to move the magnet.
  boardDriver->readSensors();
  const TargetMove& tm = _targetMoves[_curPlie];

  bool fromEmpty   = !boardDriver->getSensorState(tm.fromRow, tm.fromCol);
  bool toOccupied  = boardDriver->getSensorState(tm.toRow,   tm.toCol);

  // Scan for any sensor that deviates from the expected pre-move state,
  // ignoring the from-/to-square of the target move.
  int errRow = -1, errCol = -1;
  for (int r = 0; r < 8 && errRow < 0; r++) {
    for (int c = 0; c < 8; c++) {
      if (r == tm.fromRow && c == tm.fromCol) continue;
      if (r == tm.toRow   && c == tm.toCol)   continue;
      bool shouldHave = (board[r][c] != ' ');
      bool hasIt = boardDriver->getSensorState(r, c);
      if (shouldHave != hasIt) {
        errRow = r; errCol = c;
        break;
      }
    }
  }

  if (errRow >= 0) {
    // Player touched the wrong square. Blink red ONCE per error event,
    // and stay in "error state" until the board matches again.
    if (!_inErrorState) {
      _inErrorState = true;
      _mistakes++;
      Serial.printf("[LEARN] Wrong square %c%d — mistake #%d\n",
                    (char)('a' + errCol), 8 - errRow, _mistakes);
      boardDriver->blinkSquare(errRow, errCol, LedColors::Red, 3,
                               /*clearAfter=*/true);
    }
    boardDriver->updateSensorPrev();
    return;
  }

  if (_inErrorState) {
    // Wait for the player to put the board back the way it was.
    if (_boardMatchesLogical()) {
      _inErrorState = false;
      _hintsShown = false;  // re-show hint LEDs next tick
    }
    boardDriver->updateSensorPrev();
    return;
  }

  if (fromEmpty && toOccupied) {
    _commitCurrentMove();
  }

  boardDriver->updateSensorPrev();
}

void ChessLearning::_commitCurrentMove() {
  const TargetMove& tm = _targetMoves[_curPlie];
  boardDriver->clearAllLEDs();
  applyMove(tm.fromRow, tm.fromCol, tm.toRow, tm.toCol, tm.promotion,
            /*isRemoteMove=*/false);
  advanceTurn();
  _curPlie++;
  _hintsShown = false;
  _inErrorState = false;
  wifiManager->updateBoardState(
      ChessUtils::boardToFEN(board, currentTurn, chessEngine),
      ChessUtils::evaluatePosition(board));
}

void ChessLearning::_resetLesson() {
  _curPlie = 0;
  _mistakes = 0;
  _lessonCompleted = false;
  _hintsShown = false;
  _inErrorState = false;
  gameOver = false;
  _winnerColor = '?';
  _endReason = ' ';
  _parseSequence();
}

void ChessLearning::_parseSequence() {
  // Slice the UCI sequence into TargetMove[] up to the level-specific
  // pliesLn cutoff. Format per ply: "<file><rank><file><rank>[promo]" e.g.
  // "e2e4" or "e7e8q".
  const LearnOpening& op = LEARN_OPENINGS[_openingIdx];
  uint8_t maxPlies = (_level == 1) ? op.pliesL1
                   : (_level == 2) ? op.pliesL2
                                   : op.pliesL3;
  if (maxPlies > MAX_PLIES) maxPlies = MAX_PLIES;

  _totalPlies = 0;
  const char* p = op.moves;
  while (*p && _totalPlies < maxPlies) {
    while (*p == ' ') p++;
    if (!*p) break;
    if (strlen(p) < 4) break;
    TargetMove& tm = _targetMoves[_totalPlies];
    int fromCol = p[0] - 'a';
    int fromRank = p[1] - '0';
    int toCol = p[2] - 'a';
    int toRank = p[3] - '0';
    if (fromCol < 0 || fromCol > 7 || toCol < 0 || toCol > 7 ||
        fromRank < 1 || fromRank > 8 || toRank < 1 || toRank > 8) {
      Serial.printf("[LEARN] Malformed UCI in opening %s near: %s\n",
                    op.id, p);
      break;
    }
    tm.fromCol = (int8_t)fromCol;
    tm.fromRow = (int8_t)(8 - fromRank);
    tm.toCol   = (int8_t)toCol;
    tm.toRow   = (int8_t)(8 - toRank);
    tm.promotion = ' ';
    if (p[4] && p[4] != ' ') {
      tm.promotion = p[4];
      p += 5;
    } else {
      p += 4;
    }
    _totalPlies++;
  }
  Serial.printf("[LEARN] Parsed %d plies for %s (level %d)\n",
                _totalPlies, op.id, _level);
}

const ChessLearning::TargetMove* ChessLearning::getNextTargetMove() const {
  if (_curPlie >= _totalPlies) return nullptr;
  return &_targetMoves[_curPlie];
}

const char* ChessLearning::getOpeningId() const {
  return LEARN_OPENINGS[_openingIdx].id;
}

const char* ChessLearning::getOpeningDisplayName() const {
  return LEARN_OPENINGS[_openingIdx].name;
}

void ChessLearning::_showHintsForCurrentMove() {
  if (_curPlie >= _totalPlies) return;
  const TargetMove& tm = _targetMoves[_curPlie];
  char moveColor = (_curPlie % 2 == 0) ? 'w' : 'b';
  bool isPlayerSide = (moveColor == _playerColor);

  LedRGB fromColor = isPlayerSide ? LedColors::Yellow : LedColors::Purple;
  LedRGB toColor   = isPlayerSide ? LedColors::Green  : LedColors::Cyan;

  boardDriver->acquireLEDs();
  boardDriver->clearAllLEDs(false);
  boardDriver->setSquareLED(tm.fromRow, tm.fromCol, fromColor);
  boardDriver->setSquareLED(tm.toRow,   tm.toCol,   toColor);
  boardDriver->showLEDs();
  boardDriver->releaseLEDs();
}

bool ChessLearning::_boardMatchesLogical() const {
  for (int r = 0; r < 8; r++) {
    for (int c = 0; c < 8; c++) {
      bool shouldHave = (board[r][c] != ' ');
      // NOTE: const-cast because getSensorState is not const-correct in the
      // BoardDriver API. It only reads internal state, no mutation.
      bool hasIt = const_cast<BoardDriver*>(boardDriver)->getSensorState(r, c);
      if (shouldHave != hasIt) return false;
    }
  }
  return true;
}

