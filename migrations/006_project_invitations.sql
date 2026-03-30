-- ============================================================================
-- Project Invitations and Applications
-- ============================================================================

CREATE TYPE invitation_kind_enum AS ENUM ('invite', 'application');
CREATE TYPE invitation_status_enum AS ENUM ('pending', 'approved', 'rejected', 'cancelled');

CREATE TABLE project_invitation (
    id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    project_id UUID NOT NULL REFERENCES task(id) ON DELETE CASCADE,
    inviter_user_id UUID REFERENCES app_user(id) ON DELETE SET NULL,
    invitee_user_id UUID REFERENCES app_user(id) ON DELETE CASCADE,
    invitee_email TEXT,
    kind invitation_kind_enum NOT NULL DEFAULT 'invite',
    status invitation_status_enum NOT NULL DEFAULT 'pending',
    created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    decided_at TIMESTAMPTZ,
    decided_by UUID REFERENCES app_user(id) ON DELETE SET NULL
);

CREATE INDEX idx_project_invitation_project_id ON project_invitation(project_id);
CREATE INDEX idx_project_invitation_invitee_user_id ON project_invitation(invitee_user_id);
CREATE INDEX idx_project_invitation_status ON project_invitation(status);

-- Add wanted_skills to task (used by projects to declare desired skills)
ALTER TABLE task ADD COLUMN IF NOT EXISTS wanted_skills TEXT[] DEFAULT '{}';
