// Present so the page can be added to the home screen. It caches nothing: the network answers every
// request, so nothing shown can be stale and no reply is ever kept on the phone.
self.addEventListener('install', () => self.skipWaiting());
self.addEventListener('activate', (event) => event.waitUntil(self.clients.claim()));
self.addEventListener('fetch', () => {});
