ALTER TABLE task ADD COLUMN IF NOT EXISTS is_private BOOLEAN NOT NULL DEFAULT FALSE;

CREATE TABLE IF NOT EXISTS meeting (
    id            UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    title         TEXT NOT NULL,
    description   TEXT,
    organizer_id  UUID NOT NULL REFERENCES app_user(id) ON DELETE CASCADE,
    start_at      TIMESTAMPTZ NOT NULL,
    duration_min  INTEGER NOT NULL DEFAULT 60,
    meeting_url   TEXT,
    location      TEXT,
    created_at    TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

CREATE TABLE IF NOT EXISTS meeting_participant (
    meeting_id  UUID NOT NULL REFERENCES meeting(id) ON DELETE CASCADE,
    user_id     UUID NOT NULL REFERENCES app_user(id) ON DELETE CASCADE,
    PRIMARY KEY (meeting_id, user_id)
);

CREATE INDEX IF NOT EXISTS idx_meeting_organizer ON meeting(organizer_id);
CREATE INDEX IF NOT EXISTS idx_meeting_start_at  ON meeting(start_at);
CREATE INDEX IF NOT EXISTS idx_meeting_participant_user ON meeting_participant(user_id);
