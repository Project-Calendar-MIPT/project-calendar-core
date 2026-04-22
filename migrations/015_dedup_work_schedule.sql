-- Remove duplicate work schedule entries, keeping only the latest per (user_id, weekday).
-- Duplicates accumulated from multiple failed registration attempts and test runs.
DELETE FROM user_work_schedule
WHERE id NOT IN (
    SELECT DISTINCT ON (user_id, weekday) id
    FROM user_work_schedule
    ORDER BY user_id, weekday, id
);
