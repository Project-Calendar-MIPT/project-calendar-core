# Backend Full Implementation Design
**Date:** 2026-03-20
**Stack:** C++23 · Drogon · PostgreSQL 16 · Docker · JWT Bearer

---

## Context

The backend is a C++ Drogon monolith. The orchestration spec (originally written for Spring Boot/Node.js) has been adapted for this stack. Execution follows **Option B: strict sequential** to avoid file conflicts in a C++ monolith.

---

## Phase 0 — Build Fix (prerequisite)

**Problem:** `ProjectController.cpp` and `TaskController.cpp` include `.hpp` headers that don't exist. All model headers and API headers use `.h`. The project does not currently compile.

**Fix:**
- Add `back/src/API/ProjectController.h` declaring all existing + new project routes
- Fix all `#include "API/*.hpp"` → `#include "API/*.h"` in every `.cpp`
- Fix all `#include "models/*.hpp"` → `#include "models/*.h"` in every `.cpp`

---

## Phase 1 — Agent 1: Verification

Audit existing endpoints. Mark ✅ PASS / ⚠️ PARTIAL / ❌ FAIL, fix inline.

### Tasks checklist:
- POST /api/tasks — description required, estimated_hours >= 0
- Server-side date validation: task dates within parent project dates (PUT + POST)
- Subtasks CRUD
- Task notes: POST/GET /api/tasks/{id}/notes, PUT/DELETE /api/notes/{id}
- PUT /api/tasks/{id} — all fields editable
- Status auto-transition: when dates set → status = "in_progress"
- Tasks split by has_time (start_date/due_date present)
- GET /api/users/{id}/workload — exists and works
- Candidate sorting by skill relevance
- POST /api/projects — description required, single owner enforced
- visibility field filterable in GET /api/projects
- GET /api/projects/{id}/progress — % completed tasks
- GET /api/projects/{id}/participants — role filter + sort

---

## Phase 2 — Agent 3: Bug Fixes

### B.1 — Owner shown as "Unknown"
- **Root cause:** `owner_id` taken from request body instead of JWT
- **Fix:** Always set `created_by = requesterId` from JWT; ignore body `owner_id`
- **Also:** GET /api/projects/:id response must join owner name + email

### B.3 — Task dates exceed project range
- **Root cause:** Date validation may be missing on PUT
- **Fix:** `validateDatesAgainstProject()` must be called in both POST and PUT handlers; return HTTP 422 on failure

### B.6 — Unauthorized member changes
- **Root cause:** Member mutation endpoints lack role-based authorization
- **Fix:** Check caller has OWNER or ADMIN role on project before mutating; return HTTP 403 with `{"error": "Insufficient permissions"}`

---

## Phase 3 — Agent 2: New Features

### Group A — ProjectController additions

**New routes:**
- `DELETE /api/projects/{project_id}` — owner only, CASCADE deletes all tasks
- `PUT /api/projects/{project_id}` — edit name, description, dates, visibility, wanted_skills
- `POST /api/projects/{project_id}/invite` — owner invites user by user_id or email
- `POST /api/projects/{project_id}/apply` — authenticated user requests to join
- `POST /api/projects/{project_id}/applications/{application_id}/approve`
- `POST /api/projects/{project_id}/applications/{application_id}/reject`
- `GET /api/projects/{project_id}/applications` — owner views pending applications

**New migration:** `006_project_invitations.sql`
```sql
CREATE TYPE invitation_status_enum AS ENUM ('pending', 'approved', 'rejected', 'cancelled');
CREATE TABLE project_invitation (
    id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    project_id UUID NOT NULL REFERENCES task(id) ON DELETE CASCADE,
    inviter_user_id UUID REFERENCES app_user(id),
    invitee_user_id UUID REFERENCES app_user(id),
    invitee_email TEXT,
    kind TEXT NOT NULL DEFAULT 'invite', -- 'invite' | 'application'
    status invitation_status_enum NOT NULL DEFAULT 'pending',
    created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    decided_at TIMESTAMPTZ
);
```

**wanted_skills:** Add `wanted_skills TEXT[]` column to `task` table via migration, store/return in project endpoints.

### Group B — FeedController (new file)

- `GET /api/projects/recommended` — projects where `wanted_skills ∩ user.skills` > 0, sorted by match score
- `GET /api/feed` — recent project updates, new members, completed tasks; paginated newest-first
- `GET /api/tasks/deadlines` — tasks assigned to me with due_date within next 7 days, sorted ascending

### Group C — RBAC

Role hierarchy from existing `role_enum`: `owner > admin > supervisor > hybrid > executor > spectator`

**Permission helper (inline function, not a separate middleware file):**
```cpp
bool hasProjectRole(dbClient, projectId, userId, minRole)
```
Queries `task_role_assignment` for the user's role on the project, returns true if role >= minRole.

Apply to:
- All member mutation endpoints → require OWNER or ADMIN
- DELETE project → require OWNER
- PUT project → require OWNER or ADMIN
- Invite/approve/reject → require OWNER

### Group D — TeamController (new file)

**New migration:** `007_teams.sql`
```sql
CREATE TABLE team (
    id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    name TEXT NOT NULL,
    description TEXT,
    owner_user_id UUID NOT NULL REFERENCES app_user(id),
    parent_team_id UUID REFERENCES team(id),
    created_at TIMESTAMPTZ NOT NULL DEFAULT NOW()
);
CREATE TABLE team_member (
    id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    team_id UUID NOT NULL REFERENCES team(id) ON DELETE CASCADE,
    user_id UUID NOT NULL REFERENCES app_user(id) ON DELETE CASCADE,
    UNIQUE(team_id, user_id)
);
CREATE TABLE team_task_assignment (
    team_id UUID NOT NULL REFERENCES team(id) ON DELETE CASCADE,
    task_id UUID NOT NULL REFERENCES task(id) ON DELETE CASCADE,
    PRIMARY KEY (team_id, task_id)
);
CREATE TABLE team_project_assignment (
    team_id UUID NOT NULL REFERENCES team(id) ON DELETE CASCADE,
    project_id UUID NOT NULL REFERENCES task(id) ON DELETE CASCADE,
    PRIMARY KEY (team_id, project_id)
);
```

**New routes:**
- `POST /api/teams` — create team
- `GET /api/teams/{team_id}` — team details with members
- `PUT /api/teams/{team_id}` — edit team (name, description, parent_team_id)
- `POST /api/teams/{team_id}/members` — add member
- `DELETE /api/teams/{team_id}/members/{user_id}` — remove member
- `POST /api/tasks/{task_id}/assign-team` — assign team to task
- `POST /api/projects/{project_id}/assign-team` — assign team to project
- `GET /api/projects?scope=all|mine|mine_and_teams` — scope filter on list

---

## Phase 4 — Integration & Report

1. Verify no duplicate routes in all controller headers
2. Confirm JWT extracted from attributes everywhere (no body `owner_id`)
3. Update `back/docs/openapi.yaml` with all new routes and schemas
4. Write `AGENT_REPORT.md` at repo root

---

## File Inventory

### New files:
- `back/src/API/ProjectController.h` (missing header)
- `back/src/API/FeedController.h` + `FeedController.cpp`
- `back/src/API/TeamController.h` + `TeamController.cpp`
- `back/migrations/006_project_invitations.sql`
- `back/migrations/007_teams.sql`
- `AGENT_REPORT.md`

### Modified files:
- `back/src/API/ProjectController.cpp` — fix includes, add new routes
- `back/src/API/TaskController.cpp` — fix includes, verify validation
- `back/src/API/TaskController.h` — add any missing route declarations
- `back/docs/openapi.yaml` — add all new routes + schemas

---

## Constraints

- Never read `owner_id` from request body for ownership — always from JWT
- All datetime comparisons use `DATE` string comparison (`YYYY-MM-DD`) consistent with existing code
- Use `execSqlSync` + `Mapper` pattern matching existing controllers
- Error responses: plain string body (existing style), HTTP status codes as specified
- Every new public endpoint must have `"AuthFilter"` in route declaration
