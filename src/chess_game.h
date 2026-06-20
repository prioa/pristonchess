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
  bool animateLocalMoves = false;   // When true, physically-played moves also get the LED walk trail (HvH). Off for bot/lichess (those only animate the remote side).
  std::atomic<bool>* stopAnimation; // Stop flag for cancellable animations (thinking/waiting), managed by subclasses

  // Game-end metadata (set when gameOver becomes true). Surfaced via
  // GameStatusData → /board-update so the web UI shows the right winner / draw.
  // _winnerColor: 'w' / 'b' / 'd' (draw) / '?' (not over yet)
  // _endReason:   'C' mate, 'S' stalemate, '5' 50-move, '3' 3-fold,
  //               'I' insufficient material, 'R' resign, 'D' draw agreement,
  //               'M' manual / forced end, ' ' (not over)
  char _winnerColor = '?';
  char _endReason   = ' ';
  char _playerNameWhite[32] = "";
  char _playerNameBlack[32] = "";
  // Per-game player colours (HvH: each player's profile colour). When set, the
  // glow / move-trail / move-highlights derive from these instead of the global
  // animPath colours. _pcSet stays false for bot/lichess → unchanged behaviour.
  LedRGB _pcWhite{0, 0, 0};
  LedRGB _pcBlack{0, 0, 0};
  bool   _pcSet = false;

  // When true, the next renderBoardLEDs() fades the resting glow in from black
  // (a smooth game-start reveal) instead of snapping it on. Set in initializeBoard().
  bool   _glowFadePending = false;

  // millis() of the last turn switch — the shiny turn-indicator cross-fades the
  // glint from the old side to the new side over a short window after this.
  uint32_t _shinyTurnSwitchMs = 0;

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

  // Paint every occupied square in its side's player colour (empty squares
  // off) — the "resting glow" that shows where the pieces stand. Used by the
  // simulation and the Human-vs-Human mode between moves.
  void renderBoardLEDs();
  // Re-illuminate the resting glow as a WAVE radiating out from (originRow,
  // originCol) — used at placement so the pieces dimmed during the pickup
  // brighten back up in a ripple instead of snapping on.
  void renderBoardLEDsWave(int originRow, int originCol);
  // Shiny-aware resting glow for one square (turn-indicator glint + opponent
  // dim baked in). Shared by renderBoardLEDs() and the placement wave so the
  // wave settles straight into the shiny state — no separate "brighten then
  // glint" step.
  LedRGB shinyGlowAt(int row, int col);
  // Resting render for the between-moves state: renders renderBoardLEDs() but,
  // while the shiny glint is mid-sweep, bursts extra frames at a high rate so a
  // fast sweep doesn't stutter at the slow main-loop tick rate.
  void renderRestingFrame();

  // One-shot LED preview of a piece's reachable squares, for modes WITHOUT a
  // physical pickup loop (simulation): all reachable squares glow dim, every
  // other piece stays at full player-colour glow, and — in noob mode — a bright
  // point traces each option in turn (budget-timed). Blocks for ~one cycle.
  void previewReachable(int srcR, int srcC, int moves[][2], int moveCount, char piece);

  // Read-only accessors for non-game subsystems (board driver, web UI).
  char getCurrentTurn() const { return currentTurn; }
  // LED colour to use for the given side in end-of-game animations etc.
  // Defaults to the per-side chess-animation colour (settable in the web
  // UI). SimulationMode overrides to fixed blue / green.
  virtual LedRGB getPlayerLedColor(char color) const;
  // Player display names used in the win cinematic. Default empty falls back
  // to "WEISS" / "SCHWARZ" in the firmware text scroller.
  void setPlayerNames(const char* white, const char* black);
  // Set per-game player colours (e.g. HvH from profile colours). Enables
  // player-colour-derived glow + move highlights for this game instance.
  void setPlayerColors(LedRGB white, LedRGB black) { _pcWhite = white; _pcBlack = black; _pcSet = true; }
  bool hasPlayerColors() const { return _pcSet; }
  const char* getPlayerName(char color) const { return color == 'w' ? _playerNameWhite : _playerNameBlack; }
  const char (&getBoard() const)[8][8] { return board; }
  char getWinnerColor() const { return _winnerColor; }
  char getEndReason()   const { return _endReason;   }
  const char* getOpeningName() const { return _openingName; }

  // Force-end the game with a chosen result. `reason` selects the splash
  // label:
  //   'M' — generic manual end (web "Spiel beenden" button)
  //   'T' — time-loss (a player's clock hit zero)
  // Anything else falls back to 'M'. winner: 'w' / 'b' / 'd'.
  void endGameManually(char winner, char reason = 'M');
  // Mark the game as ended without writing to MoveHistory or going through
  // updateGameStatus. Used by manual-sim FEN diff when it detects checkmate
  // — gives the web UI immediate gameOver state so the win modal and clock
  // stop fire NOW instead of after the LED cinematic.
  void markGameEnded(char winner, char reason) {
    if (gameOver) return;
    gameOver = true;
    _winnerColor = winner;
    _endReason   = reason;
  }

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
