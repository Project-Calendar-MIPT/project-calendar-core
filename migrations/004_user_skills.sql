-- ============================================================================
-- User Skills
-- ============================================================================

CREATE TABLE user_skill (
    id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    user_id UUID NOT NULL REFERENCES app_user(id) ON DELETE CASCADE,
    skill_key TEXT NOT NULL,
    proficiency INT NOT NULL CHECK (proficiency BETWEEN 1 AND 5),
    updated_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    UNIQUE (user_id, skill_key)
);

CREATE INDEX idx_user_skill_user_id ON user_skill(user_id);
CREATE INDEX idx_user_skill_skill_key ON user_skill(skill_key);
