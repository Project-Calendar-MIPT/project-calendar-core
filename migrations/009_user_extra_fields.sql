ALTER TABLE app_user
  ADD COLUMN IF NOT EXISTS timezone           TEXT DEFAULT 'Europe/Moscow',
  ADD COLUMN IF NOT EXISTS contacts_visible   BOOLEAN DEFAULT TRUE,
  ADD COLUMN IF NOT EXISTS experience_level   TEXT CHECK (experience_level IN ('junior', 'middle', 'senior'));

CREATE TABLE IF NOT EXISTS user_skill (
  id               UUID PRIMARY KEY DEFAULT gen_random_uuid(),
  user_id          UUID NOT NULL REFERENCES app_user(id) ON DELETE CASCADE,
  name             TEXT NOT NULL,
  experience_level TEXT CHECK (experience_level IN ('junior', 'middle', 'senior')),
  UNIQUE (user_id, name)
);
