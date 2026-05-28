/* PristonChess service worker.
   Goal: serve the static shell from the client cache so the ESP32 only has to
   answer the small dynamic JSON/SSE requests, and keep the UI usable offline.

   Routing:
   - Navigations / *.html → network-first (always fresh when online, cache
     fallback offline). This is what fixes stale pages after a redeploy.
   - Static assets (js/css/svg/png/sounds/manifest) → cache-first: served straight
     from the cache with NO network round-trip, so heavy use never piles up
     background revalidation requests on the single-threaded ESP32. A new asset
     version lands when CACHE is bumped (the activate handler purges the old cache).
   - Everything else (/board-update, /events SSE, /wifi, /debug-*, POSTs, …)
     is NOT intercepted at all → the browser talks straight to the ESP32.

   NOTE: bump CACHE whenever web assets change so clients pick up the new files. */
const CACHE = 'pristonchess-v2';
const STATIC_RE = /\.(?:js|css|svg|png|jpg|jpeg|gif|webp|ico|woff2?|ttf|mp3|wav|ogg|json)$/i;

self.addEventListener('install', () => {
  self.skipWaiting();
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

// Cache-first: once an asset is in the cache it is served with no network call,
// so navigating around never generates background traffic to the ESP32. A cache
// miss (first load, or after a CACHE bump) fetches once and stores it.
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
  // Dynamic endpoints (board-update, events, wifi, debug-*, …) → untouched.
});
