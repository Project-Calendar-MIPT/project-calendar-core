-- Mark all users created before email verification was introduced as verified
UPDATE app_user SET email_verified = TRUE WHERE email_verification_token IS NULL;
