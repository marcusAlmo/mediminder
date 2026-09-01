// Mediminder Service Worker
// Provides offline support, caching, and background sync

const CACHE_NAME = 'mediminder-v2';
const RUNTIME_CACHE = 'mediminder-runtime-v1';
const API_CACHE = 'mediminder-api-v1';

const STATIC_ASSETS = [
  '/',
  '/dashboard',
  '/static/style.css',
  '/static/manifest.json',
  '/static/offline.html',
  '/static/icon.png',
  '/static/icon-192x192.png',
  '/static/icon-512x512.png',
  '/static/apple-touch-icon.png'
];

// Install event - cache static assets
self.addEventListener('install', (event) => {
  console.log('[SW] Installing Service Worker');
  event.waitUntil(
    caches.open(CACHE_NAME).then((cache) => {
      console.log('[SW] Caching static assets');
      return cache.addAll(STATIC_ASSETS).catch((err) => {
        console.log('[SW] Some assets failed to cache:', err);
        // Continue even if some assets fail
        return Promise.resolve();
      });
    })
  );
  self.skipWaiting();
});

// Activate event - clean up old caches
self.addEventListener('activate', (event) => {
  console.log('[SW] Activating Service Worker');
  event.waitUntil(
    caches.keys().then((cacheNames) => {
      return Promise.all(
        cacheNames.map((cacheName) => {
          if (cacheName !== CACHE_NAME && 
              cacheName !== RUNTIME_CACHE && 
              cacheName !== API_CACHE) {
            console.log('[SW] Deleting old cache:', cacheName);
            return caches.delete(cacheName);
          }
        })
      );
    })
  );
  self.clients.claim();
});

// Fetch event - implement caching strategies
self.addEventListener('fetch', (event) => {
  const { request } = event;
  const url = new URL(request.url);

  // Skip non-GET requests
  if (request.method !== 'GET') {
    return;
  }

  // API requests - Network first, fallback to cache
  if (url.pathname.startsWith('/api/')) {
    event.respondWith(networkFirstStrategy(request));
    return;
  }

  // Static assets - Cache first, fallback to network
  if (url.pathname.startsWith('/static/')) {
    event.respondWith(cacheFirstStrategy(request));
    return;
  }

  // HTML pages - Network first, fallback to cache
  if (request.headers.get('accept')?.includes('text/html')) {
    event.respondWith(networkFirstStrategy(request));
    return;
  }

  // Default - Network first
  event.respondWith(networkFirstStrategy(request));
});

// Network first strategy - try network, fallback to cache
async function networkFirstStrategy(request) {
  try {
    const response = await fetch(request);
    
    // Cache successful responses
    if (response.ok) {
      const cache = await caches.open(
        request.url.includes('/api/') ? API_CACHE : RUNTIME_CACHE
      );
      cache.put(request, response.clone());
    }
    
    return response;
  } catch (error) {
    console.log('[SW] Network request failed, trying cache:', request.url);
    
    // Try to get from cache
    const cached = await caches.match(request);
    if (cached) {
      return cached;
    }

    // Return offline page for HTML requests
    if (request.headers.get('accept')?.includes('text/html')) {
      const offlineCache = await caches.open(CACHE_NAME);
      return offlineCache.match('/static/offline.html') || 
             new Response('Offline - Please check your connection', {
               status: 503,
               statusText: 'Service Unavailable',
               headers: new Headers({
                 'Content-Type': 'text/plain'
               })
             });
    }

    // Return error response
    return new Response('Network request failed', {
      status: 503,
      statusText: 'Service Unavailable'
    });
  }
}

// Cache first strategy - try cache, fallback to network
async function cacheFirstStrategy(request) {
  const cached = await caches.match(request);
  if (cached) {
    return cached;
  }

  try {
    const response = await fetch(request);
    
    if (response.ok) {
      const cache = await caches.open(RUNTIME_CACHE);
      cache.put(request, response.clone());
    }
    
    return response;
  } catch (error) {
    console.log('[SW] Cache and network failed:', request.url);
    return new Response('Resource not available', {
      status: 503,
      statusText: 'Service Unavailable'
    });
  }
}

// Handle messages from clients
self.addEventListener('message', (event) => {
  console.log('[SW] Message received:', event.data);

  if (event.data.type === 'SKIP_WAITING') {
    self.skipWaiting();
  }

  if (event.data.type === 'CLEAR_CACHE') {
    caches.keys().then((cacheNames) => {
      Promise.all(
        cacheNames.map((cacheName) => caches.delete(cacheName))
      );
    });
  }
});

// Background sync for offline data
self.addEventListener('sync', (event) => {
  console.log('[SW] Background sync event:', event.tag);

  if (event.tag === 'sync-logs') {
    event.waitUntil(syncLogs());
  }
});

async function syncLogs() {
  try {
    // This would sync any pending logs when connection returns
    console.log('[SW] Syncing logs...');
    // Implementation depends on your offline data storage
  } catch (error) {
    console.log('[SW] Sync failed:', error);
  }
}

console.log('[SW] Service Worker loaded');
