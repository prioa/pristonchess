#pragma once
#include <Arduino.h>

// Lightweight chess.com ELO refresher. Pulls each profile's chess.com
// username and queries https://api.chess.com/pub/player/<handle>/stats
// for the latest rapid/blitz/bullet/daily ratings, then writes them back
// via Profiles::setRatings(). Synchronous HTTPS — call only when WiFi is
// connected and from a context that can spare a few hundred ms per profile.
namespace EloFetcher {

struct Ratings {
    int rapid  = -1;
    int blitz  = -1;
    int bullet = -1;
    int daily  = -1;
};

// Fetch all four ratings for a single chess.com handle. Any rating the user
// hasn't played stays at -1. Returns true if at least one rating came back.
bool fetchRatings(const String& handle, Ratings& out);

// Iterate every profile that has a chess.com handle set and refresh its
// stored ratings via Profiles::setRatings(). Returns the number of profiles
// updated.
int  refreshAllProfiles();

} // namespace EloFetcher
