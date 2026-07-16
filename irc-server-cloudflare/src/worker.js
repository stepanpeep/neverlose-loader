const JSON_HEADERS = {
  "content-type": "application/json; charset=utf-8",
  "cache-control": "no-store",
  "x-content-type-options": "nosniff",
};

const json = (value, status = 200) =>
  new Response(JSON.stringify(value), { status, headers: JSON_HEADERS });

const now = () => Date.now();

async function sha256(value) {
  const bytes = new TextEncoder().encode(value);
  const digest = await crypto.subtle.digest("SHA-256", bytes);
  return [...new Uint8Array(digest)]
    .map((byte) => byte.toString(16).padStart(2, "0"))
    .join("");
}

function cleanUsername(value) {
  if (typeof value !== "string") return "";
  return value
    .replace(/§./g, "")
    .replace(/[^\p{L}\p{N}_.\- ]/gu, "")
    .trim()
    .slice(0, 24);
}

function cleanMessage(value) {
  if (typeof value !== "string") return "";
  return value
    .replace(/[\u0000-\u001f\u007f]/g, " ")
    .replace(/\s+/g, " ")
    .trim()
    .slice(0, 240);
}

async function body(request) {
  const length = Number(request.headers.get("content-length") || 0);
  if (length > 4096) throw new Error("request too large");
  return request.json();
}

async function authenticate(request, env) {
  const header = request.headers.get("authorization") || "";
  if (!header.startsWith("Bearer ")) return null;
  const token = header.slice(7).trim();
  if (token.length < 32 || token.length > 128) return null;
  const tokenHash = await sha256(token);
  const timestamp = now();
  const session = await env.DB.prepare(
    "SELECT token_hash, username, expires_at FROM sessions WHERE token_hash = ? AND expires_at > ?"
  ).bind(tokenHash, timestamp).first();
  if (!session) return null;
  await env.DB.prepare("UPDATE sessions SET last_seen = ? WHERE token_hash = ?")
    .bind(timestamp, tokenHash).run();
  return session;
}

async function createSession(request, env) {
  let data;
  try {
    data = await body(request);
  } catch {
    return json({ error: "Invalid JSON" }, 400);
  }
  const username = cleanUsername(data.username);
  const clientVersion = String(data.clientVersion || "").slice(0, 24);
  if (username.length < 3) return json({ error: "Username is too short" }, 400);
  if (!clientVersion) return json({ error: "Client version is required" }, 400);

  const timestamp = now();
  const ip = request.headers.get("cf-connecting-ip") || "unknown";
  const ipHash = await sha256(`${ip}:${env.IP_SALT || "unset"}`);
  const count = await env.DB.prepare(
    "SELECT COUNT(*) AS amount FROM sessions WHERE ip_hash = ? AND expires_at > ?"
  ).bind(ipHash, timestamp).first();
  if (Number(count?.amount || 0) >= 8) return json({ error: "Too many active sessions" }, 429);

  const token = `${crypto.randomUUID().replaceAll("-", "")}${crypto.randomUUID().replaceAll("-", "")}`;
  const tokenHash = await sha256(token);
  const expiresAt = timestamp + 24 * 60 * 60 * 1000;
  await env.DB.prepare(
    "INSERT INTO sessions(token_hash, username, client_version, ip_hash, last_seen, expires_at, last_message_at) VALUES (?, ?, ?, ?, ?, ?, 0)"
  ).bind(tokenHash, username, clientVersion, ipHash, timestamp, expiresAt).run();
  return json({ token, username, expiresAt }, 201);
}

async function getMessages(request, env, session) {
  const url = new URL(request.url);
  const after = Math.max(0, Number.parseInt(url.searchParams.get("after") || "0", 10) || 0);
  const result = await env.DB.prepare(
    "SELECT id, author, content, timestamp FROM messages WHERE id > ? ORDER BY id ASC LIMIT 100"
  ).bind(after).all();
  return json(result.results || []);
}

async function postMessage(request, env, session) {
  let data;
  try {
    data = await body(request);
  } catch {
    return json({ error: "Invalid JSON" }, 400);
  }
  const content = cleanMessage(data.content);
  if (!content) return json({ error: "Message is empty" }, 400);

  const timestamp = now();
  const throttle = await env.DB.prepare(
    "UPDATE sessions SET last_message_at = ?, last_seen = ? WHERE token_hash = ? AND last_message_at <= ?"
  ).bind(timestamp, timestamp, session.token_hash, timestamp - 2000).run();
  if (!throttle.meta || throttle.meta.changes !== 1) {
    return json({ error: "Slow down" }, 429);
  }

  const insert = await env.DB.prepare(
    "INSERT INTO messages(author, content, timestamp) VALUES (?, ?, ?)"
  ).bind(session.username, content, timestamp).run();
  const id = Number(insert.meta?.last_row_id || 0);
  return json({ id, author: session.username, content, timestamp }, 201);
}

async function getOnline(env) {
  const cutoff = now() - 60_000;
  const result = await env.DB.prepare(
    "SELECT username, MAX(last_seen) AS lastSeen FROM sessions WHERE last_seen >= ? AND expires_at > ? GROUP BY username ORDER BY username COLLATE NOCASE LIMIT 200"
  ).bind(cutoff, now()).all();
  return json(result.results || []);
}

async function cleanup(env) {
  const timestamp = now();
  await env.DB.batch([
    env.DB.prepare("DELETE FROM sessions WHERE expires_at <= ?").bind(timestamp),
    env.DB.prepare("DELETE FROM messages WHERE timestamp < ?").bind(timestamp - 7 * 24 * 60 * 60 * 1000),
  ]);
}

export default {
  async fetch(request, env, context) {
    const url = new URL(request.url);
    if (!url.pathname.startsWith("/v1/")) return json({ error: "Not found" }, 404);
    if (request.method === "OPTIONS") return new Response(null, { status: 204 });

    try {
      if (url.pathname === "/v1/health" && request.method === "GET") {
        return json({ ok: true, service: "neverlose-irc", time: now() });
      }
      if (url.pathname === "/v1/session" && request.method === "POST") {
        context.waitUntil(cleanup(env));
        return createSession(request, env);
      }

      const session = await authenticate(request, env);
      if (!session) return json({ error: "Unauthorized" }, 401);

      if (url.pathname === "/v1/messages" && request.method === "GET") {
        return getMessages(request, env, session);
      }
      if (url.pathname === "/v1/messages" && request.method === "POST") {
        return postMessage(request, env, session);
      }
      if (url.pathname === "/v1/users/online" && request.method === "GET") {
        return getOnline(env);
      }
      return json({ error: "Not found" }, 404);
    } catch (error) {
      console.error(error);
      return json({ error: "Internal server error" }, 500);
    }
  },
};
