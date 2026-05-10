"""
Deadline Reminder Service
Runs every hour, sends email reminders based on user notification settings.
Deduplicates via deadline_reminder_sent table.
"""

import logging
import os
import smtplib
import time
from email.mime.multipart import MIMEMultipart
from email.mime.text import MIMEText

import psycopg2
import psycopg2.extras
import schedule

logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
log = logging.getLogger(__name__)

DB_HOST     = os.environ.get("DB_HOST", "db")
DB_PORT     = int(os.environ.get("DB_PORT", "5432"))
DB_NAME     = os.environ.get("DB_NAME", "project_calendar")
DB_USER     = os.environ.get("DB_USER", "pc_admin")
DB_PASSWORD = os.environ.get("DB_PASSWORD", "pc_password")

SMTP_HOST     = os.environ.get("SMTP_HOST", "smtp.gmail.com")
SMTP_PORT     = int(os.environ.get("SMTP_PORT", "587"))
SMTP_TLS      = os.environ.get("SMTP_TLS", "true").lower() == "true"
SMTP_USER     = os.environ.get("SMTP_USER", "")
SMTP_PASSWORD = os.environ.get("SMTP_PASSWORD", "")
EMAIL_FROM    = os.environ.get("EMAIL_FROM", "noreply@mipt.impelix.dev")
FRONTEND_URL  = os.environ.get("FRONTEND_URL", "https://project-calendar-mipt.github.io/project-calendar-web")


def get_db():
    return psycopg2.connect(
        host=DB_HOST, port=DB_PORT, dbname=DB_NAME,
        user=DB_USER, password=DB_PASSWORD,
        cursor_factory=psycopg2.extras.RealDictCursor,
    )


def send_email(to: str, subject: str, html_body: str) -> None:
    msg = MIMEMultipart("alternative")
    msg["Subject"] = subject
    msg["From"]    = EMAIL_FROM
    msg["To"]      = to
    msg.attach(MIMEText(html_body, "html", "utf-8"))

    with smtplib.SMTP(SMTP_HOST, SMTP_PORT, timeout=15) as server:
        if SMTP_TLS:
            server.ehlo()
            server.starttls()
            server.ehlo()
        if SMTP_USER and SMTP_PASSWORD:
            server.login(SMTP_USER, SMTP_PASSWORD)
        server.sendmail(EMAIL_FROM, [to], msg.as_string())


def label_for_days(days: int) -> str:
    if days == 1:
        return "завтра"
    if 2 <= days <= 4:
        return f"через {days} дня"
    return f"через {days} дней"


def label_for_hours(hours: int) -> str:
    if hours == 1:
        return "через 1 час"
    if 2 <= hours <= 4:
        return f"через {hours} часа"
    return f"через {hours} часов"


def make_html(display_name: str, task_title: str, due_str: str, time_label: str, task_url: str) -> str:
    return f"""
<html><body style="font-family:Arial,sans-serif;background:#f9fafb;padding:24px">
<div style="max-width:520px;margin:0 auto;background:#fff;border-radius:8px;
            border:1px solid #e5e7eb;padding:32px">
  <h2 style="color:#1f2937;margin-top:0">⏰ Напоминание о дедлайне</h2>
  <p style="color:#374151">Привет, <strong>{display_name}</strong>!</p>
  <p style="color:#374151">Срок выполнения задачи истекает <strong>{time_label}</strong>:</p>
  <div style="background:#f3f4f6;border-left:4px solid #4f46e5;
              padding:12px 16px;border-radius:4px;margin:16px 0">
    <strong style="color:#1f2937">{task_title}</strong>
    <div style="color:#6b7280;font-size:13px;margin-top:4px">Дедлайн: {due_str}</div>
  </div>
  <a href="{task_url}"
     style="display:inline-block;background:#4f46e5;color:#fff;
            padding:10px 20px;border-radius:6px;text-decoration:none;
            font-weight:bold;margin-top:8px">
    Открыть задачу
  </a>
  <p style="color:#9ca3af;font-size:12px;margin-top:24px">
    Вы получили это письмо, так как включили уведомления о дедлайнах.<br>
    Управление уведомлениями — в настройках профиля.
  </p>
</div>
</body></html>
"""


def check_and_send():
    log.info("Running deadline check…")
    try:
        conn = get_db()
        cur  = conn.cursor()

        # --- Day-level reminders ---
        cur.execute("""
            WITH candidates AS (
                SELECT
                    u.id           AS user_id,
                    u.email,
                    u.display_name,
                    t.id           AS task_id,
                    t.title        AS task_title,
                    t.due_date::text AS due_str,
                    d.days_before
                FROM task t
                JOIN task_assignment ta ON ta.task_id = t.id
                JOIN app_user u ON u.id = ta.user_id
                JOIN user_notification_settings ns ON ns.user_id = u.id,
                LATERAL unnest(ns.reminder_days_before) AS d(days_before)
                WHERE ns.deadline_reminders_enabled = TRUE
                  AND t.due_date IS NOT NULL
                  AND t.status NOT IN ('completed', 'cancelled')
                  AND t.due_date = CURRENT_DATE + (d.days_before || ' days')::interval
            )
            SELECT c.*
            FROM candidates c
            WHERE NOT EXISTS (
                SELECT 1 FROM deadline_reminder_sent drs
                WHERE drs.user_id = c.user_id
                  AND drs.task_id = c.task_id
                  AND drs.days_before = c.days_before
                  AND drs.sent_at::date = CURRENT_DATE
            )
        """)
        day_rows = cur.fetchall()

        # --- Hour-level reminders ---
        cur.execute("""
            WITH candidates AS (
                SELECT
                    u.id           AS user_id,
                    u.email,
                    u.display_name,
                    t.id           AS task_id,
                    t.title        AS task_title,
                    t.due_date::text AS due_str,
                    h.hours_before
                FROM task t
                JOIN task_assignment ta ON ta.task_id = t.id
                JOIN app_user u ON u.id = ta.user_id
                JOIN user_notification_settings ns ON ns.user_id = u.id,
                LATERAL unnest(ns.reminder_hours_before) AS h(hours_before)
                WHERE ns.deadline_reminders_enabled = TRUE
                  AND t.due_date IS NOT NULL
                  AND t.status NOT IN ('completed', 'cancelled')
                  AND (t.due_date::timestamptz - NOW()) BETWEEN
                      ((h.hours_before - 1) || ' hours')::interval AND
                      (h.hours_before || ' hours')::interval
            )
            SELECT c.*, -(1000 + c.hours_before) AS days_before
            FROM candidates c
            WHERE NOT EXISTS (
                SELECT 1 FROM deadline_reminder_sent drs
                WHERE drs.user_id = c.user_id
                  AND drs.task_id = c.task_id
                  AND drs.days_before = -(1000 + c.hours_before)
                  AND drs.sent_at > NOW() - interval '2 hours'
            )
        """)
        hour_rows = cur.fetchall()

        total = 0
        for row in list(day_rows) + list(hour_rows):
            email        = row["email"]
            display_name = row["display_name"] or email.split("@")[0]
            task_title   = row["task_title"]
            due_str      = row["due_str"]
            days_before  = row["days_before"]
            task_id      = row["task_id"]
            user_id      = row["user_id"]

            is_hours = days_before < 0
            if is_hours:
                hours_before = -(days_before + 1000)
                time_label = label_for_hours(hours_before)
            else:
                time_label = label_for_days(days_before)

            task_url = f"{FRONTEND_URL}/tasks/{task_id}"
            subject  = f"⏰ Дедлайн {time_label}: {task_title}"
            html     = make_html(display_name, task_title, due_str, time_label, task_url)

            try:
                send_email(email, subject, html)
                cur.execute(
                    "INSERT INTO deadline_reminder_sent (user_id, task_id, days_before) "
                    "VALUES (%s, %s, %s) ON CONFLICT DO NOTHING",
                    (str(user_id), str(task_id), days_before),
                )
                conn.commit()
                log.info("Sent reminder to %s: '%s' (%s)", email, task_title, time_label)
                total += 1
            except Exception as e:
                log.error("Failed to send reminder to %s: %s", email, e)
                conn.rollback()

        log.info("Deadline check done. Sent %d reminder(s).", total)
        cur.close()
        conn.close()

    except Exception as e:
        log.error("Deadline check error: %s", e)


if __name__ == "__main__":
    log.info("Deadline Reminder Service started")
    log.info("DB: %s@%s:%s/%s", DB_USER, DB_HOST, DB_PORT, DB_NAME)

    # Wait for DB
    for _ in range(20):
        try:
            conn = get_db()
            conn.close()
            log.info("DB connection OK")
            break
        except Exception as e:
            log.warning("Waiting for DB… %s", e)
            time.sleep(5)

    # Run once on startup, then every hour
    check_and_send()
    schedule.every(1).hours.do(check_and_send)

    while True:
        schedule.run_pending()
        time.sleep(60)
