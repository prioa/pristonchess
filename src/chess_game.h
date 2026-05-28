#ifndef CHESS_GAME_H
#define CHESS_GAME_H

#include "board_driver.h"
#include "chess_engine.h"
#include "chess_utils.h"
#include "led_colors.h"
#include <Arduino.h>
#include <atomic>

// Forward declarations to avoid circular dependencies
class WiFiManagerESP32;
class MoveHistory;

// Base class for chess game modes (shared state and common functionality)
class ChessGame {
  friend class MoveHistory; // MoveHistory needs access to applyMove/advanceTurn for replay
 protected:
  BoardDriver* boardDriver;
  ChessEngine* chessEngine;
  WiFiManagerESP32* wifiManager;
  MoveHistory* moveHistory; // nullptr for Lichess mode (moves already recorded on Lichess cloud)

  char board[8][8];
  char currentTurn; // 'w' or 'b'
  bool gameOver;
  bool replaying;                   // True while replaying moves during resume (suppresses LEDs and physical move waits)
  std::atomic<bool>* stopAnimation; // Stop flag for cancellable animations (thinking/waiting), managed by subclasses

  // Game-end metadata (set when gameOver becomes true). Read by the e-paper
  // display module to render the right winner / draw splash.
  // _winnerColor: 'w' / 'b' / 'd' (draw) / '?' (not over yet)
  // _endReason:   'C' mate, 'S' stalemate, '5' 50-move, '3' 3-fold,
  //               'I' insufficient material, 'R' resign, 'D' draw agreement,
  //               'M' manual / forced end, ' ' (not over)
  char _winnerColor = '?';
  char _endReason   = ' ';

  // Last move squares — set by applyMove(). -1 means "no move yet" so the
  // e-paper can suppress the indicator on a fresh game. board.html doesn't
  // use these; they're for the on-device last-move highlight only.
  int8_t _lastFromRow = -1;
  int8_t _lastFromCol = -1;
  int8_t _lastToRow   = -1;
  int8_t _lastToCol   = -1;

  // Concatenated UCI move history ("e2e4 c7c5 g1f3 …") — used to
  // resolve the opening name. Capped at 12 plies to keep the buffer
  // small; longer games don't change the opening anyway.
  String _moveHistoryUci;
  char   _openingName[24] = "";
  void   _updateOpeningName();

  // Standard initial chess board setup
  static const char INITIAL_BOARD[8][8];

  // Constructor
  ChessGame(BoardDriver* bd, ChessEngine* ce, WiFiManagerESP32* wm, MoveHistory* mh);

  // Common initialization and game flow methods
  void initializeBoard();
  void waitForBoardSetup(const char targetBoard[8][8], bool showFirework = true);
  void applyMove(int fromRow, int fromCol, int toRow, int toCol, char promotion = ' ', bool isRemoteMove = false);
  bool tryPlayerMove(char playerColor, int& fromRow, int& fromCol, int& toRow, int& toCol);
  void updateGameStatus();

  // Chess rule helpers
  void updateCastlingRightsAfterMove(int fromRow, int fromCol, int toRow, int toCol, char movedPiece, char capturedPiece);
  void applyCastling(int kingFromRow, int kingFromCol, int kingToRow, int kingToCol, char kingPiece, bool waitForKingCompletion = false);
  void confirmSquareCompletion(int row, int col);

  static constexpr unsigned long PROMOTION_TIMEOUT_MS = 120000; // 2 minutes to choose a promotion, after which it defaults to queen

  // Virtual hooks (overridden in subclasses)
  virtual void waitForRemoteMoveCompletion(int fromRow, int fromCol, int toRow, int toCol, bool isCapture, bool isEnPassant = false, int enPassantCapturedPawnRow = -1) {}
  virtual char waitForPromotionChoice(char piece);

 public:
  virtual ~ChessGame();

  virtual void begin() = 0;
  virtual void update() = 0;

  void setBoardStateFromFEN(const String& fen);
  bool isGameOver() const { return gameOver; }

  // Read-only accessors for non-game subsystems (e.g. e-paper display).
  char getCurrentTurn() const { return currentTurn; }
  const char (&getBoard() const)[8][8] { return board; }
  char getWinnerColor() const { return _winnerColor; }
  char getEndReason()   const { return _endReason;   }
  // Last move squares for the e-paper last-move highlight. Negative values
  // mean "no move yet" (fresh game or board just initialised).
  int8_t getLastFromRow() const { return _lastFromRow; }
  int8_t getLastFromCol() const { return _lastFromCol; }
  int8_t getLastToRow()   const { return _lastToRow;   }
  int8_t getLastToCol()   const { return _lastToCol;   }
  const char* getOpeningName() const { return _openingName; }

  // Force-end the game with a chosen result. `reason` selects the splash
  // label:
  //   'M' — generic manual end (web "Spiel beenden" button)
  //   'T' — time-loss (a player's clock hit zero)
  // Anything else falls back to 'M'. winner: 'w' / 'b' / 'd'.
  void endGameManually(char winner, char reason = 'M');

  // Resign: the resigning color loses
  void resignGame(char resigningColor);
  // Draw by mutual agreement
  void drawGame();
  // Check if kings have been lifted off the board (physical resign/draw gesture)
  // Returns true if a resign or draw was triggered
  bool checkPhysicalResignOrDraw();

  // Advance turn and record position (extracted from updateGameStatus for replay use)
  void advanceTurn();
};

#endif // CHESS_GAME_H
