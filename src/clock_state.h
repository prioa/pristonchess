#pragma once
#include <stdint.h>

// Chess clock state for both sides.
// timeLimitMs == 0 means "no limit" → the active side counts UP; otherwise the
// active side counts DOWN and flags at zero. resetTo() reloads both clocks and
// clears the flags but intentionally leaves `ticking` untouched — callers set
// the run state explicitly. (Formerly defined in the removed display.h.)
struct ClockState {
    uint32_t whiteMs;
    uint32_t blackMs;
    uint32_t timeLimitMs;
    bool     ticking;
    bool     whiteFlagged;
    bool     blackFlagged;

    void resetTo(uint32_t ms) {
        whiteMs      = ms;
        blackMs      = ms;
        timeLimitMs  = ms;
        whiteFlagged = false;
        blackFlagged = false;
    }
};
