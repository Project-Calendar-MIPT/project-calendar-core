# API Specification

Base URL: `/api`

Authentication:
- Protected endpoints require `Authorization: Bearer <jwt>`.
- Error format is plain JSON string or object depending on endpoint implementation. Do not assume a single error envelope.

## Authentication

### POST `/auth/register`
Registers a user and initial work schedule.

Request body:
```json
{
  "email": "user@example.com",
  "password": "TestPassword123!",
  "display_name": "Test User",
  "name": "Test",
  "surname": "User",
  "phone": "+123456789",
  "telegram": "@test",
  "locale": "ru",
  "work_schedule": [
    { "weekday": 1, "start_time": "09:00:00", "end_time": "18:00:00" }
  ]
}
```

Rules:
- Required fields: `email`, `password`, `display_name`, `work_schedule`.
- `password` must be at least 8 characters.
- `work_schedule` must be a non-empty array.

Success response: `201 Created`
```json
{
  "success": true,
  "token": "<jwt>",
  "user": {
    "id": "uuid",
    "email": "user@example.com",
    "display_name": "Test User",
    "name": "Test",
    "surname": "User"
  },
  "work_schedule": []
}
```

### POST `/auth/login`

Request body:
```json
{
  "email": "user@example.com",
  "password": "TestPassword123!"
}
```

Success response: `200 OK`
```json
{
  "token": "<jwt>",
  "user": {
    "id": "uuid",
    "display_name": "Test User",
    "email": "user@example.com"
  }
}
```

### GET `/auth/me`
Returns current authenticated user.

Success response: `200 OK`
```json
{
  "id": "uuid",
  "display_name": "Test User",
  "email": "user@example.com",
  "created_at": "...",
  "updated_at": "..."
}
```

## Projects

### POST `/projects`
Creates a top-level project.

Request body:
```json
{
  "title": "Project title",
  "description": "Project description",
  "priority": "high",
  "status": "open",
  "estimated_hours": 120,
  "start_date": "2026-03-14",
  "due_date": "2026-04-01",
  "visibility": "private",
  "owner_user_id": "uuid"
}
```

Rules:
- Required: `title`, `description`.
- `description` must be non-empty.
- `estimated_hours` must be `>= 0`.
- `visibility` supports only `public` or `private`.
- Exactly one owner is allowed for a project.
- Owner is set via `owner_user_id` (or defaults to authenticated user if omitted).

Success response: `201 Created`
Returns base project/task fields plus:
- `visibility`
- `owner_user_id`

### GET `/projects`
Lists projects visible to the authenticated user.

Visibility behavior:
- `public` projects are visible to all authenticated users.
- `private` projects are visible only to project participants (or creator).

Query parameters:
- `visibility`: `public|private` (optional)
- `limit`
- `offset`

### GET `/projects/{project_id}/progress`
Returns project progress as percentage of completed tasks.

Response:
```json
{
  "project_id": "uuid",
  "total_tasks": 10,
  "completed_tasks": 4,
  "progress_percent": 40
}
```

### GET `/projects/{project_id}/participants`
Returns project participants with their roles.

Query parameters:
- `role` (optional role filter)
- `sort_by`: `role|display_name|assigned_at`
- `order`: `asc|desc`

Response item fields:
- `user_id`
- `display_name`
- `email`
- `role`
- `assigned_hours`
- `assigned_at`
- `role_assigned_at`

## Tasks

### POST `/tasks`
Creates a task.

Request body:
```json
{
  "title": "Task title",
  "description": "Task description",
  "priority": "high",
  "status": "open",
  "estimated_hours": 8,
  "parent_task_id": "uuid",
  "project_root_id": "uuid",
  "start_date": "2026-03-14",
  "due_date": "2026-03-16",
  "assignee_user_id": "uuid"
}
```

Rules:
- Required: `title`, `description`.
- `description` must be non-empty.
- `estimated_hours` must be `>= 0`.
- If `start_date` or `due_date` is present, `assignee_user_id` is required.
- If dates are present and `status` is omitted, `null`, `open`, or `pending`, the status is auto-set to `in_progress`.
- If `project_root_id` is set, task dates must stay within the project date range.
- If `parent_task_id` is set, the parent task must exist.

Success response: `201 Created`
Returns task JSON with fields like:
`id`, `parent_task_id`, `title`, `description`, `priority`, `status`, `estimated_hours`, `start_date`, `due_date`, `project_root_id`, `created_by`, `created_at`, `updated_at`.

### GET `/tasks`
Lists tasks assigned to the authenticated user.

Query parameters:
- `parent_task_id`
- `status`
- `priority`
- `has_time`: `true|false`
- `limit`
- `offset`

Success response: `200 OK`
Returns an array of task objects. Each item includes:
- base task fields
- `has_time`
- `assigned_hours`
- `role`
- `schedule`: array of task schedule items

### PUT `/tasks/{task_id}`
Updates a task. Owner permission required.

Supported fields:
- `title`
- `description`
- `priority`
- `status`
- `estimated_hours`
- `parent_task_id`
- `project_root_id`
- `start_date`
- `due_date`
- `assignee_user_id`

Rules:
- `description`, if provided, must be non-empty.
- `estimated_hours`, if provided, must be `>= 0`.
- Task cannot reference itself as `parent_task_id`.
- If effective dates exist, task must have an assignee either already assigned or provided in payload.
- Project date bounds are revalidated on update.
- Date-based auto-promotion to `in_progress` also applies on update.

### DELETE `/tasks/{task_id}`
Deletes a task. Owner permission required.

Success response: `200 OK`
```json
"Deleted"
```

### GET `/tasks/{task_id}/subtasks`
Lists subtasks for a parent task visible to the authenticated user.

Success response: `200 OK`
Returns an array of task objects with:
- `id`, `parent_task_id`, `title`, `description`, `priority`, `status`
- `estimated_hours`, `start_date`, `due_date`
- `project_root_id`, `created_by`, `created_at`, `updated_at`
- `assigned_hours`, `role`

## Task Assignments

### POST `/tasks/{task_id}/assignments`
Creates a task assignment.

Request body:
```json
{
  "user_id": "uuid",
  "role": "contributor",
  "assigned_hours": 6
}
```

Notes:
- If `user_id` is omitted, the authenticated user is used.
- The implementation also creates/uses role assignments.
- For projects (root tasks), role `owner` cannot be assigned if an owner already exists.

### GET `/tasks/{task_id}/assignments`
Lists assignments for a task.

### DELETE `/assignments/{assignment_id}`
Deletes an assignment.

## Task Notes

### POST `/tasks/{task_id}/notes`
Creates a note for a task.

Request body:
```json
{
  "content": "Some note"
}
```

Rules:
- `content` is required and must be non-empty.
- Caller must be the task creator or assigned to the task.

### GET `/tasks/{task_id}/notes`
Lists task notes.

Response item:
- `id`, `task_id`, `user_id`, `content`, `created_at`, `updated_at`, `author_name`

### PUT `/notes/{note_id}`
Updates a note. Only the note author may update it.

### DELETE `/notes/{note_id}`
Deletes a note. Allowed for the note author or task owner.

## Availability and Scheduling

### POST `/tasks/availability/calculate`
Calculates user availability for a time range.

Request body:
```json
{
  "user_id": "uuid",
  "start_ts": "2026-03-14T09:00:00Z",
  "end_ts": "2026-03-14T18:00:00Z"
}
```

Response:
```json
{
  "user_id": "uuid",
  "start_ts": "...",
  "end_ts": "...",
  "capacity_hours": 8,
  "busy_hours": 2,
  "available_hours": 6,
  "load_percent": 25
}
```

### POST `/tasks/{task_id}/candidate-assignees`
Returns ranked assignee candidates for a task. Owner permission required.

Request body:
```json
{
  "required_skills": ["backend", "sql"],
  "start_ts": "2026-03-18T09:00:00Z",
  "end_ts": "2026-03-19T18:00:00Z"
}
```

Notes:
- `required_skills` is optional.
- If `start_ts` and `end_ts` are omitted, the endpoint derives them from task dates.
- If the task itself has no dates, the request must provide `start_ts` and `end_ts`.
- Ranking is based on skill match plus available hours.

Response:
- `task_id`, `start_ts`, `end_ts`
- `candidates`: array with `user_id`, `display_name`, `email`, `skill_score`, `matched_skills`, `capacity_hours`, `busy_hours`, `available_hours`, `load_percent`, `relevance_score`

## Users

### POST `/users/{id}/work-schedule`
Replaces the authenticated user's work schedule.

Request body:
```json
[
  {
    "day_of_week": 1,
    "is_working_day": true,
    "start_time": "09:00",
    "end_time": "18:00"
  }
]
```

Rules:
- Authenticated user may only update their own schedule.
- Body must contain 7 items.
- `day_of_week` must be unique and in range `1..7`.
- For working days, `start_time` and `end_time` are required in `HH:MM`.

### GET `/users/{id}/work-schedule`
Returns stored work schedule for a user.

Response item:
- `id`, `user_id`, `day_of_week`, `is_working_day`, `start_time`, `end_time`

### GET `/users/{id}/workload`
Returns workload summary for the authenticated user. Self-only access.

Query parameters:
- `start_ts` optional
- `end_ts` optional

If omitted, the endpoint uses `now()` to `now() + 7 days`.

Response:
```json
{
  "user_id": "uuid",
  "start_ts": "...",
  "end_ts": "...",
  "capacity_hours": 40,
  "busy_hours": 12,
  "available_hours": 28,
  "load_percent": 30,
  "scheduled_tasks": 3
}
```

### GET `/users`
Searches users.

Query parameters:
- `search`

Response fields:
- `id`, `email`, `display_name`, `name`, `surname`, `locale`, `created_at`

### GET `/users/{id}`
Returns user profile.

Response fields:
- `id`, `email`, `display_name`, `name`, `surname`, `phone`, `telegram`, `locale`, `created_at`, `updated_at`

## Calendar

### GET `/calendar/tasks`
Returns calendar-visible tasks for the authenticated user.

Query parameters:
- `start_date` in `YYYY-MM-DD`
- `end_date` in `YYYY-MM-DD`

Response item:
- `task_id`, `title`, `start_date`, `end_date`, `allocated_hours`, `role`
- `schedule`: array with `date`, `start_time`, `end_time`, `hours`

## Current Domain Rules

- Date-based tasks require an assignee.
- Project-bound tasks cannot start before the project start date or end after the project due date.
- Timed and untimed tasks can be separated with `GET /tasks?has_time=true|false`.
- Candidate ranking uses both schedule availability and skill relevance from `user_skill`.
- Notes are access-controlled separately from task updates.