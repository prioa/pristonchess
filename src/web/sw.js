/* PristonChess service worker.
   The ESP32 web server stalls when a page fires many requests in parallel (it
   serves only a handful of simultaneous TCP connections; the surplus hang
   forever, so the page's `load` event never fires and "assets/data don't load").
   Fix: keep ALL static assets in the client cache so a page load hits the ESP32
   only for its small dynamic JSON. On install we PRECACHE the whole asset shell
   SEQUENTIALLY (one request at a time) so even warming the cache never floods
   the board.

   Routing:
   - Navigations / *.html → network-first (fresh when online, cache fallback).
   - Static assets (js/css/svg/png/woff2/...) → cache-first (no network once cached).
   - Dynamic endpoints (/board-update, /wifi, /learn/list, /games, /debug-*, POSTs)
     → NOT intercepted; the browser talks straight to the ESP32.

   NOTE: bump CACHE whenever web assets change so clients pick up the new files. */
const CACHE = 'pristonchess-v18';
const STATIC_RE = /\.(?:js|css|svg|png|jpg|jpeg|gif|webp|ico|woff2?|ttf|mp3|wav|ogg|json)$/i;

// Asset shell precached on install (one request at a time — see installer).
const PRECACHE = [
  './scripts/app.js',
  './scripts/jquery-4.0.0.min.js',
  './scripts/chess.js',
  './scripts/chessboard-1.0.0.min.js',
  './css/styles.css',
  './css/chessboard-1.0.0.min.css',
  './fonts/exo2-latin.woff2',
  './icon-512.png',
  './favicon.svg',
  './manifest.json',
  './pieces/wP.svg', './pieces/wN.svg', './pieces/wB.svg',
  './pieces/wR.svg', './pieces/wQ.svg', './pieces/wK.svg',
  './pieces/bP.svg', './pieces/bN.svg', './pieces/bB.svg',
  './pieces/bR.svg', './pieces/bQ.svg', './pieces/bK.svg',
];

self.addEventListener('install', (event) => {
  event.waitUntil((async () => {
    const cache = await caches.open(CACHE);
    // Sequential — never fire these in parallel at the connection-limited ESP32.
    for (const url of PRECACHE) {
      try {
        const res = await fetch(url, { cache: 'no-store' });
        if (res && res.ok) await cache.put(url, res);
      } catch (e) { /* keep going; lazy cache-first will fill any gap later */ }
    }
    await self.skipWaiting();
  })());
});

self.addEventListener('activate', (event) => {
  event.waitUntil((async () => {
    const keys = await caches.keys();
    await Promise.all(keys.filter((k) => k !== CACHE).map((k) => caches.delete(k)));
    await self.clients.claim();
  })());
});

function isNavigation(req) {
  if (req.mode === 'navigate') return true;
  const accept = req.headers.get('accept') || '';
  if (accept.includes('text/html')) return true;
  return /\.html$/i.test(new URL(req.url).pathname);
}

async function networkFirst(req) {
  try {
    const res = await fetch(req, { cache: 'no-store' });
    if (res && res.ok) {
      const cache = await caches.open(CACHE);
      cache.put(req, res.clone());
    }
    return res;
  } catch (e) {
    const cached = await caches.match(req);
    if (cached) return cached;
    const index = await caches.match('./index.html');
    return index || Response.error();
  }
}

// Cache-first: served from cache with no network call once present. A miss
// fetches once and stores it.
async function cacheFirst(req) {
  const cache = await caches.open(CACHE);
  const cached = await cache.match(req);
  if (cached) return cached;
  try {
    const res = await fetch(req);
    if (res && res.ok) cache.put(req, res.clone());
    return res;
  } catch (e) {
    return cached || Response.error();
  }
}

self.addEventListener('fetch', (event) => {
  const req = event.request;
  if (req.method !== 'GET') return;
  const url = new URL(req.url);
  if (url.origin !== self.location.origin) return;

  if (isNavigation(req)) { event.respondWith(networkFirst(req)); return; }
  if (STATIC_RE.test(url.pathname)) { event.respondWith(cacheFirst(req)); return; }
  // Dynamic endpoints (board-update, wifi, learn/list, games, debug-*, …) → untouched.
});
