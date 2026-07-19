-- Applied automatically by the postgres container on first start (see
-- compose.yaml). Editing this file after the volume exists has no effect --
-- drop the volume or apply migrations by hand.

CREATE TABLE IF NOT EXISTS accounts (
    id            BIGSERIAL PRIMARY KEY,
    -- Case-insensitive so "Alex" and "alex" can't both be registered.
    username      TEXT NOT NULL UNIQUE,
    -- Full Argon2id encoded string from libsodium: contains the algorithm,
    -- parameters and salt, so no separate salt column is needed.
    password_hash TEXT NOT NULL,
    created_at    TIMESTAMPTZ NOT NULL DEFAULT now(),
    last_login_at TIMESTAMPTZ
);

CREATE UNIQUE INDEX IF NOT EXISTS accounts_username_lower_idx ON accounts (lower(username));

-- Short-lived handoff tokens: the auth server issues one, the world server
-- redeems it. Deliberately not a long-lived credential.
CREATE TABLE IF NOT EXISTS sessions (
    token      TEXT PRIMARY KEY,
    account_id BIGINT NOT NULL REFERENCES accounts (id) ON DELETE CASCADE,
    created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    expires_at TIMESTAMPTZ NOT NULL
);

CREATE INDEX IF NOT EXISTS sessions_account_idx ON sessions (account_id);
CREATE INDEX IF NOT EXISTS sessions_expires_idx ON sessions (expires_at);
