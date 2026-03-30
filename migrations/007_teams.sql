-- ============================================================================
-- Teams
-- ============================================================================

CREATE TABLE team (
    id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    name TEXT NOT NULL,
    description TEXT,
    owner_user_id UUID NOT NULL REFERENCES app_user(id) ON DELETE RESTRICT,
    parent_team_id UUID REFERENCES team(id) ON DELETE SET NULL,
    created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    updated_at TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

CREATE INDEX idx_team_owner_user_id ON team(owner_user_id);
CREATE INDEX idx_team_parent_team_id ON team(parent_team_id);

CREATE TABLE team_member (
    id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    team_id UUID NOT NULL REFERENCES team(id) ON DELETE CASCADE,
    user_id UUID NOT NULL REFERENCES app_user(id) ON DELETE CASCADE,
    added_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    UNIQUE(team_id, user_id)
);

CREATE INDEX idx_team_member_team_id ON team_member(team_id);
CREATE INDEX idx_team_member_user_id ON team_member(user_id);

CREATE TABLE team_task_assignment (
    team_id UUID NOT NULL REFERENCES team(id) ON DELETE CASCADE,
    task_id UUID NOT NULL REFERENCES task(id) ON DELETE CASCADE,
    assigned_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    PRIMARY KEY (team_id, task_id)
);

CREATE INDEX idx_team_task_assignment_team_id ON team_task_assignment(team_id);
CREATE INDEX idx_team_task_assignment_task_id ON team_task_assignment(task_id);

CREATE TABLE team_project_assignment (
    team_id UUID NOT NULL REFERENCES team(id) ON DELETE CASCADE,
    project_id UUID NOT NULL REFERENCES task(id) ON DELETE CASCADE,
    assigned_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    PRIMARY KEY (team_id, project_id)
);

CREATE INDEX idx_team_project_assignment_team_id ON team_project_assignment(team_id);
CREATE INDEX idx_team_project_assignment_project_id ON team_project_assignment(project_id);
