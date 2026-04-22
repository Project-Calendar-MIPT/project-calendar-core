-- If user_skill exists with wrong schema (missing 'name' column), recreate it.
-- Stack registration never worked, so there is no data to preserve.
DO $$
BEGIN
    IF EXISTS (SELECT 1 FROM information_schema.tables WHERE table_name = 'user_skill')
       AND NOT EXISTS (SELECT 1 FROM information_schema.columns
                       WHERE table_name = 'user_skill' AND column_name = 'name') THEN
        DROP TABLE user_skill;
        CREATE TABLE user_skill (
            id               UUID PRIMARY KEY DEFAULT gen_random_uuid(),
            user_id          UUID NOT NULL REFERENCES app_user(id) ON DELETE CASCADE,
            name             TEXT NOT NULL,
            experience_level TEXT CHECK (experience_level IN ('junior', 'middle', 'senior')),
            UNIQUE (user_id, name)
        );
    END IF;
END $$;
