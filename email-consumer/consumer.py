"""
Email consumer: reads user-registration events from Kafka
and sends verification emails via SMTP (MailHog in dev).
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
EMAIL_FROM = os.environ.get("EMAIL_FROM", "noreply@project-calendar.dev")
TOPIC = "user-registration"
GROUP_ID = "email-consumer-group"


def send_email(to: str, display_name: str, verification_url: str) -> None:
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

    msg = MIMEMultipart("alternative")
    msg["Subject"] = subject
    msg["From"] = EMAIL_FROM
    msg["To"] = to
    msg.attach(MIMEText(html_body, "html", "utf-8"))

    with smtplib.SMTP(SMTP_HOST, SMTP_PORT, timeout=10) as server:
        server.sendmail(EMAIL_FROM, [to], msg.as_string())

    log.info("Sent verification email to %s", to)


def run() -> None:
    conf = {
        "bootstrap.servers": KAFKA_SERVERS,
        "group.id": GROUP_ID,
        "auto.offset.reset": "earliest",
        "enable.auto.commit": True,
    }

    consumer = Consumer(conf)
    consumer.subscribe([TOPIC])
    log.info("Subscribed to topic '%s' on %s", TOPIC, KAFKA_SERVERS)

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
                payload = json.loads(msg.value().decode("utf-8"))
                email = payload.get("email", "")
                display_name = payload.get("display_name", email)
                verification_url = payload.get("verification_url", "")

                if not email or not verification_url:
                    log.warning("Skipping malformed event: %s", payload)
                    continue

                send_email(email, display_name, verification_url)

            except Exception as exc:  # noqa: BLE001
                log.error("Failed to process message: %s", exc)

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
