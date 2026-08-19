const CACHE_PREFIX = 'hexenwail-pwa-';
const CACHE_VERSION = `${CACHE_PREFIX}__HEXENWAIL_BUILD_VERSION__`;
const CORE_ASSETS = [
  './',
  './index.html',
  './app.js',
  './lib/phone-controls.js',
  './manifest.webmanifest',
  './sw.js',
  './icons/icon.svg',
  './icons/icon-180.png',
  './icons/icon-192.png',
  './icons/icon-512.png',
  './hexenwail.js',
  './hexenwail.wasm',
];
const CORE_ASSET_URLS = CORE_ASSETS.map((asset) => new URL(asset, self.location).href);
const INDEX_URL = new URL('./index.html', self.location).href;

self.addEventListener('install', (event) => {
  event.waitUntil((async () => {
    const cache = await caches.open(CACHE_VERSION);
    await cache.addAll(CORE_ASSET_URLS.map((url) => new Request(url, { cache: 'reload' })));
    await self.skipWaiting();
  })());
});

self.addEventListener('activate', (event) => {
  event.waitUntil((async () => {
    const keys = await caches.keys();
    await Promise.all(keys
      .filter((key) => key.startsWith(CACHE_PREFIX) && key !== CACHE_VERSION)
      .map((key) => caches.delete(key)));
    await self.clients.claim();
  })());
});

self.addEventListener('fetch', (event) => {
  const request = event.request;
  if (request.method !== 'GET') {
    return;
  }

  const url = new URL(request.url);
  if (request.mode === 'navigate') {
    event.respondWith((async () => {
      try {
        const response = await fetch(request);
        const cache = await caches.open(CACHE_VERSION);
        cache.put(INDEX_URL, response.clone());
        return response;
      } catch {
        const cache = await caches.open(CACHE_VERSION);
        return cache.match(INDEX_URL);
      }
    })());
    return;
  }

  if (url.origin !== self.location.origin) {
    return;
  }
  if (!CORE_ASSET_URLS.includes(request.url)) {
    return;
  }

  event.respondWith((async () => {
    const cached = await caches.match(request);
    if (cached) {
      return cached;
    }

    const response = await fetch(request);
    if (response.ok) {
      const cache = await caches.open(CACHE_VERSION);
      await cache.put(request, response.clone());
    }
    return response;
  })());
});
