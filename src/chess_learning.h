#pragma once
#include "chess_game.h"
#include "openings_learn.h"

// Lern-Modus: führt den Spieler eine Eröffnungs-Hauptlinie durch.
// - Für jeden Halbzug leuchten die From- und To-Felder farbig auf:
//   * Eigene Züge (Spielerfarbe): gelb (From) + grün (To)
//   * Gegnerische Züge: violett (From) + cyan (To) — der Spieler bewegt
//     die Figur stellvertretend.
// - Bei falscher Bewegung wird das falsche Feld rot blinkend markiert und
//   der Spieler muss das Brett auf die Ausgangsposition zurücksetzen.
// - Mastery-Counter pro (Eröffnung, Schwierigkeitsstufe) wird im NVS unter
//   dem Namespace "learn" persistiert (Schlüssel = `<id>_<level>`).
class ChessLearning : public ChessGame {
 public:
  struct TargetMove {
    int8_t fromRow;
    int8_t fromCol;
    int8_t toRow;
    int8_t toCol;
    char   promotion;
  };

  ChessLearning(BoardDriver* bd, ChessEngine* ce, WiFiManagerESP32* wm, MoveHistory* mh);

  // Auswahl der Lektion. MUSS vor begin() aufgerufen werden.
  // level: 1 / 2 / 3 (clamped). playerColor: 'w', 'b' oder ' ' für Default
  // aus der Opening-Tabelle.
  void configure(int openingIdx, int level, char playerColor);

  void begin() override;
  void update() override;

  // Wird vom Web-UI ausgelöst (POST /learn/restart). Setzt die Lektion auf
  // den Anfang zurück, lässt den Modus aber aktiv.
  void requestRestart() { _restartRequested = true; }

  // Read-only Status für /board-update JSON.
  int  getOpeningIdx() const { return _openingIdx; }
  int  getLevel()      const { return _level; }
  char getPlayerColor() const { return _playerColor; }
  int  getCurrentPlie() const { return _curPlie; }
  int  getTotalPlies()  const { return _totalPlies; }
  int  getMistakes()    const { return _mistakes; }
  bool isLessonCompleted() const { return _lessonCompleted; }
  const TargetMove* getNextTargetMove() const;
  const char* getOpeningId() const;
  const char* getOpeningDisplayName() const;
  uint8_t getLessonType() const;   // 0=opening, 1=finish, 2=tactic

  // Mastery-Persistenz (NVS, Namespace "learn"). Static so the Web-Layer
  // kann den Stand auch ohne aktive ChessLearning-Instanz abrufen.
  static uint8_t loadMastery(const char* openingId, int level);
  static void    saveMastery(const char* openingId, int level, uint8_t value);
  static uint8_t bumpMastery(const char* openingId, int level);  // returns new value

 private:
  static constexpr int MAX_PLIES = 40;
  static constexpr uint8_t MASTERY_MAX = 5;

  int  _openingIdx;
  int  _level;
  char _playerColor;
  TargetMove _targetMoves[MAX_PLIES];
  int  _totalPlies;
  int  _curPlie;
  int  _mistakes;
  volatile bool _restartRequested;
  bool _lessonCompleted;
  bool _hintsShown;
  bool _inErrorState;
  // Capture latch: a move onto an already-occupied square (a capture) is only
  // accepted once the to-square has been observed EMPTY (captured piece lifted)
  // — otherwise the move would commit prematurely the instant the capturing
  // piece is picked up (to-square still holds the victim). Reset per ply.
  bool _captureCleared;

  void _parseSequence();
  void _showHintsForCurrentMove();
  bool _boardMatchesLogical() const;
  void _resetLesson();
  void _commitCurrentMove();
  // Build, for the current target move, the squares legitimately involved in
  // the move (so the wrong-square guard ignores them — castling rook, en
  // passant victim, capture victim) and the expected post-move occupancy grid
  // (true = should hold a piece once the move is complete). isCaptureOut is set
  // when the destination square is occupied before the move (a real capture).
  void _moveMasks(bool involved[8][8], bool expectOcc[8][8], bool& isCaptureOut) const;
};
