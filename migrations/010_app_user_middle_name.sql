-- Add missing middle_name column to app_user
ALTER TABLE app_user ADD COLUMN IF NOT EXISTS middle_name TEXT;
