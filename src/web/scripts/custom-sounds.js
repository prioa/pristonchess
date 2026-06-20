/*
 * custom-sounds.js — user-uploaded sound effects, stored in the browser.
 *
 * Sounds live in IndexedDB (per device, no ESP32 flash impact). Each clip is a
 * compressed audio Blob (recommended: AAC/.m4a, mono, short). Clips are grouped
 * by (category, pieceType):
 *   category : 'pickup' | 'place' | 'capture'
 *   piece    : 'P' 'N' 'B' 'R' 'Q' 'K'  (pawn/knight/bishop/rook/queen/king)
 *
 * On an event the firmware/board reports the piece type; playRandom() picks one
 * of the stored clips for that (category, piece) at random and plays it through
 * the shared AudioContext. If none are stored (or the feature is off), it
 * returns false so the caller can fall back to the built-in synthesized sound.
 *
 * Shared by board.html (playback) and sounds.html (management) — IndexedDB is
 * shared across same-origin pages.
 */
(function (global) {
  'use strict';

  var DB_NAME = 'pc-sounds';
  var DB_VERSION = 1;
  var STORE = 'clips';
  var ENABLED_KEY = 'pcCustomSoundsOn';
  // Curated sound library in the public GitHub repo. The MANIFEST is fetched
  // from raw.githubusercontent.com (always fresh — CORS-enabled) so new/updated
  // clips appear immediately; jsDelivr caches @main for up to ~12 h and would
  // otherwise serve a stale manifest (defaults missing → fallback synth sound).
  // The audio FILES still load from the jsDelivr CDN (see manifest.base) — new
  // file paths aren't subject to the stale-manifest problem. Override both via
  // setManifestUrl(). Bump rid when replacing an existing file's contents.
  var MANIFEST_URL = 'https://raw.githubusercontent.com/prioa/pristonchess/main/sounds/manifest.json';

  var PIECES = ['P', 'N', 'B', 'R', 'Q', 'K'];
  var CATS = ['pickup', 'place', 'capture'];

  var _db = null;
  var _dbPromise = null;
  var _ctx = null;                 // shared AudioContext (set by the host page)
  var _bufCache = {};              // IndexedDB id -> decoded AudioBuffer
  var _urlBuf = {};                // remote URL -> decoded AudioBuffer (memory)
  var _manifest = {};              // "cat/PIECE" -> [{rid, url, name}] curated clips
  var _manifestLoaded = false;
  var _lastPlayed = {};            // "cat/piece" -> uid (avoid immediate repeats)
  var _liveNodes = [];             // keep sources alive until 'ended' (Safari GC)
  var _current = null;             // { src, gain } currently playing — cross-faded out
  var FADE_OUT = 0.22;             // s — previous clip fades out when the next starts
  var FADE_IN = 0.03;              // s — tiny ramp on a new clip to avoid clicks

  function _key(cat, piece) { return cat + '/' + (piece || 'P').toUpperCase(); }

  function _openDB() {
    if (_dbPromise) return _dbPromise;
    _dbPromise = new Promise(function (resolve, reject) {
      var req = indexedDB.open(DB_NAME, DB_VERSION);
      req.onupgradeneeded = function (e) {
        var db = e.target.result;
        if (!db.objectStoreNames.contains(STORE)) {
          var os = db.createObjectStore(STORE, { keyPath: 'id', autoIncrement: true });
          os.createIndex('key', 'key', { unique: false });
        }
      };
      req.onsuccess = function () { _db = req.result; resolve(_db); };
      req.onerror = function () { reject(req.error); };
    });
    return _dbPromise;
  }

  function _tx(mode) {
    return _openDB().then(function (db) {
      return db.transaction(STORE, mode).objectStore(STORE);
    });
  }

  // ---- CRUD ---------------------------------------------------------------

  function list(cat, piece) {
    var key = _key(cat, piece);
    return _tx('readonly').then(function (os) {
      return new Promise(function (resolve, reject) {
        var out = [];
        var idx = os.index('key');
        var req = idx.openCursor(IDBKeyRange.only(key));
        req.onsuccess = function (e) {
          var cur = e.target.result;
          if (cur) {
            var v = cur.value;
            out.push({ id: v.id, name: v.name, size: v.blob ? v.blob.size : 0,
                       type: v.type, source: v.source || 'local', rid: v.rid || '' });
            cur.continue();
          } else resolve(out);
        };
        req.onerror = function () { reject(req.error); };
      });
    });
  }

  // source: 'local' (user upload, device-only) or 'remote' (synced from the
  // curated CDN manifest, keyed by rid so it isn't re-downloaded each sync).
  function _store(rec) {
    return _tx('readwrite').then(function (os) {
      return new Promise(function (resolve, reject) {
        var req = os.add(rec);
        req.onsuccess = function () { resolve(req.result); };
        req.onerror = function () { reject(req.error); };
      });
    });
  }

  function add(cat, piece, file) {
    return _store({
      key: _key(cat, piece),
      cat: cat,
      piece: (piece || 'P').toUpperCase(),
      name: file.name || 'sound',
      type: file.type || 'audio/mp4',
      blob: file,
      source: 'local',
      rid: '',
      ts: 0
    });
  }

  function remove(id) {
    delete _bufCache[id];
    return _tx('readwrite').then(function (os) {
      return new Promise(function (resolve, reject) {
        var req = os.delete(id);
        req.onsuccess = function () { resolve(); };
        req.onerror = function () { reject(req.error); };
      });
    });
  }

  function _getRec(id) {
    return _tx('readonly').then(function (os) {
      return new Promise(function (resolve, reject) {
        var req = os.get(id);
        req.onsuccess = function () { resolve(req.result); };
        req.onerror = function () { reject(req.error); };
      });
    });
  }

  // Counts per (cat, piece) for badges in the UI. Returns { 'pickup/N': 2, ... }.
  function counts() {
    return _tx('readonly').then(function (os) {
      return new Promise(function (resolve, reject) {
        var map = {};
        var req = os.openCursor();
        req.onsuccess = function (e) {
          var cur = e.target.result;
          if (cur) { map[cur.value.key] = (map[cur.value.key] || 0) + 1; cur.continue(); }
          else resolve(map);
        };
        req.onerror = function () { reject(req.error); };
      });
    });
  }

  // Total bytes used, for a storage hint in the UI.
  function totalBytes() {
    return _tx('readonly').then(function (os) {
      return new Promise(function (resolve, reject) {
        var sum = 0;
        var req = os.openCursor();
        req.onsuccess = function (e) {
          var cur = e.target.result;
          if (cur) { sum += cur.value.blob ? cur.value.blob.size : 0; cur.continue(); }
          else resolve(sum);
        };
        req.onerror = function () { reject(req.error); };
      });
    });
  }

  // ---- Curated library: manifest in memory, play straight from the CDN ----

  function setManifestUrl(url) { if (url) MANIFEST_URL = url; }

  // Fetch the manifest and index it in memory so the curated sounds play
  // DIRECTLY from the CDN (per-URL buffer cache) — no dependency on the
  // IndexedDB sync, which proved fragile (private mode / storage / timing).
  // Returns { total } or rejects on network/parse error.
  function loadManifest(url) {
    return fetch(url || MANIFEST_URL, { cache: 'no-store' }).then(function (r) {
      if (!r.ok) throw new Error('manifest HTTP ' + r.status);
      return r.json();
    }).then(function (man) {
      var base = man.base || (url || MANIFEST_URL).replace(/[^/]*$/, '');
      var map = {};
      (man.clips || []).forEach(function (c) {
        if (!c.cat) return;
        var k = c.cat + '/' + (c.piece || '*').toUpperCase();
        (map[k] = map[k] || []).push({
          rid: c.rid || (c.file || ''),
          url: /^https?:/.test(c.file || '') ? c.file : (base + c.file),
          name: c.name || c.file
        });
      });
      _manifest = map;
      _manifestLoaded = true;
      if (global.console) global.console.log('[CustomSounds] manifest loaded:', (man.clips || []).length, 'clips, keys:', Object.keys(map).join(' '));
      return { total: (man.clips || []).length };
    });
  }

  // Map of rid -> stored id for all 'remote' clips currently in IndexedDB.
  function _remoteIndex() {
    return _tx('readonly').then(function (os) {
      return new Promise(function (resolve, reject) {
        var map = {};
        var req = os.openCursor();
        req.onsuccess = function (e) {
          var cur = e.target.result;
          if (cur) {
            if ((cur.value.source || 'local') === 'remote' && cur.value.rid)
              map[cur.value.rid] = cur.value.id;
            cur.continue();
          } else resolve(map);
        };
        req.onerror = function () { reject(req.error); };
      });
    });
  }

  // Pull the curated manifest and reconcile IndexedDB: download clips that are
  // new, drop 'remote' clips no longer listed. Local uploads are untouched.
  // Resolves { added, removed, total } or rejects on network/parse error.
  function sync(manifestUrl) {
    var url = manifestUrl || MANIFEST_URL;
    return fetch(url, { cache: 'no-store' }).then(function (r) {
      if (!r.ok) throw new Error('manifest HTTP ' + r.status);
      return r.json();
    }).then(function (man) {
      var base = man.base || url.replace(/[^/]*$/, '');
      var clips = man.clips || [];
      return _remoteIndex().then(function (have) {
        var wanted = {};
        clips.forEach(function (c) { if (c.rid) wanted[c.rid] = true; });
        var added = 0, removed = 0;
        // Remove remote clips dropped from the manifest.
        var removals = Object.keys(have)
          .filter(function (rid) { return !wanted[rid]; })
          .map(function (rid) { return remove(have[rid]); });
        // Download clips not yet stored.
        var downloads = clips.filter(function (c) {
          return c.rid && !have.hasOwnProperty(c.rid);
        }).map(function (c) {
          var furl = /^https?:/.test(c.file) ? c.file : (base + c.file);
          return fetch(furl, { cache: 'force-cache' }).then(function (r) {
            if (!r.ok) throw new Error('clip HTTP ' + r.status);
            return r.blob();
          }).then(function (blob) {
            return _store({
              key: _key(c.cat, c.piece),
              cat: c.cat,
              piece: (c.piece || 'P').toUpperCase(),
              name: c.name || c.file,
              type: blob.type || 'audio/mp4',
              blob: blob,
              source: 'remote',
              rid: c.rid,
              ts: 0
            });
          }).then(function () { added++; }, function (e) {
            // One bad clip shouldn't abort the whole sync.
            if (global.console) console.warn('sound sync: clip failed', c.rid, e);
          });
        });
        return Promise.all(removals.concat(downloads)).then(function () {
          removed = removals.length;
          return { added: added, removed: removed, total: clips.length };
        });
      });
    });
  }

  // ---- Playback -----------------------------------------------------------

  function setAudioContext(ctx) { _ctx = ctx; }

  function _ensureCtx() {
    if (_ctx) return _ctx;
    try {
      var AC = global.AudioContext || global.webkitAudioContext;
      if (AC) _ctx = new AC();
    } catch (e) { _ctx = null; }
    return _ctx;
  }

  // Playback uses HTML5 <audio> (platform decoder — robust for arbitrary mp3/
  // m4a, unlike WebAudio decodeAudioData which silently rejects some files) and
  // plays curated clips DIRECTLY from the CDN URL (no IndexedDB read needed).
  // iOS requires each <audio> to be started inside a user gesture before it can
  // be played async later, so we keep a POOL of elements primed on first touch
  // (via unlock(), called by the host page) and reuse them round-robin.
  var SILENT_WAV = 'data:audio/wav;base64,UklGRiQAAABXQVZFZm10IBAAAAABAAEAESsAACJWAAACABAAZGF0YQAAAAA=';
  var _pool = [];
  var _poolI = 0;
  var _primed = false;
  var _curEl = null;               // currently-playing element (for cross-fade)
  var FADE_OUT_MS = 220;
  var MAX_MS = 1000;               // cap each clip at ~1 s, then fade out

  function _log() { if (global.console && global.console.log) global.console.log.apply(global.console, arguments); }
  function _warn() { if (global.console && global.console.warn) global.console.warn.apply(global.console, arguments); }

  function _ensurePool() {
    if (_pool.length) return;
    for (var i = 0; i < 6; i++) { var a = new Audio(); a.preload = 'auto'; _pool.push(a); }
  }

  // Prime the pool inside a user gesture so later async play() calls are allowed
  // on iOS. Idempotent; the host page calls this on first pointerdown/touch.
  function unlock() {
    _ensurePool();
    if (_primed) return;
    _primed = true;
    _pool.forEach(function (a) {
      try {
        a.src = SILENT_WAV;
        var p = a.play();
        if (p && p.then) p.then(function () { try { a.pause(); a.currentTime = 0; } catch (e) {} }).catch(function () {});
      } catch (e) {}
    });
    _log('[CustomSounds] audio pool primed');
  }

  function _nextEl() { var a = _pool[_poolI % _pool.length]; _poolI++; return a; }

  // Ramp an element's volume to 0 over `ms`, then pause it. Cancels its cap timer.
  function _fadeEl(a, ms) {
    if (!a) return;
    if (a._cap) { clearTimeout(a._cap); a._cap = null; }
    try {
      var steps = 8, i = 0, v0 = a.volume;
      var iv = setInterval(function () {
        i++;
        var v = v0 * (1 - i / steps);
        try { a.volume = v < 0 ? 0 : v; } catch (e) {}
        if (i >= steps) { clearInterval(iv); try { a.pause(); a.currentTime = 0; } catch (e) {} }
      }, ms / 8);
    } catch (e) { try { a.pause(); } catch (e2) {} }
  }

  // Fade out + stop the current clip so the next one cross-fades in.
  function fadeOutCurrent() { var a = _curEl; _curEl = null; _fadeEl(a, FADE_OUT_MS); }

  function _playUrl(url, label) {
    try {
      _ensurePool();
      fadeOutCurrent();
      var a = _nextEl();
      if (a._cap) { clearTimeout(a._cap); a._cap = null; }
      a.src = url;
      a.volume = 1;
      try { a.currentTime = 0; } catch (e) {}
      _curEl = a;
      var p = a.play();
      if (p && p.catch) p.catch(function (err) { _warn('[CustomSounds] play() rejected', label, err && err.name, '— primed:', _primed); });
      // Cap every clip at MAX_MS, then fade it out (so long samples don't drag on).
      a._cap = setTimeout(function () { if (_curEl === a) _curEl = null; _fadeEl(a, FADE_OUT_MS); }, MAX_MS);
      _log('[CustomSounds] ▶', label, url);
      return true;
    } catch (e) { _warn('[CustomSounds] _playUrl threw', url, e); return false; }
  }

  function isEnabled() {
    // Default ON so the curated CDN sounds work out of the box; only an explicit
    // opt-out ('0') disables them.
    try { return localStorage.getItem(ENABLED_KEY) !== '0'; } catch (e) { return true; }
  }
  function setEnabled(on) {
    try { localStorage.setItem(ENABLED_KEY, on ? '1' : '0'); } catch (e) {}
  }

  // Candidate clips for (cat, pc): curated manifest URLs + local IndexedDB
  // uploads, with the shared DEFAULT set ('*') as fallback. Resilient — if
  // IndexedDB fails entirely, the manifest URLs still play.
  function _candidateUrls(cat, pc) {
    var key = cat + '/' + pc;
    var man = (_manifest[key] || []).map(function (m) { return { uid: 'R' + m.rid, url: m.url }; });
    var defMan = (_manifest[cat + '/*'] || []).map(function (m) { return { uid: 'R' + m.rid, url: m.url }; });
    // With the manifest loaded, IndexedDB contributes only LOCAL uploads (remote
    // clips come from the manifest URLs — avoid duplicates). Offline (manifest
    // empty), fall back to everything cached in IndexedDB.
    var ml = _manifestLoaded;
    function loc(items) {
      return (items || []).filter(function (it) { return !ml || it.source === 'local'; })
        .map(function (it) { return { uid: 'L' + it.id, id: it.id }; });
    }
    return list(cat, pc).then(function (items) {
      var all = man.concat(loc(items));
      if (all.length || pc === '*') return all;
      return list(cat, '*').then(function (d) { return defMan.concat(loc(d)); }, function () { return defMan; });
    }, function (err) {
      _warn('[CustomSounds] IndexedDB list failed — manifest only', err);
      return man.length ? man : defMan;
    });
  }

  // Play a random clip for (cat, piece): figure-specific → default set →
  // (caller's built-in fallback if this resolves false).
  function playRandom(cat, piece) {
    if (!isEnabled()) { _log('[CustomSounds] disabled'); return Promise.resolve(false); }
    var pc = (piece || '*').toUpperCase();
    return _candidateUrls(cat, pc).then(function (cands) {
      _log('[CustomSounds] playRandom', cat, pc, '→ candidates:', cands.length, 'manifestLoaded:', _manifestLoaded);
      if (!cands.length) return false;
      var key = cat + '/' + pc;
      var pick = cands;
      if (cands.length > 1 && _lastPlayed[key] != null) {
        var f = cands.filter(function (c) { return c.uid !== _lastPlayed[key]; });
        if (f.length) pick = f;
      }
      var chosen = pick[Math.floor(Math.random() * pick.length)];
      _lastPlayed[key] = chosen.uid;
      if (chosen.url) return _playUrl(chosen.url, cat + '/' + pc);
      return _getRec(chosen.id).then(function (rec) {
        if (!rec || !rec.blob) return false;
        var burl = URL.createObjectURL(rec.blob);
        var ok = _playUrl(burl, cat + '/' + pc + ' (local)');
        setTimeout(function () { try { URL.revokeObjectURL(burl); } catch (e) {} }, 30000);
        return ok;
      });
    }).catch(function (e) { _warn('[CustomSounds] playRandom error', e); return false; });
  }

  // Preview a specific local clip by id (management page).
  function preview(id) {
    return _getRec(id).then(function (rec) {
      if (!rec || !rec.blob) return false;
      var burl = URL.createObjectURL(rec.blob);
      var ok = _playUrl(burl, 'preview');
      setTimeout(function () { try { URL.revokeObjectURL(burl); } catch (e) {} }, 30000);
      return ok;
    });
  }

  global.CustomSounds = {
    PIECES: PIECES,
    CATS: CATS,
    init: _openDB,
    list: list,
    add: add,
    remove: remove,
    counts: counts,
    totalBytes: totalBytes,
    sync: sync,
    loadManifest: loadManifest,
    setManifestUrl: setManifestUrl,
    setAudioContext: setAudioContext,
    unlock: unlock,
    fadeOutCurrent: fadeOutCurrent,
    playRandom: playRandom,
    preview: preview,
    isEnabled: isEnabled,
    setEnabled: setEnabled
  };
})(window);
