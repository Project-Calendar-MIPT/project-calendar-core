-- ============================================================================
-- Add project visibility metadata
-- ============================================================================

DO $$
BEGIN
  IF NOT EXISTS (
    SELECT 1
    FROM pg_type
    WHERE typname = 'project_visibility_enum'
  ) THEN
    CREATE TYPE project_visibility_enum AS ENUM ('public', 'private');
  END IF;
END
$$;

CREATE TABLE IF NOT EXISTS project_visibility (
  project_id UUID PRIMARY KEY REFERENCES task(id) ON DELETE CASCADE,
  visibility project_visibility_enum NOT NULL DEFAULT 'private',
  created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
  updated_at TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

INSERT INTO project_visibility (project_id, visibility)
SELECT t.id, 'private'::project_visibility_enum
FROM task t
LEFT JOIN project_visibility pv ON pv.project_id = t.id
WHERE t.parent_task_id IS NULL
  AND pv.project_id IS NULL;

CREATE INDEX IF NOT EXISTS idx_project_visibility_visibility
  ON project_visibility(visibility);
