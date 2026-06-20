#include "profiles.h"
#include <Preferences.h>
#include "../serial_tee.h"  // must be last: redefines Serial -> tee

// Persisting profiles + stats in NVS (Preferences) — NVS lives in its own
// partition and survives both `upload` (firmware) and `uploadfs` (LittleFS)
// flashes, so player stats accumulate across web-UI updates.
static Preferences _prefs;
static const char* NVS_NS  = "pristonchess";
static const char* NVS_KEY = "profiles";

// Schema (verwaltet vom Frontend, hier nur lesen/schreiben):
// {
//   "active": "p_xyz",
//   "profiles": [
//     {
//       "id": "p_xyz",
//       "name": "Name",
//       "avatar": "♔",
//       "color": "#f0c040",
//       "stats": { "wins":0, "losses":0, "draws":0, "byDepth": { "8": {"w":1,"l":2,"d":0} } },
//       "history": [ {"ts":..., "mode":"stockfish", "depth":8, "result":"win", "plies":24} ],
//       "settings": { ledColors:{...}, sounds:{...} }
//     }
//   ]
// }

// Fixed roster — IDs are stable so per-profile stats survive across boots.
struct FixedProfile { const char* id; const char* name; const char* avatar; const char* color; };
static const FixedProfile FIXED_PROFILES[] = {
    {"benny",   "Benny",   "♚", "#f0c040"},
    {"rene",    "Rene",    "♛", "#58a6ff"},
    {"flo",     "Flo",     "♜", "#7ee787"},
    {"philipp", "Philipp", "♝", "#ff7b72"},
};
static const size_t FIXED_PROFILE_COUNT = sizeof(FIXED_PROFILES) / sizeof(FIXED_PROFILES[0]);

void Profiles::begin() {
    _prefs.begin(NVS_NS, false);
    // Wipe the old avatar-bitmap namespace from earlier builds so it doesn't
    // hog NVS pages once we've reverted to icon-only profiles.
    {
        Preferences old;
        if (old.begin("pc_avatars", false)) {
            old.clear();
            old.end();
        }
    }
    String stored = _prefs.getString(NVS_KEY, "");
    if (stored.length() == 0) {
        // First boot — seed with the four default profiles so the user has
        // something to play against immediately.
        _initEmpty();
        _save();
        return;
    }
    DeserializationError err = deserializeJson(_doc, stored);
    if (err) {
        Serial.printf("[PROF] NVS parse error: %s — re-initialising\n", err.c_str());
        _initEmpty();
        _save();
        return;
    }
    // One-time cleanup: drop any leftover avatar fields from the previous
    // build (they bloated the profile JSON and tripped NVS NOT_ENOUGH_SPACE).
    bool changed = false;
    for (JsonObject p : _doc["profiles"].as<JsonArray>()) {
        if (!p["chesscomAvatar"].isNull()) { p.remove("chesscomAvatar"); changed = true; }
        if (!p["hasAvatar"].isNull())      { p.remove("hasAvatar");      changed = true; }
    }
    if (changed) _save();
}

void Profiles::_initEmpty() {
    _doc.clear();
    JsonArray arr = _doc["profiles"].to<JsonArray>();
    for (const auto& fp : FIXED_PROFILES) {
        JsonObject p = arr.add<JsonObject>();
        p["id"]     = fp.id;
        p["name"]   = fp.name;
        p["avatar"] = fp.avatar;
        p["color"]  = fp.color;
        JsonObject st = p["stats"].to<JsonObject>();
        st["wins"] = 0; st["losses"] = 0; st["draws"] = 0;
        st["byDepth"].to<JsonObject>();
        p["history"].to<JsonArray>();
        p["settings"].to<JsonObject>();
    }
    _doc["active"] = FIXED_PROFILES[0].id;
}

String Profiles::idForName(const char* name) {
    if (!name || !*name) return String("");
    for (JsonObject p : _doc["profiles"].as<JsonArray>()) {
        if (strcasecmp(p["name"] | "", name) == 0) {
            return String((const char*)(p["id"] | ""));
        }
    }
    return String("");
}

int Profiles::getEloById(const char* id) {
    if (!id || !*id) return -1;
    JsonObject p = _findProfile(id);
    if (p.isNull()) return -1;
    return p["elo"] | -1;
}

String Profiles::nameForId(const char* id) {
    if (!id || !*id) return String("");
    JsonObject p = _findProfile(id);
    if (p.isNull()) return String(id);
    const char* n = p["name"] | "";
    return String(n);
}

String Profiles::colorForId(const char* id) {
    if (!id || !*id) return String("");
    JsonObject p = _findProfile(id);
    if (p.isNull()) return String("");
    const char* c = p["color"] | "";
    return String(c);
}

void Profiles::leaderboard(LeaderEntry* out, uint8_t maxCount, uint8_t& outCount) {
    outCount = 0;
    if (!out || maxCount == 0) return;
    JsonArray arr = _doc["profiles"].as<JsonArray>();
    for (JsonObject p : arr) {
        if (outCount >= maxCount) break;
        const char* n = p["name"] | "?";
        strncpy(out[outCount].name, n, sizeof(out[0].name) - 1);
        out[outCount].name[sizeof(out[0].name) - 1] = 0;
        JsonObject st = p["stats"];
        out[outCount].wins   = st["wins"]   | 0;
        out[outCount].losses = st["losses"] | 0;
        out[outCount].draws  = st["draws"]  | 0;
        int elo = p["elo"] | -1;
        if (elo > 32767) elo = 32767;
        out[outCount].elo = (int16_t)elo;
        outCount++;
    }
    // Simple bubble sort: wins desc, then losses asc.
    for (uint8_t i = 0; i + 1 < outCount; i++) {
        for (uint8_t j = i + 1; j < outCount; j++) {
            bool swap = (out[j].wins > out[i].wins) ||
                        (out[j].wins == out[i].wins && out[j].losses < out[i].losses);
            if (swap) {
                LeaderEntry tmp = out[i];
                out[i] = out[j];
                out[j] = tmp;
            }
        }
    }
}

bool Profiles::_save() {
    String out;
    serializeJson(_doc, out);
    size_t written = _prefs.putString(NVS_KEY, out);
    if (written == 0) {
        // Overwriting an existing key can fail when NVS can't briefly hold the
        // OLD + NEW copy at once (fragmentation). Free the old entry first, then
        // retry — this reclaims the space and lets the write through. Without it,
        // profile edits (e.g. player colours) silently don't persist across reboot.
        _prefs.remove(NVS_KEY);
        written = _prefs.putString(NVS_KEY, out);
    }
    if (written == 0) {
        Serial.printf("[PROF] NVS write failed (size=%u)\n", (unsigned)out.length());
        return false;
    }
    return true;
}

String Profiles::_genId() {
    static uint32_t seq = 0;
    char buf[24];
    snprintf(buf, sizeof(buf), "p_%08lx_%02lx",
             (unsigned long)(millis() & 0xFFFFFFFF),
             (unsigned long)(seq++ & 0xFF));
    return String(buf);
}

JsonObject Profiles::_findProfile(const char* id) {
    if (!id || !*id) return JsonObject();
    for (JsonObject p : _doc["profiles"].as<JsonArray>()) {
        if (strcmp(p["id"] | "", id) == 0) return p;
    }
    return JsonObject();
}

String Profiles::allJson() {
    String out;
    serializeJson(_doc, out);
    return out;
}

String Profiles::activeId() const {
    return String((const char*)(_doc["active"] | ""));
}

String Profiles::activeSettingsJson() {
    String aid = activeId();
    if (aid.isEmpty()) return "{}";
    for (JsonObject p : _doc["profiles"].as<JsonArray>()) {
        if (aid == String((const char*)(p["id"] | ""))) {
            String out;
            JsonVariantConst v = p["settings"];
            if (v.isNull()) return "{}";
            serializeJson(v, out);
            return out;
        }
    }
    return "{}";
}

String Profiles::apply(const String& body) {
    JsonDocument req;
    DeserializationError err = deserializeJson(req, body);
    if (err) return "";

    const char* action = req["action"] | "";
    JsonArray arr = _doc["profiles"].as<JsonArray>();

    if (strcmp(action, "create") == 0) {
        const char* name = req["name"] | "";
        if (!*name) return "";
        // Reject duplicate names (case-insensitive) to keep the leaderboard
        // readable.
        for (JsonObject existing : arr) {
            if (strcasecmp(existing["name"] | "", name) == 0) return "";
        }
        JsonObject p = arr.add<JsonObject>();
        String newId = _genId();
        p["id"]       = newId;
        p["name"]     = name;
        p["avatar"]   = req["avatar"]   | "\xE2\x99\x9F"; // default ♟
        p["color"]    = req["color"]    | "#ff8a3d";
        p["chesscom"] = req["chesscom"] | "";
        p["elo"]      = -1;
        JsonObject st = p["stats"].to<JsonObject>();
        st["wins"] = 0; st["losses"] = 0; st["draws"] = 0;
        st["byDepth"].to<JsonObject>();
        p["history"].to<JsonArray>();
        p["settings"].to<JsonObject>();
        if (((const char*)(_doc["active"] | ""))[0] == 0) {
            _doc["active"] = newId;
        }
        _save();
    }
    else if (strcmp(action, "update") == 0) {
        const char* id = req["id"] | "";
        JsonObject p = _findProfile(id);
        if (p.isNull()) return "";
        if (!req["name"].isNull()) {
            String newName = req["name"].as<String>();
            if (newName.length()) p["name"] = newName;  // copy into _doc
        }
        if (!req["avatar"].isNull()) p["avatar"] = req["avatar"].as<String>();  // copy into _doc (not a ptr into req)
        if (!req["color"].isNull())  p["color"]  = req["color"].as<String>();
        if (!req["chesscom"].isNull()) {
            const char* h = req["chesscom"];
            p["chesscom"] = h ? h : "";
            // Force ELO refetch on the next polling cycle.
            p["elo"] = -1;
        }
        _save();
    }
    else if (strcmp(action, "delete") == 0) {
        const char* id = req["id"] | "";
        if (!id || !*id) return "";
        bool removed = false;
        for (size_t i = 0; i < arr.size(); ++i) {
            if (strcmp(arr[i]["id"] | "", id) == 0) {
                arr.remove(i);
                removed = true;
                break;
            }
        }
        if (!removed) return "";
        // If we just deleted the active profile, pick a new active or clear.
        if (strcmp(_doc["active"] | "", id) == 0) {
            _doc["active"] = arr.size() > 0
                             ? (const char*)(arr[0]["id"] | "")
                             : "";
        }
        _save();
    }
    else if (strcmp(action, "reset") == 0) {
        // Hard reset back to the four seeded defaults.
        _initEmpty();
        _save();
    }
    else if (strcmp(action, "select") == 0) {
        const char* id = req["id"] | "";
        JsonObject p = _findProfile(id);
        if (p.isNull()) return "";
        _doc["active"] = id;
        _save();
    }
    else if (strcmp(action, "settings") == 0) {
        // Settings für aktives Profil aus body["settings"] übernehmen
        const char* id = req["id"] | (_doc["active"] | "");
        JsonObject p = _findProfile(id);
        if (p.isNull()) return "";
        JsonVariantConst src = req["settings"];
        if (src.isNull()) return "";
        p["settings"].set(src);
        _save();
    }
    else {
        return "";
    }

    return allJson();
}

// Bump stats for one profile in a Human-vs-Human game.
// `result` is the result FROM THIS PROFILE's perspective ("win"/"loss"/"draw").
static bool _bumpProfileStats(JsonObject p, const char* result, uint16_t plies) {
    if (p.isNull()) return false;
    JsonObject st = p["stats"];
    if (st.isNull()) st = p["stats"].to<JsonObject>();
    if      (strcmp(result, "win")  == 0) st["wins"]   = (st["wins"]   | 0) + 1;
    else if (strcmp(result, "loss") == 0) st["losses"] = (st["losses"] | 0) + 1;
    else if (strcmp(result, "draw") == 0) st["draws"]  = (st["draws"]  | 0) + 1;

    JsonArray hist = p["history"];
    if (hist.isNull()) hist = p["history"].to<JsonArray>();
    JsonDocument entryDoc;
    JsonObject e = entryDoc.to<JsonObject>();
    e["ts"]     = (uint32_t)(millis() / 1000);
    e["mode"]   = "hvh";
    e["result"] = result;
    e["plies"]  = plies;
    JsonDocument tmp;
    JsonArray newArr = tmp.to<JsonArray>();
    newArr.add(e);
    for (size_t i = 0; i < hist.size() && i < 9; ++i) newArr.add(hist[i]);
    p["history"].set(newArr);
    return true;
}

void Profiles::chesscomHandles(ChesscomEntry* out, uint8_t maxCount, uint8_t& outCount) {
    outCount = 0;
    if (!out || maxCount == 0) return;
    for (JsonObject p : _doc["profiles"].as<JsonArray>()) {
        if (outCount >= maxCount) break;
        const char* h = p["chesscom"] | "";
        if (!h || !*h) continue;
        const char* id = p["id"] | "";
        strncpy(out[outCount].id,     id, sizeof(out[0].id) - 1);
        out[outCount].id[sizeof(out[0].id) - 1] = 0;
        strncpy(out[outCount].handle, h,  sizeof(out[0].handle) - 1);
        out[outCount].handle[sizeof(out[0].handle) - 1] = 0;
        outCount++;
    }
}

bool Profiles::setRatings(const char* id, int rapid, int blitz, int bullet, int daily) {
    JsonObject p = _findProfile(id);
    if (p.isNull()) return false;
    JsonObject r = p["ratings"].to<JsonObject>();
    if (rapid  > 0) r["rapid"]  = rapid;  else r.remove("rapid");
    if (blitz  > 0) r["blitz"]  = blitz;  else r.remove("blitz");
    if (bullet > 0) r["bullet"] = bullet; else r.remove("bullet");
    if (daily  > 0) r["daily"]  = daily;  else r.remove("daily");
    // Mirror rapid into the legacy "elo" field so existing readers
    // (web-UI leaderboard) keep working without touching them.
    if (rapid > 0) p["elo"] = rapid;
    else           p.remove("elo");
    return _save();
}

bool Profiles::logHvHResult(const char* whiteId, const char* blackId, char winner, uint16_t plies) {
    const char* whiteResult = (winner == 'w') ? "win"  : (winner == 'b') ? "loss" : "draw";
    const char* blackResult = (winner == 'b') ? "win"  : (winner == 'w') ? "loss" : "draw";
    bool touched = false;
    if (whiteId && *whiteId) {
        if (_bumpProfileStats(_findProfile(whiteId), whiteResult, plies)) touched = true;
    }
    if (blackId && *blackId) {
        if (_bumpProfileStats(_findProfile(blackId), blackResult, plies)) touched = true;
    }
    if (touched) _save();
    return touched;
}

bool Profiles::logGameResult(const char* mode, const char* result, uint8_t depth, uint16_t plies) {
    String aid = activeId();
    if (aid.isEmpty()) return false;
    JsonObject p = _findProfile(aid.c_str());
    if (p.isNull()) return false;

    JsonObject st = p["stats"];
    if (st.isNull()) st = p["stats"].to<JsonObject>();

    if      (strcmp(result, "win")  == 0) st["wins"]   = (st["wins"]   | 0) + 1;
    else if (strcmp(result, "loss") == 0) st["losses"] = (st["losses"] | 0) + 1;
    else if (strcmp(result, "draw") == 0) st["draws"]  = (st["draws"]  | 0) + 1;

    // Per-Tiefe Breakdown nur für Stockfish-Spiele
    if (strcmp(mode, "stockfish") == 0 && depth > 0) {
        char dkey[8];
        snprintf(dkey, sizeof(dkey), "%u", (unsigned)depth);
        JsonObject by = st["byDepth"];
        if (by.isNull()) by = st["byDepth"].to<JsonObject>();
        JsonObject dst = by[dkey];
        if (dst.isNull()) dst = by[dkey].to<JsonObject>();
        if      (strcmp(result, "win")  == 0) dst["w"] = (dst["w"] | 0) + 1;
        else if (strcmp(result, "loss") == 0) dst["l"] = (dst["l"] | 0) + 1;
        else if (strcmp(result, "draw") == 0) dst["d"] = (dst["d"] | 0) + 1;
    }

    // History: vorne einfügen, max 10 Einträge halten (NVS-Blob-Budget)
    JsonArray hist = p["history"];
    if (hist.isNull()) hist = p["history"].to<JsonArray>();
    JsonDocument entryDoc;
    JsonObject e = entryDoc.to<JsonObject>();
    e["ts"]     = (uint32_t)(millis() / 1000);
    e["mode"]   = mode;
    e["result"] = result;
    e["depth"]  = depth;
    e["plies"]  = plies;
    // Vorne anhängen: erst alle nach hinten schieben (einfach via copy)
    JsonDocument tmp;
    JsonArray newArr = tmp.to<JsonArray>();
    newArr.add(e);
    for (size_t i = 0; i < hist.size() && i < 9; ++i) newArr.add(hist[i]);
    p["history"].set(newArr);

    return _save();
}
