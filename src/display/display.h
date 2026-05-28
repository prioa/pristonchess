#pragma once
#include <Arduino.h>

class ChessGame;

struct ClockState {
  uint32_t whiteMs       = 0;
  uint32_t blackMs       = 0;
  uint32_t timeLimitMs   = 0;
  bool     whiteFlagged  = false;
  bool     blackFlagged  = false;
  bool     ticking       = false;

  void resetTo(uint32_t limitMs) {
    timeLimitMs  = limitMs;
    whiteMs      = limitMs;
    blackMs      = limitMs;
    whiteFlagged = false;
    blackFlagged = false;
  }
};

namespace Display {
void begin();
void invalidate();
void tick(int gameMode, ChessGame* game, const ClockState* clock);
void reset();
} // namespace Display
