#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

// Verwaltet alle Spielerprofile als ein JsonDocument im RAM, gespiegelt nach
// /profiles.json auf LittleFS. Der Webserver bekommt den rohen JSON-Blob —
// die UI handhabt das Schema (siehe data/index.html → PROFILES_SCHEMA).
class Profiles {
public:
    void begin();

    // Liefert das komplette Dokument (für GET /api/profiles)
    String allJson();

    // Wendet eine Operation an (für POST /api/profiles).
    // body enthält {action: "create"|"update"|"delete"|"select"|"settings", ...}
    // Ergebnis: aktualisiertes Komplett-JSON oder "" bei Fehler.
    String apply(const String& body);

    // Loggt das Ergebnis einer Partie für das aktive Profil
    bool logGameResult(const char* mode, const char* result, uint8_t depth, uint16_t plies);

    // Human-vs-Human result against two profile IDs at once.
    // winner: 'w' = white won, 'b' = black won, 'd' = draw.
    bool logHvHResult(const char* whiteId, const char* blackId, char winner, uint16_t plies);

    // Liefert ID des aktiven Profils (oder leer)
    String activeId() const;

    // Per-Profil-Settings als JSON (für Initial-Load der UI). Leer = keins aktiv.
    String activeSettingsJson();

    // Display name for a fixed profile ID (Benny/Rene/Flo/Philipp). Returns
    // a fallback string when the ID is unknown or empty.
    String nameForId(const char* id);
    // Hex colour ("#rrggbb") configured for a profile, or "" if unknown.
    String colorForId(const char* id);

    // Compact leaderboard entry for the web-UI leaderboard.
    struct LeaderEntry {
        char     name[12];
        uint16_t wins;
        uint16_t losses;
        uint16_t draws;
        int16_t  elo;  // -1 when no chess.com linkage or rating not fetched yet
    };

    // Populates `out` (up to maxCount entries) sorted by wins desc / losses asc
    // and writes the actual entry count to outCount.
    void leaderboard(LeaderEntry* out, uint8_t maxCount, uint8_t& outCount);

    // Chess.com integration — iterate profiles, return the chess.com handles
    // that have one set (used by the ELO refresh task).
    struct ChesscomEntry {
        char id[24];
        char handle[32];
    };
    void chesscomHandles(ChesscomEntry* out, uint8_t maxCount, uint8_t& outCount);

    // Persist the four chess.com ratings for a profile. Pass -1 for any
    // category the user hasn't played; only positive ratings are stored.
    bool setRatings(const char* id, int rapid, int blitz, int bullet, int daily);

    // Look up an ID by display name (case-insensitive). Returns empty
    // String when not found. Maps snapshot names back to IDs for
    // avatar/ELO lookups.
    String idForName(const char* name);
    int    getEloById(const char* id);

private:
    JsonDocument _doc;

    bool _save();
    void _initEmpty();
    JsonObject _findProfile(const char* id);
    String _genId();
};
