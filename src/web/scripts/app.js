/* PristonChess — shared front-end helpers (loaded in <head> on every page).
   Keep this dependency-free and side-effect-free apart from defining window.PC. */

/* Page preloader — a pulsing king (♚) with "Lädt…", shown until the page has
   finished loading. Injected from the <head> so it covers every screen during
   load without editing each HTML file. */
(function () {
  var css =
    '#pc-preloader{position:fixed;inset:0;z-index:99999;display:flex;flex-direction:column;' +
    'align-items:center;justify-content:center;gap:16px;background:var(--color-bg,#0B0E14);' +
    'transition:opacity .35s ease;}' +
    '#pc-preloader.pc-hide{opacity:0;pointer-events:none;}' +
    '#pc-preloader .pc-king{font-size:64px;line-height:1;color:var(--color-accent,#4D8CFF);' +
    'animation:pc-pulse 1.15s ease-in-out infinite;text-shadow:0 6px 20px rgba(77,140,255,.35);}' +
    '#pc-preloader .pc-load{font:600 13px/1 var(--font-sans,system-ui,-apple-system,sans-serif);' +
    'letter-spacing:.16em;text-transform:uppercase;color:var(--color-text-muted,#9ba3b4);}' +
    '@keyframes pc-pulse{0%,100%{transform:scale(.9);opacity:.55;}50%{transform:scale(1.1);opacity:1;}}' +
    '@media (prefers-reduced-motion:reduce){#pc-preloader .pc-king{animation:none;}}';
  var style = document.createElement('style');
  style.textContent = css;
  (document.head || document.documentElement).appendChild(style);

  var el = document.createElement('div');
  el.id = 'pc-preloader';
  el.setAttribute('aria-hidden', 'true');
  el.innerHTML = '<div class="pc-king">♚</div><div class="pc-load">Lädt…</div>';
  (document.body || document.documentElement).appendChild(el);

  function hidePreloader() {
    if (!el) return;
    el.classList.add('pc-hide');
    var dead = el;
    setTimeout(function () { if (dead && dead.parentNode) dead.parentNode.removeChild(dead); }, 450);
    el = null;
  }
  // Hide as soon as the DOM is parsed — NOT on 'load', which also waits for
  // external subresources (Google Fonts). On an AP-only / no-internet client the
  // font request stalls, so gating on 'load' kept the preloader up for seconds
  // on every page. DOMContentLoaded reveals the page immediately; fonts swap in.
  if (document.readyState !== 'loading') hidePreloader();
  else document.addEventListener('DOMContentLoaded', hidePreloader);
  setTimeout(hidePreloader, 1500); // safety net
})();

(function (global) {
  'use strict';

  /** fetch + parse JSON with no-store caching. Throws on a non-OK response. */
  async function fetchJSON(url, opts) {
    const r = await fetch(url, Object.assign({ cache: 'no-store' }, opts || {}));
    if (!r.ok) throw new Error('HTTP ' + r.status + ' for ' + url);
    return r.json();
  }

  /**
   * Refresh the app-header WiFi pill (#wifiStatusPill) from /wifi.
   * Also fills the optional settings fields #sysWifi / #sysIp when present.
   * No-ops when the pill is absent; never throws. Returns the /wifi payload
   * (or null on error).
   */
  async function refreshWifiPill() {
    const pill = document.getElementById('wifiStatusPill');
    if (!pill) return null;
    try {
      const j = await fetchJSON('./wifi');
      const connected = j.connected || j.status === 'connected' || j.ssid;
      const ssid = j.ssid || j.connectedSsid || '';
      const ip   = j.ip   || j.localIp || j.address || '';
      pill.classList.remove('ok', 'warn', 'danger');
      if (connected && ip) {
        pill.classList.add('ok');
        pill.textContent = ssid ? 'WiFi · ' + ssid : 'WiFi';
      } else {
        pill.classList.add('warn');
        pill.textContent = ssid ? 'AP · ' + ssid : 'AP';
      }
      const sw = document.getElementById('sysWifi');
      const si = document.getElementById('sysIp');
      if (sw) sw.textContent = ssid || '(none)';
      if (si) si.textContent = ip   || '—';
      return j;
    } catch (e) {
      pill.classList.remove('ok', 'warn');
      pill.classList.add('danger');
      pill.textContent = 'WiFi ?';
      return null;
    }
  }

  /** Immediate refresh + repeating interval (default 10s). Returns timer id. */
  function startWifiPill(intervalMs) {
    refreshWifiPill();
    return setInterval(refreshWifiPill, intervalMs || 10000);
  }

  global.PC = { fetchJSON: fetchJSON, refreshWifiPill: refreshWifiPill, startWifiPill: startWifiPill };

  // Move-highlight colours (chosen in settings, stored in localStorage). Applied
  // as :root CSS variables so the board highlights pick them up on every page.
  function hexToRgba(hex, a) {
    const m = /^#?([0-9a-f]{6})$/i.exec(hex || '');
    if (!m) return hex;
    const n = parseInt(m[1], 16);
    return 'rgba(' + ((n >> 16) & 255) + ',' + ((n >> 8) & 255) + ',' + (n & 255) + ',' + a + ')';
  }
  global.PC.applyHighlightColors = function () {
    let c = null;
    try { c = JSON.parse(localStorage.getItem('pcHlColors') || 'null'); } catch (e) {}
    if (!c) return;
    const s = document.documentElement.style;
    if (c.source)  s.setProperty('--pc-source',  hexToRgba(c.source, 0.5));
    if (c.target)  s.setProperty('--pc-target',  hexToRgba(c.target, 0.4));
    if (c.invalid) s.setProperty('--pc-invalid', hexToRgba(c.invalid, 0.7));
    if (c.valid)   s.setProperty('--pc-valid',   hexToRgba(c.valid, 0.78));
  };
  global.PC.applyHighlightColors();
})(window);

/* PWA wiring — injected here so every page is installable/offline-capable
   without editing each HTML <head>. Runs from the <head> script, so the
   manifest/theme links land before the body is parsed. */
(function () {
  var head = document.head;
  // CD headline font (Exo 2) is now self-hosted via @font-face in styles.css —
  // no external Google Fonts request anymore.
  if (head && !document.querySelector('link[rel="manifest"]')) {
    var manifest = document.createElement('link');
    manifest.rel = 'manifest';
    manifest.href = './manifest.json';
    head.appendChild(manifest);

    var theme = document.createElement('meta');
    theme.name = 'theme-color';
    theme.content = '#0B0E14';
    head.appendChild(theme);

    var apple = document.createElement('link');
    apple.rel = 'apple-touch-icon';
    apple.href = './icon-512.png';
    head.appendChild(apple);
  }
  if ('serviceWorker' in navigator) {
    window.addEventListener('load', function () {
      navigator.serviceWorker.register('./sw.js').catch(function () {});
    });
  }
})();
