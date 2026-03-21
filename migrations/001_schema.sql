-- ============================================================================
-- Project Calendar - Database Schema
-- ============================================================================

-- ============================================================================
-- ENUM TYPES
-- ============================================================================

CREATE TYPE task_priority_enum AS ENUM ('low', 'normal', 'high', 'urgent');
CREATE TYPE task_status_enum AS ENUM ('open', 'in_progress', 'completed', 'cancelled', 'pending');
CREATE TYPE role_enum AS ENUM ('admin', 'owner', 'supervisor', 'hybrid', 'executor', 'spectator', 'audit_role');
CREATE TYPE delegation_status_enum AS ENUM ('pending', 'active', 'declined', 'expired', 'revoked');
CREATE TYPE scope_type_enum AS ENUM ('global', 'project', 'task');
CREATE TYPE resolution_kind_enum AS ENUM ('schedule_conflict', 'overallocation', 'missing_assignment');
CREATE TYPE resolution_status_enum AS ENUM ('suggested', 'accepted', 'declined', 'resolved');

-- ============================================================================
-- TABLE: app_user
-- Основная таблица пользователей
-- ============================================================================

CREATE TABLE app_user (
    id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    email TEXT UNIQUE NOT NULL,
    display_name TEXT NOT NULL,
    name TEXT,
    surname TEXT,
    patronymic TEXT,
    telegram TEXT,
    phone TEXT,
    locale TEXT DEFAULT 'ru-RU',
    password_hash TEXT,
    avatar_url TEXT,
    created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    updated_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    visibility BOOLEAN NOT NULL DEFAULT false,
    is_verified BOOLEAN NOT NULL DEFAULT false,
    confirmation_token TEXT,
    skills JSONB DEFAULT '[]'::jsonb,
    experience_level TEXT
);

CREATE INDEX idx_app_user_email ON app_user(email);
CREATE INDEX idx_app_user_display_name ON app_user(display_name);

-- ============================================================================
-- TABLE: task
-- Основная таблица задач с поддержкой иерархии
-- ============================================================================

CREATE TABLE calendar_task (
    id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    parent_task_id UUID REFERENCES task(id) ON DELETE CASCADE,
    title TEXT NOT NULL,
    description TEXT,
    priority task_priority_enum DEFAULT 'normal',
    status task_status_enum DEFAULT 'open',
    estimated_hours NUMERIC(10,2) DEFAULT 0,
    start_date DATE,
    due_date DATE,
    project_root_id UUID,
    created_by UUID NOT NULL REFERENCES app_user(id),
    created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    updated_at TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

CREATE INDEX idx_task_parent_task_id ON task(parent_task_id);
CREATE INDEX idx_task_project_root_id ON task(project_root_id);
CREATE INDEX idx_task_created_by ON task(created_by);
CREATE INDEX idx_task_status ON task(status);
CREATE INDEX idx_task_priority ON task(priority);

-- ============================================================================
-- TABLE: task_dependency
-- Зависимости между задачами
-- ============================================================================

CREATE TABLE task_dependency (
    id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    task_id UUID NOT NULL REFERENCES task(id) ON DELETE CASCADE,
    depends_on_id UUID NOT NULL REFERENCES task(id) ON DELETE CASCADE,
    kind TEXT DEFAULT 'finish_start'
);

CREATE INDEX idx_task_dependency_task_id ON task_dependency(task_id);
CREATE INDEX idx_task_dependency_depends_on_id ON task_dependency(depends_on_id);

-- ============================================================================
-- TABLE: task_role_assignment
-- Назначение ролей пользователей на задачи
-- ============================================================================

CREATE TABLE task_role_assignment (
    id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    task_id UUID NOT NULL REFERENCES task(id) ON DELETE CASCADE,
    user_id UUID NOT NULL REFERENCES app_user(id) ON DELETE CASCADE,
    role role_enum NOT NULL,
    assigned_at TIMESTAMPTZ DEFAULT NOW()
);

CREATE INDEX idx_task_role_assignment_task_id ON task_role_assignment(task_id);
CREATE INDEX idx_task_role_assignment_user_id ON task_role_assignment(user_id);
CREATE INDEX idx_task_role_assignment_role ON task_role_assignment(role);

-- ============================================================================
-- TABLE: task_assignment
-- Назначение часов работы пользователю на задачу
-- ============================================================================

CREATE TABLE task_assignment (
    id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    task_id UUID NOT NULL REFERENCES task(id) ON DELETE CASCADE,
    user_id UUID NOT NULL REFERENCES app_user(id) ON DELETE CASCADE,
    assigned_hours NUMERIC(10,2) DEFAULT 0,
    assigned_at TIMESTAMPTZ DEFAULT NOW()
);

CREATE INDEX idx_task_assignment_task_id ON task_assignment(task_id);
CREATE INDEX idx_task_assignment_user_id ON task_assignment(user_id);

-- ============================================================================
-- TABLE: permission
-- Определение разрешений в системе (согласно permissions.html спеке)
-- 28 разрешений с поддержкой global/local флага
-- ============================================================================

CREATE TABLE permission (
    key TEXT PRIMARY KEY,
    description TEXT NOT NULL,
    is_global BOOLEAN DEFAULT FALSE
);

-- ============================================================================
-- TABLE: role_permission
-- Связь ролей и разрешений
-- ============================================================================

CREATE TABLE role_permission (
    id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    role role_enum NOT NULL,
    permission_key TEXT NOT NULL REFERENCES permission(key),
    is_global BOOLEAN DEFAULT FALSE
);

CREATE INDEX idx_role_permission_role ON role_permission(role);
CREATE INDEX idx_role_permission_permission_key ON role_permission(permission_key);

-- ============================================================================
-- TABLE: global_role_grant
-- Глобальное назначение ролей пользователям
-- ============================================================================

CREATE TABLE global_role_grant (
    id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    user_id UUID NOT NULL REFERENCES app_user(id) ON DELETE CASCADE,
    scope_type scope_type_enum DEFAULT 'global',
    scope_id UUID REFERENCES task(id),
    role role_enum NOT NULL,
    granted_at TIMESTAMPTZ DEFAULT NOW()
);

CREATE INDEX idx_global_role_grant_user_id ON global_role_grant(user_id);
CREATE INDEX idx_global_role_grant_role ON global_role_grant(role);
CREATE INDEX idx_global_role_grant_scope ON global_role_grant(scope_type, scope_id);

-- ============================================================================
-- TABLE: delegation
-- Делегирование прав между пользователями
-- ============================================================================

CREATE TABLE delegation (
    id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    task_id UUID NOT NULL REFERENCES task(id) ON DELETE CASCADE,
    grantor_user_id UUID NOT NULL REFERENCES app_user(id),
    grantee_user_id UUID NOT NULL REFERENCES app_user(id),
    status delegation_status_enum DEFAULT 'pending',
    expires_at TIMESTAMPTZ,
    created_at TIMESTAMPTZ DEFAULT NOW(),
    activated_at TIMESTAMPTZ,
    revoked_at TIMESTAMPTZ,
    reason TEXT
);

CREATE INDEX idx_delegation_task_id ON delegation(task_id);
CREATE INDEX idx_delegation_grantor_user_id ON delegation(grantor_user_id);
CREATE INDEX idx_delegation_grantee_user_id ON delegation(grantee_user_id);
CREATE INDEX idx_delegation_status ON delegation(status);

-- ============================================================================
-- TABLE: delegation_permission
-- Разрешения в контексте делегирования
-- ============================================================================

CREATE TABLE delegation_permission (
    id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    delegation_id UUID NOT NULL REFERENCES delegation(id) ON DELETE CASCADE,
    permission_key TEXT NOT NULL REFERENCES permission(key),
    allow BOOLEAN DEFAULT TRUE
);

CREATE INDEX idx_delegation_permission_delegation_id ON delegation_permission(delegation_id);
CREATE INDEX idx_delegation_permission_permission_key ON delegation_permission(permission_key);

-- ============================================================================
-- TABLE: user_work_schedule
-- Расписание работы пользователя по дням недели
-- ============================================================================

CREATE TABLE user_work_schedule (
    id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    user_id UUID NOT NULL REFERENCES app_user(id) ON DELETE CASCADE,
    weekday INT CHECK (weekday >= 0 AND weekday <= 6),
    time_slots JSONB DEFAULT '[]'::jsonb,
    UNIQUE(user_id, day_of_week)
);

CREATE INDEX idx_user_work_schedule_user_id ON user_work_schedule(user_id);

-- ============================================================================
-- TABLE: task_schedule
-- Расписание выполнения конкретной задачи пользователем
-- ============================================================================

CREATE TABLE task_schedule (
    id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    task_id UUID NOT NULL REFERENCES task(id) ON DELETE CASCADE,
    user_id UUID NOT NULL REFERENCES app_user(id) ON DELETE CASCADE,
    start_ts TIMESTAMPTZ NOT NULL,
    end_ts TIMESTAMPTZ NOT NULL,
    hours NUMERIC(10,2) NOT NULL,
    auto_placed BOOLEAN DEFAULT TRUE
);

CREATE INDEX idx_task_schedule_task_id ON task_schedule(task_id);
CREATE INDEX idx_task_schedule_user_id ON task_schedule(user_id);
CREATE INDEX idx_task_schedule_start_ts ON task_schedule(start_ts);

-- ============================================================================
-- TABLE: project_allocation
-- Выделение часов проекта для пользователя по дням
-- ============================================================================

CREATE TABLE project_allocation (
    id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    project_id UUID NOT NULL REFERENCES task(id) ON DELETE CASCADE,
    user_id UUID NOT NULL REFERENCES app_user(id) ON DELETE CASCADE,
    weekday INT CHECK (weekday >= 0 AND weekday <= 6),
    start_time TIME NOT NULL,
    end_time TIME NOT NULL,
    hours_per_day NUMERIC(10,2)
);

CREATE INDEX idx_project_allocation_project_id ON project_allocation(project_id);
CREATE INDEX idx_project_allocation_user_id ON project_allocation(user_id);

-- ============================================================================
-- TABLE: super_project
-- Супер-проекты
-- ============================================================================

CREATE TABLE super_project (
    id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    name TEXT NOT NULL,
    description TEXT,
    created_at TIMESTAMPTZ DEFAULT NOW()
);

CREATE INDEX idx_super_project_name ON super_project(name);

-- ============================================================================
-- TABLE: super_project_link
-- Связь супер-проектов
-- ============================================================================

CREATE TABLE super_project_link (
    id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    super_project_id UUID NOT NULL REFERENCES super_project(id) ON DELETE CASCADE,
    project_id UUID NOT NULL REFERENCES task(id) ON DELETE CASCADE
);

CREATE INDEX idx_super_project_link_super_project_id ON super_project_link(super_project_id);
CREATE INDEX idx_super_project_link_project_id ON super_project_link(project_id);

-- ============================================================================
-- TABLE: audit_log
-- Логирование всех действий в системе
-- ============================================================================

CREATE TABLE audit_log (
    id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    timestamp TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    actor_user_id UUID REFERENCES app_user(id),
    action_type TEXT NOT NULL,
    object_type TEXT NOT NULL,
    object_id UUID,
    project_id UUID,
    details JSONB,
    ip INET,
    user_agent TEXT
);

CREATE INDEX idx_audit_log_timestamp ON audit_log(timestamp DESC);
CREATE INDEX idx_audit_log_actor_user_id ON audit_log(actor_user_id);
CREATE INDEX idx_audit_log_object_type ON audit_log(object_type);

-- ============================================================================
-- TABLE: conflict_resolution
-- Разрешение конфликтов в расписании
-- ============================================================================

CREATE TABLE conflict_resolution (
    id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    project_id UUID NOT NULL REFERENCES task(id) ON DELETE CASCADE,
    task_id UUID NOT NULL REFERENCES task(id) ON DELETE CASCADE,
    target_user_id UUID REFERENCES app_user(id),
    kind resolution_kind_enum NOT NULL,
    payload JSONB NOT NULL,
    status resolution_status_enum DEFAULT 'suggested',
    suggested_by UUID,
    suggested_at TIMESTAMPTZ,
    decided_by UUID,
    decided_at TIMESTAMPTZ
);

CREATE INDEX idx_conflict_resolution_project_id ON conflict_resolution(project_id);
CREATE INDEX idx_conflict_resolution_task_id ON conflict_resolution(task_id);
CREATE INDEX idx_conflict_resolution_status ON conflict_resolution(status);

-- ============================================================================
-- END OF SCHEMA
-- ============================================================================