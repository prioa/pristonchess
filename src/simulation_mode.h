#pragma once
#include "chess_game.h"

// Hardware-less playback mode for testing the web UI and chess
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
  // renderBoardLEDs() is inherited from ChessGame (occupied squares in team
  // colour). The manual board-edit path (main.cpp) calls it after a web move.
  // Render position + a move highlight: source square + legal target squares
  // (algebraic, e.g. from="e2", targetsCsv="e3,e4"). Used by manual mode.
  void renderHighlights(const String& fromSq, const String& targetsCsv);

  // Simulation player colours are fixed blue (white) / green (black) per the
  // user's spec — they don't follow the chess-animation settings.
  LedRGB getPlayerLedColor(char color) const override {
    return (color == 'w') ? LedRGB{40, 110, 255}   // blue
                          : LedRGB{60, 220, 90};   // green
  }

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
