#pragma once
#include <stddef.h>

// Tiny on-device ECO table — keyed by the leading UCI move sequence
// (space-separated). Names are kept short (<= 22 chars) so they fit
// inside the e-paper status line. We match the LONGEST prefix that
// hits, so more specific lines override the parent. Move counts cover
// the opening up to ~6 plies, which is plenty for naming purposes.
struct OpeningEntry {
    const char* moves;
    const char* name;
};

static const OpeningEntry OPENINGS[] = {
    // King's-pawn openings — only specific named lines, no generic
    // "Offenes Spiel"/"Königsspringer" fallbacks so the splash doesn't
    // fire on every e2e4 e7e5 game.
    { "e2e4 c7c5",                "Sizilianisch" },
    { "e2e4 c7c5 g1f3 d7d6",      "Sizilianisch Najdorf" },
    { "e2e4 c7c5 g1f3 e7e6",      "Sizilianisch (Paulsen)" },
    { "e2e4 e7e5 g1f3 b8c6 f1b5", "Spanisch" },
    { "e2e4 e7e5 g1f3 b8c6 f1c4", "Italienisch" },
    { "e2e4 e7e5 g1f3 b8c6 d2d4", "Schottisch" },
    { "e2e4 e7e5 g1f3 g8f6",      "Russisch / Petrow" },
    { "e2e4 e7e5 f2f4",           "Koenigsgambit" },
    { "e2e4 e7e6",                "Franzoesisch" },
    { "e2e4 c7c6",                "Caro-Kann" },
    { "e2e4 d7d5",                "Skandinavisch" },
    { "e2e4 d7d6",                "Pirc" },
    { "e2e4 g7g6",                "Modern" },
    { "e2e4 g8f6",                "Aljechin" },
    { "e2e4 b7b6",                "Owen" },
    // Queen's-pawn openings — specific lines only.
    { "d2d4 d7d5 c2c4",           "Damengambit" },
    { "d2d4 d7d5 c2c4 e7e6",      "Damengambit abgel." },
    { "d2d4 d7d5 c2c4 c7c6",      "Slawisch" },
    { "d2d4 d7d5 c2c4 d5c4",      "Damengambit angen." },
    { "d2d4 g8f6 c2c4 e7e6 g1f3 b7b6", "Damenindisch" },
    { "d2d4 g8f6 c2c4 g7g6 b1c3 f8g7", "Koenigsindisch" },
    { "d2d4 f7f5",                "Hollaendisch" },
    // English / flanks
    { "c2c4 e7e5",                "Englisch" },
    { "g1f3 d7d5 c2c4",           "Reti" },
    { "b2b3",                     "Larsen" },
    { "f2f4",                     "Bird" },
};
static constexpr size_t OPENINGS_COUNT = sizeof(OPENINGS) / sizeof(OPENINGS[0]);
