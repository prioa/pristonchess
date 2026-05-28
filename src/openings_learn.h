#pragma once
#include <stddef.h>
#include <stdint.h>

// On-device library of openings designed for the "Lernen" mode. Each entry
// holds the full main-line move sequence in UCI notation (space-separated)
// plus per-level depth cutoffs. The training UI offers three difficulty
// levels per opening; the level selects how many half-moves of the same
// sequence get drilled.
//
// playerColor: which side the learner physically takes ('w' or 'b'). The
// other side's moves are blinked on the LEDs so the player executes them
// stand-in for the opponent.
// Lesson categories. OPENING = drill an opening line from the start position
// (3 difficulty levels). FINISH = play out a winning endgame from a start FEN.
// TACTIC = find the winning move(s) from a start FEN. FINISH/TACTIC are single
// difficulty (set pliesL1=L2=L3 = full length) and carry a startFen.
enum LessonType : uint8_t { LESSON_OPENING = 0, LESSON_FINISH = 1, LESSON_TACTIC = 2 };

struct LearnOpening {  // generic "lesson" entry (name kept for API compatibility)
  const char* id;          // stable identifier (used by /learn API)
  const char* name;        // German display name (<= 30 chars)
  const char* eco;         // ECO code (optional, "" if none)
  char        playerColor; // 'w' or 'b'
  uint8_t     pliesL1;     // half-moves for difficulty 1
  uint8_t     pliesL2;     // half-moves for difficulty 2
  uint8_t     pliesL3;     // half-moves for difficulty 3 (full line)
  const char* moves;       // full UCI sequence ("e2e4 e7e5 g1f3 ...")
  uint8_t     type;        // LessonType; 0 (OPENING) when omitted below
  const char* startFen;    // nullptr/"" = initial position; else FINISH/TACTIC start
};

static const LearnOpening LEARN_OPENINGS[] = {
  // ---- White repertoire ---------------------------------------------------
  { "italian", "Italienische Eroeffnung", "C50", 'w', 4, 8, 14,
    "e2e4 e7e5 g1f3 b8c6 f1c4 g8f6 d2d3 f8c5 c2c3 d7d6 e1g1 a7a6 a2a4 h7h6" },
  { "spanish", "Spanisch (Ruy Lopez)", "C60", 'w', 4, 8, 14,
    "e2e4 e7e5 g1f3 b8c6 f1b5 a7a6 b5a4 g8f6 e1g1 f8e7 f1e1 b7b5 a4b3 d7d6" },
  { "scotch", "Schottisch", "C45", 'w', 4, 8, 12,
    "e2e4 e7e5 g1f3 b8c6 d2d4 e5d4 f3d4 g8f6 d4c6 b7c6 e4e5 d8e7" },
  { "queens_gambit_declined", "Damengambit abgelehnt", "D30", 'w', 4, 8, 12,
    "d2d4 d7d5 c2c4 e7e6 b1c3 g8f6 c1g5 f8e7 e2e3 e8g8 g1f3 b8d7" },
  { "queens_gambit_accepted", "Damengambit angenommen", "D20", 'w', 4, 8, 12,
    "d2d4 d7d5 c2c4 d5c4 g1f3 g8f6 e2e3 e7e6 f1c4 c7c5 e1g1 a7a6" },
  { "english", "Englische Eroeffnung", "A10", 'w', 4, 8, 12,
    "c2c4 e7e5 b1c3 g8f6 g1f3 b8c6 g2g3 d7d5 c4d5 f6d5 f1g2 d5b6" },
  // ---- Black repertoire ---------------------------------------------------
  { "caro_kann", "Caro-Kann", "B10", 'b', 4, 8, 12,
    "e2e4 c7c6 d2d4 d7d5 b1c3 d5e4 c3e4 c8f5 e4g3 f5g6 h2h4 h7h6" },
  { "sicilian_najdorf", "Sizilianisch Najdorf", "B90", 'b', 6, 10, 14,
    "e2e4 c7c5 g1f3 d7d6 d2d4 c5d4 f3d4 g8f6 b1c3 a7a6 c1e3 e7e5 d4b3 c8e6" },
  { "french", "Franzoesisch", "C00", 'b', 4, 8, 12,
    "e2e4 e7e6 d2d4 d7d5 b1c3 g8f6 c1g5 f8e7 e4e5 f6d7 g5e7 d8e7" },
  { "kings_indian", "Koenigsindisch", "E60", 'b', 6, 10, 14,
    "d2d4 g8f6 c2c4 g7g6 b1c3 f8g7 e2e4 d7d6 g1f3 e8g8 f1e2 e7e5 e1g1 b8c6" },
  { "scandinavian", "Skandinavisch", "B01", 'b', 4, 6, 10,
    "e2e4 d7d5 e4d5 d8d5 b1c3 d5a5 d2d4 g8f6 g1f3 c7c6 f1c4 c8f5" },
  { "pirc", "Pirc-Verteidigung", "B07", 'b', 4, 8, 12,
    "e2e4 d7d6 d2d4 g8f6 b1c3 g7g6 g1f3 f8g7 f1e2 e8g8 e1g1 c8g4" },

  // ---- Partien beenden / Endspiele (FINISH) -------------------------------
  { "backrank_mate", "Grundreihenmatt", "", 'w', 1, 1, 1,
    "e1e8", LESSON_FINISH, "6k1/5ppp/8/8/8/8/5PPP/4R1K1 w - - 0 1" },
  { "queen_edge_mate", "Damenmatt am Rand", "", 'w', 1, 1, 1,
    "h6h7", LESSON_FINISH, "7k/8/6KQ/8/8/8/8/8 w - - 0 1" },

  // ---- Taktik / Puzzles (TACTIC) ------------------------------------------
  { "tactic_rook_m1", "Matt in 1 - Turm", "", 'w', 1, 1, 1,
    "d1d8", LESSON_TACTIC, "3r2k1/5ppp/8/8/8/8/8/3R2K1 w - - 0 1" },
  { "tactic_queen_m1", "Matt in 1 - Dame", "", 'w', 1, 1, 1,
    "e7e8", LESSON_TACTIC, "6k1/4Q1pp/8/8/8/8/8/6K1 w - - 0 1" },
};

static constexpr size_t LEARN_OPENINGS_COUNT =
    sizeof(LEARN_OPENINGS) / sizeof(LEARN_OPENINGS[0]);
