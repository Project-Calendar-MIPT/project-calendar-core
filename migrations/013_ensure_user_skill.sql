CREATE TABLE IF NOT EXISTS user_skill (
  id               UUID PRIMARY KEY DEFAULT gen_random_uuid(),
  user_id          UUID NOT NULL REFERENCES app_user(id) ON DELETE CASCADE,
  name             TEXT NOT NULL,
  experience_level TEXT CHECK (experience_level IN ('junior', 'middle', 'senior')),
  UNIQUE (user_id, name)
);
