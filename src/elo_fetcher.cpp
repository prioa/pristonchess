#include "elo_fetcher.h"
#include "profiles/profiles.h"
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include "serial_tee.h"  // must be last: redefines Serial -> tee

extern Profiles profiles;

namespace EloFetcher {

namespace {
int extractRating(JsonVariantConst category) {
    if (category.isNull()) return -1;
    return category["last"]["rating"] | -1;
}
} // namespace

bool fetchRatings(const String& handle, Ratings& out) {
    out = Ratings();
    if (handle.isEmpty() || !WiFi.isConnected()) return false;

    WiFiClientSecure client;
    client.setInsecure(); // chess.com cert chain isn't pinned — skip validation

    HTTPClient http;
    String url = "https://api.chess.com/pub/player/" + handle + "/stats";
    if (!http.begin(client, url)) {
        Serial.println("[ELO] http.begin failed");
        return false;
    }
    http.setUserAgent("PristonChess/1.0 (ESP32)");
    // Force uncompressed body so ArduinoJson sees plain JSON, not gzip.
    http.addHeader("Accept", "application/json");
    http.addHeader("Accept-Encoding", "identity");
    http.setTimeout(8000);

    int code = http.GET();
    if (code != 200) {
        Serial.printf("[ELO] %s: HTTP %d\n", handle.c_str(), code);
        http.end();
        return false;
    }

    String body = http.getString();
    http.end();

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body);
    if (err) {
        Serial.printf("[ELO] %s: JSON parse %s (body %u bytes)\n",
                      handle.c_str(), err.c_str(), (unsigned)body.length());
        return false;
    }

    out.rapid  = extractRating(doc["chess_rapid"]);
    out.blitz  = extractRating(doc["chess_blitz"]);
    out.bullet = extractRating(doc["chess_bullet"]);
    out.daily  = extractRating(doc["chess_daily"]);

    return (out.rapid > 0 || out.blitz > 0 || out.bullet > 0 || out.daily > 0);
}

int refreshAllProfiles() {
    Profiles::ChesscomEntry entries[16];
    uint8_t count = 0;
    profiles.chesscomHandles(entries, 16, count);

    int updated = 0;
    for (uint8_t i = 0; i < count; i++) {
        Ratings r;
        bool ok = fetchRatings(String(entries[i].handle), r);
        if (ok) {
            Serial.printf("[ELO] %s (%s) → rapid=%d blitz=%d bullet=%d daily=%d\n",
                          entries[i].id, entries[i].handle,
                          r.rapid, r.blitz, r.bullet, r.daily);
            profiles.setRatings(entries[i].id, r.rapid, r.blitz, r.bullet, r.daily);
            updated++;
        } else {
            Serial.printf("[ELO] %s (%s) → no ratings\n",
                          entries[i].id, entries[i].handle);
        }
        // Brief pause to be polite to chess.com's rate limiter.
        delay(150);
    }
    return updated;
}

} // namespace EloFetcher
