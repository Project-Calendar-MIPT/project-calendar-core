ALTER TABLE app_user
  ADD COLUMN IF NOT EXISTS email_verified BOOLEAN NOT NULL DEFAULT FALSE,
  ADD COLUMN IF NOT EXISTS email_verification_token TEXT;

CREATE UNIQUE INDEX IF NOT EXISTS idx_app_user_verification_token
  ON app_user(email_verification_token)
  WHERE email_verification_token IS NOT NULL;
