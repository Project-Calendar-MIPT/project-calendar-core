#include "API/ProjectController.h"

#include <drogon/HttpResponse.h>
#include <drogon/drogon.h>
#include <drogon/orm/Mapper.h>
#include <json/json.h>
#include <trantor/utils/Logger.h>

#include <algorithm>
#include <string>

#include "API/RbacHelpers.h"
#include "models/Task.h"
#include "models/TaskAssignment.h"
#include "models/TaskRoleAssignment.h"

using namespace drogon;

namespace {

std::string getPathVariableCompat(const HttpRequestPtr& req,
                                  const std::string& name = "id") {
  const std::string q = req->getParameter(name);
  if (!q.empty()) return q;

  const std::string p = req->path();
  if (p.empty()) return {};

  auto pos = p.find_last_not_of('/');
  if (pos == std::string::npos) return {};

  auto start = p.find_last_of('/', pos);
  if (start == std::string::npos) {
    start = 0;
  } else {
    ++start;
  }
  return p.substr(start, pos - start + 1);
}

bool isAllowedVisibility(const std::string& visibility) {
  return visibility == "public" || visibility == "private";
}

bool isAllowedRole(const std::string& role) {
  return role == "admin" || role == "owner" || role == "supervisor" ||
         role == "hybrid" || role == "executor" || role == "spectator" ||
         role == "audit_role";
}

struct ProjectAccess {
  bool exists{false};
  std::string visibility;
  bool canView{false};
};

ProjectAccess resolveProjectAccess(
    const std::shared_ptr<drogon::orm::DbClient>& dbClient,
    const std::string& projectId, const std::string& userId) {
  ProjectAccess access;

  auto res = dbClient->execSqlSync(
      R"sql(
      SELECT t.id,
             pv.visibility::text AS visibility,
             EXISTS(
               SELECT 1
               FROM task_assignment ta
               WHERE ta.task_id = t.id
                 AND ta.user_id = $2::uuid
             ) AS is_member,
             (t.created_by = $2::uuid) AS is_creator
      FROM task t
      JOIN project_visibility pv ON pv.project_id = t.id
      WHERE t.id = $1::uuid
        AND t.parent_task_id IS NULL
      LIMIT 1
    )sql",
      projectId, userId);

  if (res.empty()) {
    return access;
  }

  access.exists = true;
  access.visibility = res[0]["visibility"].as<std::string>();
  const bool isMember =
      !res[0]["is_member"].isNull() && res[0]["is_member"].as<bool>();
  const bool isCreator =
      !res[0]["is_creator"].isNull() && res[0]["is_creator"].as<bool>();
  access.canView = access.visibility == "public" || isMember || isCreator;

  return access;
}

}  // namespace

void ProjectController::createProject(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback) {
  auto attrsPtr = req->attributes();
  if (!attrsPtr || !attrsPtr->find("user_id")) {
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Unauthorized"));
    resp->setStatusCode(k401Unauthorized);
    return callback(resp);
  }

  const std::string requesterId = attrsPtr->get<std::string>("user_id");
  if (requesterId.empty()) {
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Unauthorized"));
    resp->setStatusCode(k401Unauthorized);
    return callback(resp);
  }

  auto jsonPtr = req->getJsonObject();
  if (!jsonPtr || !jsonPtr->isObject()) {
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Invalid JSON"));
    resp->setStatusCode(k400BadRequest);
    return callback(resp);
  }

  const Json::Value& j = *jsonPtr;

  if (!j.isMember("title") || !j["title"].isString() ||
      j["title"].asString().empty()) {
    auto resp = HttpResponse::newHttpJsonResponse(
        Json::Value("Missing or empty title"));
    resp->setStatusCode(k400BadRequest);
    return callback(resp);
  }

  if (!j.isMember("description") || !j["description"].isString() ||
      j["description"].asString().empty()) {
    auto resp = HttpResponse::newHttpJsonResponse(
        Json::Value("Missing or empty description"));
    resp->setStatusCode(k400BadRequest);
    return callback(resp);
  }

  if (j.isMember("owners") || j.isMember("owner_ids") ||
      j.isMember("owner_user_ids")) {
    auto resp = HttpResponse::newHttpJsonResponse(
        Json::Value("Exactly one owner is required; use owner_user_id"));
    resp->setStatusCode(k400BadRequest);
    return callback(resp);
  }

  std::string ownerUserId = requesterId;
  if (j.isMember("owner_user_id") && !j["owner_user_id"].isNull()) {
    if (!j["owner_user_id"].isString() || j["owner_user_id"].asString().empty()) {
      auto resp = HttpResponse::newHttpJsonResponse(
          Json::Value("owner_user_id must be a non-empty string"));
      resp->setStatusCode(k400BadRequest);
      return callback(resp);
    }
    ownerUserId = j["owner_user_id"].asString();
  }

  std::string visibility = "private";
  if (j.isMember("visibility") && !j["visibility"].isNull()) {
    if (!j["visibility"].isString()) {
      auto resp = HttpResponse::newHttpJsonResponse(
          Json::Value("visibility must be a string"));
      resp->setStatusCode(k400BadRequest);
      return callback(resp);
    }
    visibility = j["visibility"].asString();
  }

  if (!isAllowedVisibility(visibility)) {
    auto resp = HttpResponse::newHttpJsonResponse(
        Json::Value("visibility must be either public or private"));
    resp->setStatusCode(k400BadRequest);
    return callback(resp);
  }

  if (j.isMember("estimated_hours") && !j["estimated_hours"].isNull()) {
    double hours = 0.0;
    if (j["estimated_hours"].isNumeric()) {
      hours = j["estimated_hours"].asDouble();
    } else if (j["estimated_hours"].isString()) {
      try {
        hours = std::stod(j["estimated_hours"].asString());
      } catch (...) {
        auto resp = HttpResponse::newHttpJsonResponse(
            Json::Value("Invalid estimated_hours"));
        resp->setStatusCode(k400BadRequest);
        return callback(resp);
      }
    }
    if (hours < 0.0) {
      auto resp = HttpResponse::newHttpJsonResponse(
          Json::Value("estimated_hours must be >= 0"));
      resp->setStatusCode(k400BadRequest);
      return callback(resp);
    }
  }

  auto dbClient = app().getDbClient();

  try {
    auto ownerRes = dbClient->execSqlSync(
        "SELECT 1 FROM app_user WHERE id = $1::uuid LIMIT 1", ownerUserId);
    if (ownerRes.empty()) {
      auto resp =
          HttpResponse::newHttpJsonResponse(Json::Value("owner_user_id not found"));
      resp->setStatusCode(k400BadRequest);
      return callback(resp);
    }

    dbClient->execSqlSync("BEGIN");

    drogon_model::project_calendar::Task project;
    project.setTitle(j["title"].asString());
    project.setDescription(j["description"].asString());
    project.setParentTaskIdToNull();
    project.setProjectRootIdToNull();
    project.setCreatedBy(requesterId);
    project.setCreatedAt(::trantor::Date::now());
    project.setUpdatedAt(::trantor::Date::now());

    if (j.isMember("priority") && j["priority"].isString() &&
        !j["priority"].asString().empty()) {
      project.setPriority(j["priority"].asString());
    }
    if (j.isMember("status") && j["status"].isString() &&
        !j["status"].asString().empty()) {
      project.setStatus(j["status"].asString());
    }
    if (j.isMember("estimated_hours") && !j["estimated_hours"].isNull()) {
      if (j["estimated_hours"].isNumeric()) {
        project.setEstimatedHours(std::to_string(j["estimated_hours"].asDouble()));
      } else if (j["estimated_hours"].isString()) {
        project.setEstimatedHours(j["estimated_hours"].asString());
      }
    }
    drogon::orm::Mapper<drogon_model::project_calendar::Task> taskMapper(dbClient);
    taskMapper.insert(project);

    std::string projectId;
    try {
      projectId = project.getValueOfId();
    } catch (...) {
      projectId.clear();
    }

    if (projectId.empty()) {
      auto idRes = dbClient->execSqlSync(
          R"sql(
          SELECT id::text AS id
          FROM task
          WHERE created_by = $1::uuid
            AND title = $2
            AND parent_task_id IS NULL
          ORDER BY created_at DESC
          LIMIT 1
        )sql",
          requesterId, j["title"].asString());
      if (idRes.empty()) {
        dbClient->execSqlSync("ROLLBACK");
        auto resp = HttpResponse::newHttpJsonResponse(
            Json::Value("Failed to determine inserted project id"));
        resp->setStatusCode(k500InternalServerError);
        return callback(resp);
      }
      projectId = idRes[0]["id"].as<std::string>();
    }

    if (j.isMember("start_date") && j["start_date"].isString() &&
        !j["start_date"].asString().empty()) {
      dbClient->execSqlSync(
          "UPDATE task SET start_date = $2::date WHERE id = $1::uuid", projectId,
          j["start_date"].asString());
    }
    if (j.isMember("due_date") && j["due_date"].isString() &&
        !j["due_date"].asString().empty()) {
      dbClient->execSqlSync(
          "UPDATE task SET due_date = $2::date WHERE id = $1::uuid", projectId,
          j["due_date"].asString());
    }

    drogon_model::project_calendar::TaskAssignment ta;
    ta.setTaskId(projectId);
    ta.setUserId(ownerUserId);
    ta.setAssignedAt(::trantor::Date::now());
    drogon::orm::Mapper<drogon_model::project_calendar::TaskAssignment> taMapper(
        dbClient);
    taMapper.insert(ta);

    drogon_model::project_calendar::TaskRoleAssignment tra;
    tra.setTaskId(projectId);
    tra.setUserId(ownerUserId);
    tra.setRole(std::string("owner"));
    tra.setAssignedAt(::trantor::Date::now());
    drogon::orm::Mapper<drogon_model::project_calendar::TaskRoleAssignment>
        traMapper(dbClient);
    traMapper.insert(tra);

    dbClient->execSqlSync(
        "INSERT INTO project_visibility (project_id, visibility) VALUES "
        "($1::uuid, $2::project_visibility_enum)",
        projectId, visibility);

    dbClient->execSqlSync("COMMIT");

    auto finalRes = dbClient->execSqlSync(
        R"sql(
        SELECT t.id::text AS id,
               t.parent_task_id::text AS parent_task_id,
               t.title,
               t.description,
               t.priority::text AS priority,
               t.status::text AS status,
               t.estimated_hours,
               t.start_date::text AS start_date,
               t.due_date::text AS due_date,
               t.project_root_id::text AS project_root_id,
               t.created_by::text AS created_by,
               t.created_at::text AS created_at,
               t.updated_at::text AS updated_at,
               pv.visibility::text AS visibility,
               tra.user_id::text AS owner_user_id,
               au.display_name AS owner_display_name,
               au.email AS owner_email
        FROM task t
        JOIN project_visibility pv ON pv.project_id = t.id
        LEFT JOIN task_role_assignment tra
          ON tra.task_id = t.id
         AND tra.role = 'owner'
        LEFT JOIN app_user au ON au.id = tra.user_id
        WHERE t.id = $1::uuid
        LIMIT 1
      )sql",
        projectId);

    if (finalRes.empty()) {
      auto resp = HttpResponse::newHttpJsonResponse(
          Json::Value("Project created but cannot fetch it"));
      resp->setStatusCode(k500InternalServerError);
      return callback(resp);
    }

    Json::Value out(Json::objectValue);
    const auto& row = finalRes[0];
    out["id"] = row["id"].as<std::string>();
    out["parent_task_id"] = row["parent_task_id"].isNull()
                                ? Json::Value()
                                : Json::Value(row["parent_task_id"].as<std::string>());
    out["title"] = row["title"].isNull() ? Json::Value()
                                           : Json::Value(row["title"].as<std::string>());
    out["description"] = row["description"].isNull()
                             ? Json::Value()
                             : Json::Value(row["description"].as<std::string>());
    out["priority"] = row["priority"].isNull()
                          ? Json::Value()
                          : Json::Value(row["priority"].as<std::string>());
    out["status"] = row["status"].isNull()
                        ? Json::Value()
                        : Json::Value(row["status"].as<std::string>());
    out["estimated_hours"] = row["estimated_hours"].isNull()
                                 ? Json::Value()
                                 : Json::Value(row["estimated_hours"].as<std::string>());
    out["start_date"] = row["start_date"].isNull()
                            ? Json::Value()
                            : Json::Value(row["start_date"].as<std::string>());
    out["due_date"] = row["due_date"].isNull()
                          ? Json::Value()
                          : Json::Value(row["due_date"].as<std::string>());
    out["project_root_id"] = row["project_root_id"].isNull()
                                 ? Json::Value()
                                 : Json::Value(row["project_root_id"].as<std::string>());
    out["created_by"] = row["created_by"].isNull()
                            ? Json::Value()
                            : Json::Value(row["created_by"].as<std::string>());
    out["created_at"] = row["created_at"].isNull()
                            ? Json::Value()
                            : Json::Value(row["created_at"].as<std::string>());
    out["updated_at"] = row["updated_at"].isNull()
                            ? Json::Value()
                            : Json::Value(row["updated_at"].as<std::string>());
    out["visibility"] = row["visibility"].isNull()
                            ? Json::Value()
                            : Json::Value(row["visibility"].as<std::string>());
    out["owner_user_id"] = row["owner_user_id"].isNull()
                               ? Json::Value()
                               : Json::Value(row["owner_user_id"].as<std::string>());
    out["owner_display_name"] = row["owner_display_name"].isNull()
                                    ? Json::Value()
                                    : Json::Value(row["owner_display_name"].as<std::string>());
    out["owner_email"] = row["owner_email"].isNull()
                             ? Json::Value()
                             : Json::Value(row["owner_email"].as<std::string>());

    auto resp = HttpResponse::newHttpJsonResponse(out);
    resp->setStatusCode(k201Created);
    return callback(resp);
  } catch (const std::exception& e) {
    LOG_ERROR << "createProject failed: " << e.what();
    try {
      dbClient->execSqlSync("ROLLBACK");
    } catch (...) {
    }
    auto resp =
        HttpResponse::newHttpJsonResponse(Json::Value("Internal server error"));
    resp->setStatusCode(k500InternalServerError);
    return callback(resp);
  }
}

void ProjectController::getProjects(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback) {
  auto attrsPtr = req->attributes();
  if (!attrsPtr || !attrsPtr->find("user_id")) {
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Unauthorized"));
    resp->setStatusCode(k401Unauthorized);
    return callback(resp);
  }

  const std::string requesterId = attrsPtr->get<std::string>("user_id");
  if (requesterId.empty()) {
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Unauthorized"));
    resp->setStatusCode(k401Unauthorized);
    return callback(resp);
  }

  std::string visibilityFilter = req->getParameter("visibility");
  if (!visibilityFilter.empty() && !isAllowedVisibility(visibilityFilter)) {
    auto resp = HttpResponse::newHttpJsonResponse(
        Json::Value("visibility must be either public or private"));
    resp->setStatusCode(k400BadRequest);
    return callback(resp);
  }

  std::string scopeParam = req->getParameter("scope");
  if (!scopeParam.empty() && scopeParam != "all" && scopeParam != "mine" &&
      scopeParam != "mine_and_teams") {
    auto resp = HttpResponse::newHttpJsonResponse(
        Json::Value("scope must be all, mine, or mine_and_teams"));
    resp->setStatusCode(k400BadRequest);
    return callback(resp);
  }
  if (scopeParam.empty()) scopeParam = "all";

  int64_t limit = 100;
  int64_t offset = 0;

  const std::string limitParam = req->getParameter("limit");
  const std::string offsetParam = req->getParameter("offset");

  if (!limitParam.empty()) {
    try {
      limit = std::clamp(static_cast<int64_t>(std::stoll(limitParam)), int64_t(1),
                         int64_t(2000));
    } catch (...) {
      limit = 100;
    }
  }

  if (!offsetParam.empty()) {
    try {
      offset =
          std::max(int64_t(0), static_cast<int64_t>(std::stoll(offsetParam)));
    } catch (...) {
      offset = 0;
    }
  }

  auto dbClient = app().getDbClient();

  try {
    // Build scope-dependent WHERE fragment
    std::string scopeWhere;
    if (scopeParam == "mine") {
      scopeWhere = "AND ta_me.user_id IS NOT NULL";
    } else if (scopeParam == "mine_and_teams") {
      scopeWhere =
          "AND (ta_me.user_id IS NOT NULL OR EXISTS ("
          "  SELECT 1 FROM team_member tm2"
          "  JOIN team_project_assignment tpa ON tpa.team_id = tm2.team_id"
          "  WHERE tm2.user_id = $1::uuid AND tpa.project_id = t.id"
          "))";
    } else {
      // all: public projects + ones user has access to
      scopeWhere =
          "AND (pv.visibility = 'public' OR ta_me.user_id IS NOT NULL OR "
          "t.created_by = $1::uuid)";
    }

    const std::string sql =
        R"sql(
      SELECT t.id::text AS id,
             t.title,
             t.description,
             t.priority::text AS priority,
             t.status::text AS status,
             t.estimated_hours,
             t.start_date::text AS start_date,
             t.due_date::text AS due_date,
             t.created_by::text AS created_by,
             t.created_at::text AS created_at,
             t.updated_at::text AS updated_at,
             pv.visibility::text AS visibility,
             owner.owner_user_id,
             owner.owner_display_name,
             owner.owner_email
      FROM task t
      JOIN project_visibility pv ON pv.project_id = t.id
      LEFT JOIN LATERAL (
        SELECT tra.user_id::text AS owner_user_id,
               u.display_name AS owner_display_name,
               u.email AS owner_email
        FROM task_role_assignment tra
        JOIN app_user u ON u.id = tra.user_id
        WHERE tra.task_id = t.id
          AND tra.role = 'owner'
        ORDER BY tra.assigned_at ASC
        LIMIT 1
      ) owner ON TRUE
      LEFT JOIN task_assignment ta_me
        ON ta_me.task_id = t.id
       AND ta_me.user_id = $1::uuid
      WHERE t.parent_task_id IS NULL
        AND ($2 = '' OR pv.visibility::text = $2)
        )sql" + scopeWhere + R"sql(
      ORDER BY t.created_at DESC
      LIMIT )sql" +
        std::to_string(limit) + " OFFSET " + std::to_string(offset);

    auto rows = dbClient->execSqlSync(sql, requesterId, visibilityFilter);

    Json::Value out(Json::arrayValue);
    for (const auto& row : rows) {
      Json::Value item(Json::objectValue);
      item["id"] = row["id"].isNull() ? Json::Value()
                                        : Json::Value(row["id"].as<std::string>());
      item["title"] = row["title"].isNull()
                          ? Json::Value()
                          : Json::Value(row["title"].as<std::string>());
      item["description"] = row["description"].isNull()
                                ? Json::Value()
                                : Json::Value(row["description"].as<std::string>());
      item["priority"] = row["priority"].isNull()
                             ? Json::Value()
                             : Json::Value(row["priority"].as<std::string>());
      item["status"] = row["status"].isNull()
                           ? Json::Value()
                           : Json::Value(row["status"].as<std::string>());
      item["estimated_hours"] = row["estimated_hours"].isNull()
                                     ? Json::Value()
                                     : Json::Value(row["estimated_hours"].as<std::string>());
      item["start_date"] = row["start_date"].isNull()
                               ? Json::Value()
                               : Json::Value(row["start_date"].as<std::string>());
      item["due_date"] = row["due_date"].isNull()
                             ? Json::Value()
                             : Json::Value(row["due_date"].as<std::string>());
      item["created_by"] = row["created_by"].isNull()
                               ? Json::Value()
                               : Json::Value(row["created_by"].as<std::string>());
      item["created_at"] = row["created_at"].isNull()
                               ? Json::Value()
                               : Json::Value(row["created_at"].as<std::string>());
      item["updated_at"] = row["updated_at"].isNull()
                               ? Json::Value()
                               : Json::Value(row["updated_at"].as<std::string>());
      item["visibility"] = row["visibility"].isNull()
                               ? Json::Value()
                               : Json::Value(row["visibility"].as<std::string>());
      item["owner_user_id"] = row["owner_user_id"].isNull()
                                   ? Json::Value()
                                   : Json::Value(row["owner_user_id"].as<std::string>());
      item["owner_display_name"] = row["owner_display_name"].isNull()
                                       ? Json::Value()
                                       : Json::Value(row["owner_display_name"].as<std::string>());
      item["owner_email"] = row["owner_email"].isNull()
                                ? Json::Value()
                                : Json::Value(row["owner_email"].as<std::string>());

      out.append(item);
    }

    auto resp = HttpResponse::newHttpJsonResponse(out);
    resp->setStatusCode(k200OK);
    return callback(resp);
  } catch (const std::exception& e) {
    LOG_ERROR << "getProjects failed: " << e.what();
    auto resp =
        HttpResponse::newHttpJsonResponse(Json::Value("Internal server error"));
    resp->setStatusCode(k500InternalServerError);
    return callback(resp);
  }
}

void ProjectController::getProject(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback) {
  auto attrsPtr = req->attributes();
  if (!attrsPtr || !attrsPtr->find("user_id")) {
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Unauthorized"));
    resp->setStatusCode(k401Unauthorized);
    return callback(resp);
  }
  const std::string requesterId = attrsPtr->get<std::string>("user_id");
  if (requesterId.empty()) {
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Unauthorized"));
    resp->setStatusCode(k401Unauthorized);
    return callback(resp);
  }

  // Extract project_id from path: /api/projects/{project_id}
  std::string projectId = req->getParameter("project_id");
  if (projectId.empty()) {
    const std::string path = req->path();
    const size_t markerPos = path.find("/api/projects/");
    if (markerPos != std::string::npos) {
      const size_t idStart = markerPos + 14;
      const size_t idEnd = path.find('/', idStart);
      projectId = (idEnd == std::string::npos)
          ? path.substr(idStart)
          : path.substr(idStart, idEnd - idStart);
    }
  }

  if (projectId.empty()) {
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Missing project_id"));
    resp->setStatusCode(k400BadRequest);
    return callback(resp);
  }

  auto dbClient = app().getDbClient();
  try {
    const auto access = resolveProjectAccess(dbClient, projectId, requesterId);
    if (!access.exists) {
      auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Project not found"));
      resp->setStatusCode(k404NotFound);
      return callback(resp);
    }
    if (!access.canView) {
      auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Forbidden"));
      resp->setStatusCode(k403Forbidden);
      return callback(resp);
    }

    auto res = dbClient->execSqlSync(
        R"sql(
        SELECT t.id::text AS id,
               t.title,
               t.description,
               t.priority::text AS priority,
               t.status::text AS status,
               t.estimated_hours,
               t.start_date::text AS start_date,
               t.due_date::text AS due_date,
               t.created_by::text AS created_by,
               t.created_at::text AS created_at,
               t.updated_at::text AS updated_at,
               pv.visibility::text AS visibility,
               oref.user_id::text AS owner_user_id,
               owner_u.display_name AS owner_display_name,
               owner_u.email AS owner_email
        FROM task t
        JOIN project_visibility pv ON pv.project_id = t.id
        LEFT JOIN LATERAL (
          SELECT tra.user_id
          FROM task_role_assignment tra
          WHERE tra.task_id = t.id AND tra.role = 'owner'
          ORDER BY tra.assigned_at ASC LIMIT 1
        ) oref ON TRUE
        LEFT JOIN app_user owner_u ON owner_u.id = oref.user_id
        WHERE t.id = $1::uuid AND t.parent_task_id IS NULL
        LIMIT 1
        )sql",
        projectId);

    if (res.empty()) {
      auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Project not found"));
      resp->setStatusCode(k404NotFound);
      return callback(resp);
    }

    const auto& row = res[0];
    Json::Value out(Json::objectValue);
    auto s = [&](const char* col) -> Json::Value {
      return row[col].isNull() ? Json::Value() : Json::Value(row[col].as<std::string>());
    };
    out["id"] = s("id");
    out["title"] = s("title");
    out["description"] = s("description");
    out["priority"] = s("priority");
    out["status"] = s("status");
    out["estimated_hours"] = s("estimated_hours");
    out["start_date"] = s("start_date");
    out["due_date"] = s("due_date");
    out["created_by"] = s("created_by");
    out["created_at"] = s("created_at");
    out["updated_at"] = s("updated_at");
    out["visibility"] = s("visibility");
    out["owner_user_id"] = s("owner_user_id");
    out["owner_display_name"] = s("owner_display_name");
    out["owner_email"] = s("owner_email");

    auto resp = HttpResponse::newHttpJsonResponse(out);
    resp->setStatusCode(k200OK);
    return callback(resp);
  } catch (const std::exception& e) {
    LOG_ERROR << "getProject failed: " << e.what();
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Internal server error"));
    resp->setStatusCode(k500InternalServerError);
    return callback(resp);
  }
}

void ProjectController::getProjectProgress(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback) {
  auto attrsPtr = req->attributes();
  if (!attrsPtr || !attrsPtr->find("user_id")) {
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Unauthorized"));
    resp->setStatusCode(k401Unauthorized);
    return callback(resp);
  }

  const std::string requesterId = attrsPtr->get<std::string>("user_id");
  if (requesterId.empty()) {
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Unauthorized"));
    resp->setStatusCode(k401Unauthorized);
    return callback(resp);
  }

  std::string projectId = getPathVariableCompat(req, "project_id");
  if (projectId.empty() || projectId == "progress") {
    std::string path = req->path();
    const size_t markerPos = path.find("/api/projects/");
    if (markerPos != std::string::npos) {
      const size_t idStart = markerPos + 14;
      const size_t idEnd = path.find('/', idStart);
      if (idEnd != std::string::npos) {
        projectId = path.substr(idStart, idEnd - idStart);
      }
    }
  }

  if (projectId.empty()) {
    auto resp =
        HttpResponse::newHttpJsonResponse(Json::Value("Missing project_id"));
    resp->setStatusCode(k400BadRequest);
    return callback(resp);
  }

  auto dbClient = app().getDbClient();
  try {
    const auto access = resolveProjectAccess(dbClient, projectId, requesterId);
    if (!access.exists) {
      auto resp =
          HttpResponse::newHttpJsonResponse(Json::Value("Project not found"));
      resp->setStatusCode(k404NotFound);
      return callback(resp);
    }
    if (!access.canView) {
      auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Forbidden"));
      resp->setStatusCode(k403Forbidden);
      return callback(resp);
    }

    auto progressRes = dbClient->execSqlSync(
        R"sql(
        SELECT COUNT(*)::int AS total_tasks,
               COUNT(*) FILTER (WHERE status = 'completed')::int AS completed_tasks
        FROM task
        WHERE project_root_id = $1::uuid
      )sql",
        projectId);

    int totalTasks = 0;
    int completedTasks = 0;
    if (!progressRes.empty()) {
      totalTasks = progressRes[0]["total_tasks"].isNull()
                       ? 0
                       : progressRes[0]["total_tasks"].as<int>();
      completedTasks = progressRes[0]["completed_tasks"].isNull()
                           ? 0
                           : progressRes[0]["completed_tasks"].as<int>();
    }

    if (totalTasks == 0) {
      auto rootRes = dbClient->execSqlSync(
          "SELECT status::text AS status FROM task WHERE id = $1::uuid LIMIT 1",
          projectId);
      if (!rootRes.empty()) {
        totalTasks = 1;
        if (!rootRes[0]["status"].isNull() &&
            rootRes[0]["status"].as<std::string>() == "completed") {
          completedTasks = 1;
        }
      }
    }

    const double progressPercent =
        (totalTasks <= 0)
            ? 0.0
            : (static_cast<double>(completedTasks) * 100.0 /
               static_cast<double>(totalTasks));

    Json::Value out(Json::objectValue);
    out["project_id"] = projectId;
    out["total_tasks"] = totalTasks;
    out["completed_tasks"] = completedTasks;
    out["progress_percent"] = progressPercent;

    auto resp = HttpResponse::newHttpJsonResponse(out);
    resp->setStatusCode(k200OK);
    return callback(resp);
  } catch (const std::exception& e) {
    LOG_ERROR << "getProjectProgress failed: " << e.what();
    auto resp =
        HttpResponse::newHttpJsonResponse(Json::Value("Internal server error"));
    resp->setStatusCode(k500InternalServerError);
    return callback(resp);
  }
}

void ProjectController::getProjectParticipants(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback) {
  auto attrsPtr = req->attributes();
  if (!attrsPtr || !attrsPtr->find("user_id")) {
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Unauthorized"));
    resp->setStatusCode(k401Unauthorized);
    return callback(resp);
  }

  const std::string requesterId = attrsPtr->get<std::string>("user_id");
  if (requesterId.empty()) {
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Unauthorized"));
    resp->setStatusCode(k401Unauthorized);
    return callback(resp);
  }

  std::string projectId = getPathVariableCompat(req, "project_id");
  if (projectId.empty() || projectId == "participants") {
    std::string path = req->path();
    const size_t markerPos = path.find("/api/projects/");
    if (markerPos != std::string::npos) {
      const size_t idStart = markerPos + 14;
      const size_t idEnd = path.find('/', idStart);
      if (idEnd != std::string::npos) {
        projectId = path.substr(idStart, idEnd - idStart);
      }
    }
  }

  if (projectId.empty()) {
    auto resp =
        HttpResponse::newHttpJsonResponse(Json::Value("Missing project_id"));
    resp->setStatusCode(k400BadRequest);
    return callback(resp);
  }

  const std::string roleFilter = req->getParameter("role");
  if (!roleFilter.empty() && !isAllowedRole(roleFilter)) {
    auto resp = HttpResponse::newHttpJsonResponse(
        Json::Value("Invalid role filter"));
    resp->setStatusCode(k400BadRequest);
    return callback(resp);
  }

  std::string sortBy = req->getParameter("sort_by");
  if (sortBy.empty()) {
    sortBy = "display_name";
  }

  std::string sortOrder = req->getParameter("order");
  if (sortOrder.empty()) {
    sortOrder = "asc";
  }

  if (sortOrder != "asc" && sortOrder != "desc") {
    auto resp = HttpResponse::newHttpJsonResponse(
        Json::Value("order must be asc or desc"));
    resp->setStatusCode(k400BadRequest);
    return callback(resp);
  }

  std::string sortColumn;
  if (sortBy == "role") {
    sortColumn = "tr.role";
  } else if (sortBy == "assigned_at") {
    sortColumn = "COALESCE(tr.assigned_at, ta.assigned_at)";
  } else if (sortBy == "display_name") {
    sortColumn = "u.display_name";
  } else {
    auto resp = HttpResponse::newHttpJsonResponse(
        Json::Value("sort_by must be one of: role, display_name, assigned_at"));
    resp->setStatusCode(k400BadRequest);
    return callback(resp);
  }

  auto dbClient = app().getDbClient();
  try {
    const auto access = resolveProjectAccess(dbClient, projectId, requesterId);
    if (!access.exists) {
      auto resp =
          HttpResponse::newHttpJsonResponse(Json::Value("Project not found"));
      resp->setStatusCode(k404NotFound);
      return callback(resp);
    }
    if (!access.canView) {
      auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Forbidden"));
      resp->setStatusCode(k403Forbidden);
      return callback(resp);
    }

    const std::string sql =
        R"sql(
      SELECT ta.user_id::text AS user_id,
             u.display_name,
             u.email,
             ta.assigned_hours,
             ta.assigned_at::text AS assigned_at,
             tr.role::text AS role,
             tr.assigned_at::text AS role_assigned_at
      FROM task_assignment ta
      JOIN app_user u ON u.id = ta.user_id
      LEFT JOIN LATERAL (
        SELECT tra.role,
               tra.assigned_at
        FROM task_role_assignment tra
        WHERE tra.task_id = ta.task_id
          AND tra.user_id = ta.user_id
        ORDER BY tra.assigned_at DESC
        LIMIT 1
      ) tr ON TRUE
      WHERE ta.task_id = $1::uuid
        AND ($2 = '' OR tr.role::text = $2)
      ORDER BY )sql" +
        sortColumn + " " + sortOrder +
        R"sql( NULLS LAST, u.display_name ASC, ta.assigned_at ASC
    )sql";

    auto rows = dbClient->execSqlSync(sql, projectId, roleFilter);

    Json::Value out(Json::arrayValue);
    for (const auto& row : rows) {
      Json::Value item(Json::objectValue);
      item["user_id"] = row["user_id"].isNull()
                            ? Json::Value()
                            : Json::Value(row["user_id"].as<std::string>());
      item["display_name"] = row["display_name"].isNull()
                                 ? Json::Value()
                                 : Json::Value(row["display_name"].as<std::string>());
      item["email"] = row["email"].isNull()
                          ? Json::Value()
                          : Json::Value(row["email"].as<std::string>());
      item["role"] = row["role"].isNull()
                         ? Json::Value()
                         : Json::Value(row["role"].as<std::string>());
      item["assigned_hours"] = row["assigned_hours"].isNull()
                                   ? Json::Value()
                                   : Json::Value(row["assigned_hours"].as<std::string>());
      item["assigned_at"] = row["assigned_at"].isNull()
                                ? Json::Value()
                                : Json::Value(row["assigned_at"].as<std::string>());
      item["role_assigned_at"] = row["role_assigned_at"].isNull()
                                     ? Json::Value()
                                     : Json::Value(row["role_assigned_at"].as<std::string>());

      out.append(item);
    }

    auto resp = HttpResponse::newHttpJsonResponse(out);
    resp->setStatusCode(k200OK);
    return callback(resp);
  } catch (const std::exception& e) {
    LOG_ERROR << "getProjectParticipants failed: " << e.what();
    auto resp =
        HttpResponse::newHttpJsonResponse(Json::Value("Internal server error"));
    resp->setStatusCode(k500InternalServerError);
    return callback(resp);
  }
}

void ProjectController::deleteProject(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback) {
  auto attrsPtr = req->attributes();
  if (!attrsPtr || !attrsPtr->find("user_id")) {
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Unauthorized"));
    resp->setStatusCode(k401Unauthorized);
    return callback(resp);
  }
  const std::string requesterId = attrsPtr->get<std::string>("user_id");
  if (requesterId.empty()) {
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Unauthorized"));
    resp->setStatusCode(k401Unauthorized);
    return callback(resp);
  }

  std::string projectId = req->getParameter("project_id");
  if (projectId.empty()) {
    const std::string path = req->path();
    const size_t markerPos = path.find("/api/projects/");
    if (markerPos != std::string::npos) {
      const size_t idStart = markerPos + 14;
      const size_t idEnd = path.find('/', idStart);
      projectId = (idEnd == std::string::npos) ? path.substr(idStart) : path.substr(idStart, idEnd - idStart);
    }
  }

  if (projectId.empty()) {
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Missing project_id"));
    resp->setStatusCode(k400BadRequest);
    return callback(resp);
  }

  auto dbClient = app().getDbClient();
  try {
    // Check project exists
    const auto access = resolveProjectAccess(dbClient, projectId, requesterId);
    if (!access.exists) {
      auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Project not found"));
      resp->setStatusCode(k404NotFound);
      return callback(resp);
    }

    // Check caller is owner
    const std::string callerRole = getCallerProjectRole(dbClient, projectId, requesterId);
    if (callerRole != "owner") {
      auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Forbidden: only owner can delete a project"));
      resp->setStatusCode(k403Forbidden);
      return callback(resp);
    }

    // Delete project (FK ON DELETE CASCADE removes subtasks)
    dbClient->execSqlSync(
        "DELETE FROM task WHERE id = $1::uuid AND parent_task_id IS NULL",
        projectId);

    Json::Value out(Json::objectValue);
    out["deleted"] = true;
    out["id"] = projectId;

    auto resp = HttpResponse::newHttpJsonResponse(out);
    resp->setStatusCode(k200OK);
    return callback(resp);
  } catch (const std::exception& e) {
    LOG_ERROR << "deleteProject failed: " << e.what();
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Internal server error"));
    resp->setStatusCode(k500InternalServerError);
    return callback(resp);
  }
}

void ProjectController::updateProject(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback) {
  auto attrsPtr = req->attributes();
  if (!attrsPtr || !attrsPtr->find("user_id")) {
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Unauthorized"));
    resp->setStatusCode(k401Unauthorized);
    return callback(resp);
  }
  const std::string requesterId = attrsPtr->get<std::string>("user_id");
  if (requesterId.empty()) {
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Unauthorized"));
    resp->setStatusCode(k401Unauthorized);
    return callback(resp);
  }

  std::string projectId = req->getParameter("project_id");
  if (projectId.empty()) {
    const std::string path = req->path();
    const size_t markerPos = path.find("/api/projects/");
    if (markerPos != std::string::npos) {
      const size_t idStart = markerPos + 14;
      const size_t idEnd = path.find('/', idStart);
      projectId = (idEnd == std::string::npos) ? path.substr(idStart) : path.substr(idStart, idEnd - idStart);
    }
  }

  if (projectId.empty()) {
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Missing project_id"));
    resp->setStatusCode(k400BadRequest);
    return callback(resp);
  }

  auto jsonPtr = req->getJsonObject();
  if (!jsonPtr || !jsonPtr->isObject()) {
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Invalid JSON"));
    resp->setStatusCode(k400BadRequest);
    return callback(resp);
  }
  const Json::Value& j = *jsonPtr;

  auto dbClient = app().getDbClient();
  try {
    // Check project exists
    const auto access = resolveProjectAccess(dbClient, projectId, requesterId);
    if (!access.exists) {
      auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Project not found"));
      resp->setStatusCode(k404NotFound);
      return callback(resp);
    }

    // Check caller is owner or admin
    const std::string callerRole = getCallerProjectRole(dbClient, projectId, requesterId);
    if (!isOwnerOrAdmin(callerRole)) {
      auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Forbidden: only owner or admin can update a project"));
      resp->setStatusCode(k403Forbidden);
      return callback(resp);
    }

    dbClient->execSqlSync("BEGIN");

    if (j.isMember("title") && j["title"].isString() && !j["title"].asString().empty()) {
      dbClient->execSqlSync("UPDATE task SET title = $2 WHERE id = $1::uuid", projectId, j["title"].asString());
    }
    if (j.isMember("description") && j["description"].isString()) {
      dbClient->execSqlSync("UPDATE task SET description = $2 WHERE id = $1::uuid", projectId, j["description"].asString());
    }
    if (j.isMember("status") && j["status"].isString() && !j["status"].asString().empty()) {
      dbClient->execSqlSync("UPDATE task SET status = $2::task_status_enum WHERE id = $1::uuid", projectId, j["status"].asString());
    }
    if (j.isMember("priority") && j["priority"].isString() && !j["priority"].asString().empty()) {
      dbClient->execSqlSync("UPDATE task SET priority = $2::task_priority_enum WHERE id = $1::uuid", projectId, j["priority"].asString());
    }
    if (j.isMember("start_date") && j["start_date"].isString() && !j["start_date"].asString().empty()) {
      dbClient->execSqlSync("UPDATE task SET start_date = $2::date WHERE id = $1::uuid", projectId, j["start_date"].asString());
    }
    if (j.isMember("due_date") && j["due_date"].isString() && !j["due_date"].asString().empty()) {
      dbClient->execSqlSync("UPDATE task SET due_date = $2::date WHERE id = $1::uuid", projectId, j["due_date"].asString());
    }
    if (j.isMember("wanted_skills") && j["wanted_skills"].isArray()) {
      std::string arr = "{";
      for (unsigned i = 0; i < j["wanted_skills"].size(); ++i) {
        if (i > 0) arr += ",";
        arr += j["wanted_skills"][i].asString();
      }
      arr += "}";
      dbClient->execSqlSync("UPDATE task SET wanted_skills = $2::text[] WHERE id = $1::uuid", projectId, arr);
    }
    // Always update updated_at
    dbClient->execSqlSync("UPDATE task SET updated_at = NOW() WHERE id = $1::uuid", projectId);

    if (j.isMember("visibility") && j["visibility"].isString()) {
      const std::string vis = j["visibility"].asString();
      if (!isAllowedVisibility(vis)) {
        dbClient->execSqlSync("ROLLBACK");
        auto resp = HttpResponse::newHttpJsonResponse(Json::Value("visibility must be public or private"));
        resp->setStatusCode(k400BadRequest);
        return callback(resp);
      }
      dbClient->execSqlSync(
          "UPDATE project_visibility SET visibility = $2::project_visibility_enum, updated_at = NOW() WHERE project_id = $1::uuid",
          projectId, vis);
    }

    dbClient->execSqlSync("COMMIT");

    // Fetch and return the updated project
    auto res = dbClient->execSqlSync(
        R"sql(
        SELECT t.id::text AS id,
               t.title,
               t.description,
               t.priority::text AS priority,
               t.status::text AS status,
               t.estimated_hours,
               t.start_date::text AS start_date,
               t.due_date::text AS due_date,
               t.created_by::text AS created_by,
               t.created_at::text AS created_at,
               t.updated_at::text AS updated_at,
               pv.visibility::text AS visibility,
               oref.user_id::text AS owner_user_id,
               owner_u.display_name AS owner_display_name,
               owner_u.email AS owner_email
        FROM task t
        JOIN project_visibility pv ON pv.project_id = t.id
        LEFT JOIN LATERAL (
          SELECT tra.user_id
          FROM task_role_assignment tra
          WHERE tra.task_id = t.id AND tra.role = 'owner'
          ORDER BY tra.assigned_at ASC LIMIT 1
        ) oref ON TRUE
        LEFT JOIN app_user owner_u ON owner_u.id = oref.user_id
        WHERE t.id = $1::uuid AND t.parent_task_id IS NULL
        LIMIT 1
        )sql",
        projectId);

    if (res.empty()) {
      auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Project updated but cannot fetch it"));
      resp->setStatusCode(k500InternalServerError);
      return callback(resp);
    }

    const auto& row = res[0];
    Json::Value out(Json::objectValue);
    auto s = [&](const char* col) -> Json::Value {
      return row[col].isNull() ? Json::Value() : Json::Value(row[col].as<std::string>());
    };
    out["id"] = s("id");
    out["title"] = s("title");
    out["description"] = s("description");
    out["priority"] = s("priority");
    out["status"] = s("status");
    out["estimated_hours"] = s("estimated_hours");
    out["start_date"] = s("start_date");
    out["due_date"] = s("due_date");
    out["created_by"] = s("created_by");
    out["created_at"] = s("created_at");
    out["updated_at"] = s("updated_at");
    out["visibility"] = s("visibility");
    out["owner_user_id"] = s("owner_user_id");
    out["owner_display_name"] = s("owner_display_name");
    out["owner_email"] = s("owner_email");

    auto resp = HttpResponse::newHttpJsonResponse(out);
    resp->setStatusCode(k200OK);
    return callback(resp);
  } catch (const std::exception& e) {
    LOG_ERROR << "updateProject failed: " << e.what();
    try { dbClient->execSqlSync("ROLLBACK"); } catch (...) {}
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Internal server error"));
    resp->setStatusCode(k500InternalServerError);
    return callback(resp);
  }
}

// ─── POST /api/projects/{project_id}/invite ───────────────────────────────────
void ProjectController::inviteUser(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback) {
  auto attrsPtr = req->attributes();
  if (!attrsPtr || !attrsPtr->find("user_id")) {
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Unauthorized"));
    resp->setStatusCode(k401Unauthorized);
    return callback(resp);
  }
  const std::string requesterId = attrsPtr->get<std::string>("user_id");
  if (requesterId.empty()) {
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Unauthorized"));
    resp->setStatusCode(k401Unauthorized);
    return callback(resp);
  }

  std::string projectId = req->getParameter("project_id");
  if (projectId.empty()) {
    const std::string path = req->path();
    const size_t markerPos = path.find("/api/projects/");
    if (markerPos != std::string::npos) {
      const size_t idStart = markerPos + 14;
      const size_t idEnd = path.find('/', idStart);
      projectId = (idEnd == std::string::npos) ? path.substr(idStart) : path.substr(idStart, idEnd - idStart);
    }
  }
  if (projectId.empty()) {
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Missing project_id"));
    resp->setStatusCode(k400BadRequest);
    return callback(resp);
  }

  auto jsonPtr = req->getJsonObject();
  if (!jsonPtr || !jsonPtr->isObject()) {
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Invalid JSON"));
    resp->setStatusCode(k400BadRequest);
    return callback(resp);
  }
  const Json::Value& j = *jsonPtr;

  auto dbClient = app().getDbClient();
  try {
    // Caller must be owner
    const std::string callerRole = getCallerProjectRole(dbClient, projectId, requesterId);
    if (!isOwnerOrAdmin(callerRole)) {
      auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Forbidden: owner required"));
      resp->setStatusCode(k403Forbidden);
      return callback(resp);
    }

    // Resolve invitee
    std::string inviteeUserId;
    if (j.isMember("user_id") && j["user_id"].isString() && !j["user_id"].asString().empty()) {
      inviteeUserId = j["user_id"].asString();
      auto chk = dbClient->execSqlSync(
          "SELECT 1 FROM app_user WHERE id = $1::uuid LIMIT 1", inviteeUserId);
      if (chk.empty()) {
        auto resp = HttpResponse::newHttpJsonResponse(Json::Value("User not found"));
        resp->setStatusCode(k404NotFound);
        return callback(resp);
      }
    } else if (j.isMember("email") && j["email"].isString() && !j["email"].asString().empty()) {
      const std::string email = j["email"].asString();
      auto chk = dbClient->execSqlSync(
          "SELECT id::text AS id FROM app_user WHERE email = $1 LIMIT 1", email);
      if (chk.empty()) {
        auto resp = HttpResponse::newHttpJsonResponse(Json::Value("User not found"));
        resp->setStatusCode(k404NotFound);
        return callback(resp);
      }
      inviteeUserId = chk[0]["id"].as<std::string>();
    } else {
      auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Provide user_id or email"));
      resp->setStatusCode(k400BadRequest);
      return callback(resp);
    }

    // Insert invitation
    dbClient->execSqlSync(
        R"sql(
        INSERT INTO project_invitation
          (project_id, inviter_user_id, invitee_user_id, kind, status)
        VALUES ($1::uuid, $2::uuid, $3::uuid, 'invite', 'pending')
        )sql",
        projectId, requesterId, inviteeUserId);

    Json::Value out(Json::objectValue);
    out["invited"] = true;
    out["invitee_user_id"] = inviteeUserId;
    auto resp = HttpResponse::newHttpJsonResponse(out);
    resp->setStatusCode(k201Created);
    return callback(resp);
  } catch (const std::exception& e) {
    LOG_ERROR << "inviteUser failed: " << e.what();
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Internal server error"));
    resp->setStatusCode(k500InternalServerError);
    return callback(resp);
  }
}

// ─── POST /api/projects/{project_id}/apply ───────────────────────────────────
void ProjectController::applyToProject(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback) {
  auto attrsPtr = req->attributes();
  if (!attrsPtr || !attrsPtr->find("user_id")) {
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Unauthorized"));
    resp->setStatusCode(k401Unauthorized);
    return callback(resp);
  }
  const std::string requesterId = attrsPtr->get<std::string>("user_id");
  if (requesterId.empty()) {
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Unauthorized"));
    resp->setStatusCode(k401Unauthorized);
    return callback(resp);
  }

  std::string projectId = req->getParameter("project_id");
  if (projectId.empty()) {
    const std::string path = req->path();
    const size_t markerPos = path.find("/api/projects/");
    if (markerPos != std::string::npos) {
      const size_t idStart = markerPos + 14;
      const size_t idEnd = path.find('/', idStart);
      projectId = (idEnd == std::string::npos) ? path.substr(idStart) : path.substr(idStart, idEnd - idStart);
    }
  }
  if (projectId.empty()) {
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Missing project_id"));
    resp->setStatusCode(k400BadRequest);
    return callback(resp);
  }

  auto dbClient = app().getDbClient();
  try {
    // Check project exists
    auto projChk = dbClient->execSqlSync(
        "SELECT 1 FROM task WHERE id = $1::uuid AND parent_task_id IS NULL LIMIT 1",
        projectId);
    if (projChk.empty()) {
      auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Project not found"));
      resp->setStatusCode(k404NotFound);
      return callback(resp);
    }

    // Check not already a member
    auto memberChk = dbClient->execSqlSync(
        "SELECT 1 FROM task_assignment WHERE task_id = $1::uuid AND user_id = $2::uuid LIMIT 1",
        projectId, requesterId);
    if (!memberChk.empty()) {
      auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Already a member"));
      resp->setStatusCode(k409Conflict);
      return callback(resp);
    }

    // Insert application
    dbClient->execSqlSync(
        R"sql(
        INSERT INTO project_invitation
          (project_id, invitee_user_id, kind, status)
        VALUES ($1::uuid, $2::uuid, 'application', 'pending')
        )sql",
        projectId, requesterId);

    Json::Value out(Json::objectValue);
    out["applied"] = true;
    auto resp = HttpResponse::newHttpJsonResponse(out);
    resp->setStatusCode(k201Created);
    return callback(resp);
  } catch (const std::exception& e) {
    LOG_ERROR << "applyToProject failed: " << e.what();
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Internal server error"));
    resp->setStatusCode(k500InternalServerError);
    return callback(resp);
  }
}

// ─── GET /api/projects/{project_id}/applications ─────────────────────────────
void ProjectController::listApplications(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback) {
  auto attrsPtr = req->attributes();
  if (!attrsPtr || !attrsPtr->find("user_id")) {
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Unauthorized"));
    resp->setStatusCode(k401Unauthorized);
    return callback(resp);
  }
  const std::string requesterId = attrsPtr->get<std::string>("user_id");
  if (requesterId.empty()) {
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Unauthorized"));
    resp->setStatusCode(k401Unauthorized);
    return callback(resp);
  }

  std::string projectId = req->getParameter("project_id");
  if (projectId.empty()) {
    const std::string path = req->path();
    const size_t markerPos = path.find("/api/projects/");
    if (markerPos != std::string::npos) {
      const size_t idStart = markerPos + 14;
      const size_t idEnd = path.find('/', idStart);
      projectId = (idEnd == std::string::npos) ? path.substr(idStart) : path.substr(idStart, idEnd - idStart);
    }
  }
  if (projectId.empty()) {
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Missing project_id"));
    resp->setStatusCode(k400BadRequest);
    return callback(resp);
  }

  auto dbClient = app().getDbClient();
  try {
    // Caller must be owner
    const std::string callerRole = getCallerProjectRole(dbClient, projectId, requesterId);
    if (!isOwnerOrAdmin(callerRole)) {
      auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Forbidden: owner required"));
      resp->setStatusCode(k403Forbidden);
      return callback(resp);
    }

    auto rows = dbClient->execSqlSync(
        R"sql(
        SELECT pi.id::text AS id, pi.kind::text AS kind, pi.status::text AS status,
               pi.created_at::text AS created_at,
               u.id::text AS user_id, u.display_name, u.email
        FROM project_invitation pi
        LEFT JOIN app_user u ON u.id = pi.invitee_user_id
        WHERE pi.project_id = $1::uuid AND pi.status = 'pending' AND pi.kind = 'application'::invitation_kind_enum
        ORDER BY pi.created_at DESC
        )sql",
        projectId);

    Json::Value out(Json::arrayValue);
    for (const auto& row : rows) {
      Json::Value item(Json::objectValue);
      item["id"] = row["id"].isNull() ? Json::Value() : Json::Value(row["id"].as<std::string>());
      item["kind"] = row["kind"].isNull() ? Json::Value() : Json::Value(row["kind"].as<std::string>());
      item["status"] = row["status"].isNull() ? Json::Value() : Json::Value(row["status"].as<std::string>());
      item["created_at"] = row["created_at"].isNull() ? Json::Value() : Json::Value(row["created_at"].as<std::string>());
      item["user_id"] = row["user_id"].isNull() ? Json::Value() : Json::Value(row["user_id"].as<std::string>());
      item["display_name"] = row["display_name"].isNull() ? Json::Value() : Json::Value(row["display_name"].as<std::string>());
      item["email"] = row["email"].isNull() ? Json::Value() : Json::Value(row["email"].as<std::string>());
      out.append(item);
    }

    auto resp = HttpResponse::newHttpJsonResponse(out);
    resp->setStatusCode(k200OK);
    return callback(resp);
  } catch (const std::exception& e) {
    LOG_ERROR << "listApplications failed: " << e.what();
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Internal server error"));
    resp->setStatusCode(k500InternalServerError);
    return callback(resp);
  }
}

// ─── POST /api/projects/{project_id}/applications/{application_id}/approve ───
void ProjectController::approveApplication(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback) {
  auto attrsPtr = req->attributes();
  if (!attrsPtr || !attrsPtr->find("user_id")) {
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Unauthorized"));
    resp->setStatusCode(k401Unauthorized);
    return callback(resp);
  }
  const std::string requesterId = attrsPtr->get<std::string>("user_id");
  if (requesterId.empty()) {
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Unauthorized"));
    resp->setStatusCode(k401Unauthorized);
    return callback(resp);
  }

  const std::string path = req->path();
  std::string projectId = req->getParameter("project_id");
  if (projectId.empty()) {
    const size_t markerPos = path.find("/api/projects/");
    if (markerPos != std::string::npos) {
      const size_t idStart = markerPos + 14;
      const size_t idEnd = path.find('/', idStart);
      projectId = (idEnd == std::string::npos) ? path.substr(idStart) : path.substr(idStart, idEnd - idStart);
    }
  }

  std::string applicationId = req->getParameter("application_id");
  if (applicationId.empty()) {
    const size_t am = path.find("/applications/");
    if (am != std::string::npos) {
      const size_t idStart = am + 14;
      const size_t idEnd = path.find('/', idStart);
      applicationId = (idEnd == std::string::npos) ? path.substr(idStart) : path.substr(idStart, idEnd - idStart);
    }
  }

  if (projectId.empty() || applicationId.empty()) {
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Missing project_id or application_id"));
    resp->setStatusCode(k400BadRequest);
    return callback(resp);
  }

  auto dbClient = app().getDbClient();
  try {
    // Caller must be owner
    const std::string callerRole = getCallerProjectRole(dbClient, projectId, requesterId);
    if (!isOwnerOrAdmin(callerRole)) {
      auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Forbidden: owner required"));
      resp->setStatusCode(k403Forbidden);
      return callback(resp);
    }

    // Verify invitation exists and is pending
    auto invRes = dbClient->execSqlSync(
        R"sql(
        SELECT invitee_user_id::text AS invitee_user_id
        FROM project_invitation
        WHERE id = $1::uuid AND project_id = $2::uuid AND status = 'pending'
        LIMIT 1
        )sql",
        applicationId, projectId);
    if (invRes.empty()) {
      auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Application not found"));
      resp->setStatusCode(k404NotFound);
      return callback(resp);
    }
    const std::string inviteeId = invRes[0]["invitee_user_id"].as<std::string>();

    // Transaction: update invitation + add member
    dbClient->execSqlSync("BEGIN");

    dbClient->execSqlSync(
        R"sql(
        UPDATE project_invitation
        SET status = 'approved', decided_at = NOW(), decided_by = $2::uuid
        WHERE id = $1::uuid
        )sql",
        applicationId, requesterId);

    dbClient->execSqlSync(
        R"sql(
        INSERT INTO task_assignment (task_id, user_id)
        VALUES ($1::uuid, $2::uuid)
        ON CONFLICT DO NOTHING
        )sql",
        projectId, inviteeId);

    dbClient->execSqlSync(
        R"sql(
        INSERT INTO task_role_assignment (task_id, user_id, role)
        VALUES ($1::uuid, $2::uuid, 'executor')
        ON CONFLICT DO NOTHING
        )sql",
        projectId, inviteeId);

    dbClient->execSqlSync("COMMIT");

    Json::Value out(Json::objectValue);
    out["approved"] = true;
    out["user_id"] = inviteeId;
    auto resp = HttpResponse::newHttpJsonResponse(out);
    resp->setStatusCode(k200OK);
    return callback(resp);
  } catch (const std::exception& e) {
    LOG_ERROR << "approveApplication failed: " << e.what();
    try { dbClient->execSqlSync("ROLLBACK"); } catch (...) {}
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Internal server error"));
    resp->setStatusCode(k500InternalServerError);
    return callback(resp);
  }
}

// ─── POST /api/projects/{project_id}/applications/{application_id}/reject ────
void ProjectController::rejectApplication(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback) {
  auto attrsPtr = req->attributes();
  if (!attrsPtr || !attrsPtr->find("user_id")) {
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Unauthorized"));
    resp->setStatusCode(k401Unauthorized);
    return callback(resp);
  }
  const std::string requesterId = attrsPtr->get<std::string>("user_id");
  if (requesterId.empty()) {
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Unauthorized"));
    resp->setStatusCode(k401Unauthorized);
    return callback(resp);
  }

  const std::string path = req->path();
  std::string projectId = req->getParameter("project_id");
  if (projectId.empty()) {
    const size_t markerPos = path.find("/api/projects/");
    if (markerPos != std::string::npos) {
      const size_t idStart = markerPos + 14;
      const size_t idEnd = path.find('/', idStart);
      projectId = (idEnd == std::string::npos) ? path.substr(idStart) : path.substr(idStart, idEnd - idStart);
    }
  }

  std::string applicationId = req->getParameter("application_id");
  if (applicationId.empty()) {
    const size_t am = path.find("/applications/");
    if (am != std::string::npos) {
      const size_t idStart = am + 14;
      const size_t idEnd = path.find('/', idStart);
      applicationId = (idEnd == std::string::npos) ? path.substr(idStart) : path.substr(idStart, idEnd - idStart);
    }
  }

  if (projectId.empty() || applicationId.empty()) {
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Missing project_id or application_id"));
    resp->setStatusCode(k400BadRequest);
    return callback(resp);
  }

  auto dbClient = app().getDbClient();
  try {
    // Caller must be owner
    const std::string callerRole = getCallerProjectRole(dbClient, projectId, requesterId);
    if (!isOwnerOrAdmin(callerRole)) {
      auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Forbidden: owner required"));
      resp->setStatusCode(k403Forbidden);
      return callback(resp);
    }

    // Verify invitation exists and is pending
    auto invRes = dbClient->execSqlSync(
        R"sql(
        SELECT invitee_user_id::text AS invitee_user_id
        FROM project_invitation
        WHERE id = $1::uuid AND project_id = $2::uuid AND status = 'pending'
        LIMIT 1
        )sql",
        applicationId, projectId);
    if (invRes.empty()) {
      auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Application not found"));
      resp->setStatusCode(k404NotFound);
      return callback(resp);
    }

    dbClient->execSqlSync(
        R"sql(
        UPDATE project_invitation
        SET status = 'rejected', decided_at = NOW(), decided_by = $2::uuid
        WHERE id = $1::uuid
        )sql",
        applicationId, requesterId);

    Json::Value out(Json::objectValue);
    out["rejected"] = true;
    auto resp = HttpResponse::newHttpJsonResponse(out);
    resp->setStatusCode(k200OK);
    return callback(resp);
  } catch (const std::exception& e) {
    LOG_ERROR << "rejectApplication failed: " << e.what();
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Internal server error"));
    resp->setStatusCode(k500InternalServerError);
    return callback(resp);
  }
}
