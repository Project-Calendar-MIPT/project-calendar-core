-- Настройки уведомлений пользователя о дедлайнах
CREATE TABLE IF NOT EXISTS user_notification_settings (
    user_id                    UUID PRIMARY KEY REFERENCES app_user(id) ON DELETE CASCADE,
    deadline_reminders_enabled BOOLEAN      NOT NULL DEFAULT TRUE,
    reminder_days_before       INTEGER[]    NOT NULL DEFAULT '{1,3,7}',
    reminder_hours_before      INTEGER[]    NOT NULL DEFAULT '{}',
    updated_at                 TIMESTAMPTZ  NOT NULL DEFAULT NOW()
);

-- Журнал отправленных напоминаний (дедупликация)
CREATE TABLE IF NOT EXISTS deadline_reminder_sent (
    id         UUID         PRIMARY KEY DEFAULT gen_random_uuid(),
    user_id    UUID         NOT NULL REFERENCES app_user(id) ON DELETE CASCADE,
    task_id    UUID         NOT NULL REFERENCES task(id) ON DELETE CASCADE,
    days_before INTEGER     NOT NULL,
    sent_at    TIMESTAMPTZ  NOT NULL DEFAULT NOW(),
    UNIQUE (user_id, task_id, days_before)
);

CREATE INDEX IF NOT EXISTS idx_deadline_reminder_sent_task
    ON deadline_reminder_sent (task_id, user_id);
