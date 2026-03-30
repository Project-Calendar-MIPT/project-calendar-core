#pragma once
#include <drogon/drogon.h>
#include <initializer_list>
#include <string>

// Returns the caller's highest role on a project (by task_id = project root id).
// Returns empty string if no role found.
inline std::string getCallerProjectRole(
    const std::shared_ptr<drogon::orm::DbClient>& dbClient,
    const std::string& projectId, const std::string& userId) {
  try {
    auto res = dbClient->execSqlSync(
        "SELECT role::text AS role FROM task_role_assignment "
        "WHERE task_id = $1::uuid AND user_id = $2::uuid "
        "ORDER BY assigned_at DESC LIMIT 1",
        projectId, userId);
    if (res.empty() || res[0]["role"].isNull()) return "";
    return res[0]["role"].as<std::string>();
  } catch (...) {
    return "";
  }
}

inline bool isOwnerOrAdmin(const std::string& role) {
  return role == "owner" || role == "admin";
}
