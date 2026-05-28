#pragma once
#include "chess_game.h"

// Hardware-less playback mode for testing the e-paper display and chess
// engine without physical sensors. Plays a fixed scripted game on a timer,
// then restarts. Enabled via -DSIMULATION_MODE=1 in platformio.ini.
class SimulationMode : public ChessGame {
 public:
  SimulationMode(BoardDriver* bd, ChessEngine* ce, WiFiManagerESP32* wm, MoveHistory* mh);

  struct ScriptedMove {
    int  fromRow;
    int  fromCol;
    int  toRow;
    int  toCol;
    char promotion;
  };

  void begin() override;
  void update() override;

  // Manual mode: no scripted playback — the board is driven by web moves.
  void setManual(bool m) { manual = m; }
  // Mirror the current position onto the LEDs (occupied squares in team colour).
  // Public so the manual board-edit path (main.cpp) can refresh after a web move.
  void renderBoardLEDs();
  // Render position + a move highlight: source square + legal target squares
  // (algebraic, e.g. from="e2", targetsCsv="e3,e4"). Used by manual mode.
  void renderHighlights(const String& fromSq, const String& targetsCsv);

 private:
  bool manual = false;  // true = interactive web play instead of scripted moves

  struct ScriptedGame {
    const ScriptedMove* moves;
    size_t              length;
    const char*         name;
  };

  static const ScriptedGame SCRIPTS[];
  static const size_t       SCRIPT_COUNT;

  const ScriptedGame* current;
  size_t              step;
  unsigned long       lastTickMs;
  bool                finished;
  uint32_t            seed;

  static constexpr unsigned long MOVE_INTERVAL_MS = 2600;
  static constexpr unsigned long RESTART_DELAY_MS = 4500;
};
