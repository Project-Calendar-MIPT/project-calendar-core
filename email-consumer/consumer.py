"""
Email consumer: reads events from Kafka and sends emails via SMTP.
Topics handled:
  - user-registration:   sends email verification link
  - project-invitation:  sends project join invitation link
"""

import json
import logging
import os
import smtplib
import time
from email.mime.multipart import MIMEMultipart
from email.mime.text import MIMEText

from confluent_kafka import Consumer, KafkaError, KafkaException

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s %(levelname)s %(message)s",
)
log = logging.getLogger(__name__)

KAFKA_SERVERS = os.environ.get("KAFKA_BOOTSTRAP_SERVERS", "kafka:9092")
SMTP_HOST = os.environ.get("SMTP_HOST", "mailhog")
SMTP_PORT = int(os.environ.get("SMTP_PORT", "1025"))
SMTP_USER = os.environ.get("SMTP_USER", "")
SMTP_PASSWORD = os.environ.get("SMTP_PASSWORD", "")
SMTP_TLS = os.environ.get("SMTP_TLS", "false").lower() == "true"
EMAIL_FROM = os.environ.get("EMAIL_FROM", "noreply@mipt.impelix.dev")
TOPICS = ["user-registration", "project-invitation"]
GROUP_ID = "email-consumer-group"


def _send(to: str, subject: str, html_body: str) -> None:
    msg = MIMEMultipart("alternative")
    msg["Subject"] = subject
    msg["From"] = EMAIL_FROM
    msg["To"] = to
    msg.attach(MIMEText(html_body, "html", "utf-8"))

    with smtplib.SMTP(SMTP_HOST, SMTP_PORT, timeout=10) as server:
        if SMTP_TLS:
            server.ehlo()
            server.starttls()
            server.ehlo()
        if SMTP_USER and SMTP_PASSWORD:
            server.login(SMTP_USER, SMTP_PASSWORD)
        server.sendmail(EMAIL_FROM, [to], msg.as_string())


def send_verification_email(to: str, display_name: str, verification_url: str) -> None:
    subject = "Подтвердите вашу почту — Project Calendar"
    html_body = f"""
<html><body>
<h2>Привет, {display_name}!</h2>
<p>Для завершения регистрации нажмите кнопку ниже:</p>
<p>
  <a href="{verification_url}"
     style="background:#4f46e5;color:#fff;padding:12px 24px;
            border-radius:6px;text-decoration:none;font-weight:bold;">
    Подтвердить email
  </a>
</p>
<p style="color:#6b7280;font-size:13px;">
  Или скопируйте ссылку: {verification_url}
</p>
<p style="color:#6b7280;font-size:13px;">
  Ссылка действительна 24 часа. Если вы не регистрировались — просто
  проигнорируйте это письмо.
</p>
</body></html>
"""
    _send(to, subject, html_body)
    log.info("Sent verification email to %s", to)


def send_invitation_email(to: str, display_name: str, project_title: str, join_url: str) -> None:
    subject = f"Вас пригласили в проект «{project_title}» — Project Calendar"
    html_body = f"""
<html><body>
<h2>Привет, {display_name}!</h2>
<p>Вас пригласили присоединиться к проекту <strong>«{project_title}»</strong>.</p>
<p>
  <a href="{join_url}"
     style="background:#4f46e5;color:#fff;padding:12px 24px;
            border-radius:6px;text-decoration:none;font-weight:bold;">
    Перейти к проекту
  </a>
</p>
<p style="color:#6b7280;font-size:13px;">
  Или скопируйте ссылку: {join_url}
</p>
<p style="color:#6b7280;font-size:13px;">
  Если вы не ожидали этого письма — просто проигнорируйте его.
</p>
</body></html>
"""
    _send(to, subject, html_body)
    log.info("Sent invitation email to %s for project %s", to, project_title)


def handle_message(topic: str, payload: dict) -> None:
    if topic == "user-registration":
        email = payload.get("email", "")
        display_name = payload.get("display_name", email)
        verification_url = payload.get("verification_url", "")
        if not email or not verification_url:
            log.warning("Skipping malformed user-registration event: %s", payload)
            return
        send_verification_email(email, display_name, verification_url)

    elif topic == "project-invitation":
        email = payload.get("email", "")
        display_name = payload.get("display_name", email)
        project_title = payload.get("project_title", "проект")
        join_url = payload.get("join_url", "")
        if not email or not join_url:
            log.warning("Skipping malformed project-invitation event: %s", payload)
            return
        send_invitation_email(email, display_name, project_title, join_url)

    else:
        log.warning("Unknown topic '%s', skipping", topic)


def run() -> None:
    conf = {
        "bootstrap.servers": KAFKA_SERVERS,
        "group.id": GROUP_ID,
        "auto.offset.reset": "earliest",
        "enable.auto.commit": True,
        "allow.auto.create.topics": True,
    }

    consumer = Consumer(conf)
    consumer.subscribe(TOPICS)
    log.info("Subscribed to topics %s on %s", TOPICS, KAFKA_SERVERS)

    try:
        while True:
            msg = consumer.poll(timeout=1.0)
            if msg is None:
                continue
            if msg.error():
                if msg.error().code() == KafkaError._PARTITION_EOF:
                    continue
                raise KafkaException(msg.error())

            try:
                topic = msg.topic()
                payload = json.loads(msg.value().decode("utf-8"))
                handle_message(topic, payload)
            except Exception as exc:  # noqa: BLE001
                log.error("Failed to process message from %s: %s", msg.topic(), exc)

    finally:
        consumer.close()


if __name__ == "__main__":
    # Wait a bit for Kafka to be fully ready
    time.sleep(5)
    while True:
        try:
            run()
        except Exception as exc:  # noqa: BLE001
            log.error("Consumer crashed, restarting in 5s: %s", exc)
            time.sleep(5)
