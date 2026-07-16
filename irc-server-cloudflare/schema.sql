CREATE TABLE IF NOT EXISTS sessions (
  token_hash TEXT PRIMARY KEY,
  username TEXT NOT NULL,
  client_version TEXT NOT NULL,
  ip_hash TEXT NOT NULL,
  last_seen INTEGER NOT NULL,
  expires_at INTEGER NOT NULL,
  last_message_at INTEGER NOT NULL DEFAULT 0,
  role TEXT NOT NULL DEFAULT 'USER' CHECK(role IN ('USER', 'DEV'))
);

CREATE INDEX IF NOT EXISTS idx_sessions_online
  ON sessions(last_seen, expires_at);
CREATE INDEX IF NOT EXISTS idx_sessions_ip
  ON sessions(ip_hash, expires_at);

CREATE TABLE IF NOT EXISTS messages (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  author TEXT NOT NULL,
  content TEXT NOT NULL,
  timestamp INTEGER NOT NULL,
  role TEXT NOT NULL DEFAULT 'USER' CHECK(role IN ('USER', 'DEV'))
);

CREATE INDEX IF NOT EXISTS idx_messages_timestamp
  ON messages(timestamp);
