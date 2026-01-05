/**
 * Remote Desktop Signaling Server
 * Cloudflare Worker for WebRTC signaling without port forwarding
 */

const CORS = {
  'Access-Control-Allow-Origin': '*',
  'Access-Control-Allow-Methods': 'GET, POST, OPTIONS',
  'Access-Control-Allow-Headers': 'Content-Type',
};

const json = (data, status = 200) =>
  new Response(JSON.stringify(data), {
    status,
    headers: { 'Content-Type': 'application/json', ...CORS }
  });

const ROOM_TTL = 3600; // 1 hour expiration

export default {
  async fetch(request, env) {
    if (request.method === 'OPTIONS') {
      return new Response(null, { status: 204, headers: CORS });
    }

    const url = new URL(request.url);
    const path = url.pathname;
    const KV = env.ROOMS;

    try {
      // ══════════════════════════════════════════════════════════════
      // HOST ENDPOINTS
      // ══════════════════════════════════════════════════════════════

      // Host poll (READ ONLY) - called every 1 second
      // GET /api/host/:hostId/poll
      if (path.match(/^\/api\/host\/([^\/]+)\/poll$/)) {
        const hostId = path.split('/')[3].toUpperCase();
        const room = await KV.get(hostId, 'json');

        if (!room) {
          return json({ status: 'waiting' });
        }

        if (room.offer && !room.offerClaimed) {
          return json({
            status: 'offer',
            offer: room.offer,
            clientIce: room.clientIce || [],
            sessionId: room.sessionId
          });
        }

        const lastIceSeen = parseInt(url.searchParams.get('lastIce') || '0');
        if (room.clientIce && room.clientIce.length > lastIceSeen) {
          return json({
            status: 'ice',
            clientIce: room.clientIce.slice(lastIceSeen),
            iceIndex: room.clientIce.length,
            sessionId: room.sessionId
          });
        }

        return json({ status: 'waiting' });
      }

      // Host sends answer (WRITE)
      // POST /api/host/:hostId/answer
      if (request.method === 'POST' && path.match(/^\/api\/host\/([^\/]+)\/answer$/)) {
        const hostId = path.split('/')[3].toUpperCase();
        const body = await request.json();
        let room = await KV.get(hostId, 'json') || {};

        room.offerClaimed = true;
        room.answer = body.answer;
        room.hostIce = body.ice || [];
        room.answerTime = Date.now();

        await KV.put(hostId, JSON.stringify(room), { expirationTtl: ROOM_TTL });
        return json({ success: true });
      }

      // Host sends ICE candidates (WRITE)
      // POST /api/host/:hostId/ice
      if (request.method === 'POST' && path.match(/^\/api\/host\/([^\/]+)\/ice$/)) {
        const hostId = path.split('/')[3].toUpperCase();
        const body = await request.json();
        let room = await KV.get(hostId, 'json') || {};

        room.hostIce = room.hostIce || [];
        if (Array.isArray(body.ice)) {
          room.hostIce.push(...body.ice);
        }

        await KV.put(hostId, JSON.stringify(room), { expirationTtl: ROOM_TTL });
        return json({ success: true, iceCount: room.hostIce.length });
      }

      // ══════════════════════════════════════════════════════════════
      // CLIENT ENDPOINTS
      // ══════════════════════════════════════════════════════════════

      // Client sends offer (WRITE)
      // POST /api/connect/:hostId
      if (request.method === 'POST' && path.match(/^\/api\/connect\/([^\/]+)$/)) {
        const hostId = path.split('/')[3].toUpperCase();
        const body = await request.json();

        const room = {
          offer: body.offer,
          offerClaimed: false,
          answer: null,
          clientIce: body.ice || [],
          hostIce: [],
          sessionId: crypto.randomUUID(),
          offerTime: Date.now()
        };

        await KV.put(hostId, JSON.stringify(room), { expirationTtl: ROOM_TTL });
        return json({ success: true, sessionId: room.sessionId });
      }

      // Client polls for answer (READ)
      // GET /api/client/:hostId/poll?session=xxx
      if (path.match(/^\/api\/client\/([^\/]+)\/poll$/)) {
        const hostId = path.split('/')[3].toUpperCase();
        const sessionId = url.searchParams.get('session');
        const lastHostIce = parseInt(url.searchParams.get('lastIce') || '0');

        const room = await KV.get(hostId, 'json');

        if (!room) {
          return json({ error: 'Room not found', code: 'NOT_FOUND' }, 404);
        }

        if (room.sessionId !== sessionId) {
          return json({ error: 'Session expired', code: 'SESSION_EXPIRED' }, 404);
        }

        if (room.answer) {
          const newHostIce = room.hostIce ? room.hostIce.slice(lastHostIce) : [];
          return json({
            status: 'answer',
            answer: room.answer,
            hostIce: newHostIce,
            iceIndex: room.hostIce ? room.hostIce.length : 0
          });
        }

        if (room.hostIce && room.hostIce.length > lastHostIce) {
          return json({
            status: 'waiting',
            hostIce: room.hostIce.slice(lastHostIce),
            iceIndex: room.hostIce.length
          });
        }

        return json({ status: 'waiting', hostIce: [], iceIndex: lastHostIce });
      }

      // Client sends ICE candidates (WRITE)
      // POST /api/client/:hostId/ice
      if (request.method === 'POST' && path.match(/^\/api\/client\/([^\/]+)\/ice$/)) {
        const hostId = path.split('/')[3].toUpperCase();
        const body = await request.json();
        let room = await KV.get(hostId, 'json');

        if (!room) return json({ error: 'Room not found' }, 404);

        room.clientIce = room.clientIce || [];
        if (Array.isArray(body.ice)) {
          room.clientIce.push(...body.ice);
        }

        await KV.put(hostId, JSON.stringify(room), { expirationTtl: ROOM_TTL });
        return json({ success: true, iceCount: room.clientIce.length });
      }

      // ══════════════════════════════════════════════════════════════
      // UTILITY
      // ══════════════════════════════════════════════════════════════

      if (path === '/health' || path === '/') {
        return json({ status: 'ok', timestamp: Date.now() });
      }

      return json({ error: 'Not found' }, 404);

    } catch (e) {
      console.error('Worker error:', e);
      return json({ error: 'Internal error', message: e.message }, 500);
    }
  }
};
