#include "TaskController.h"

#include <drogon/HttpResponse.h>
#include <drogon/drogon.h>
#include <drogon/orm/Mapper.h>
#include <drogon/orm/Result.h>
#include <json/json.h>
#include <trantor/utils/Logger.h>

#include <algorithm>
#include <exception>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "../models/CalendarTask.h"
#include "../models/TaskAssignment.h"
#include "../models/TaskRoleAssignment.h"
#include "../models/TaskSchedule.h"

using namespace drogon;

static std::string getPathVariableCompat(const HttpRequestPtr& req,
                                         const std::string& name = "id") {
  const std::string q = req->getParameter(name);
  if (!q.empty()) return q;
  const std::string p = req->path();
  if (p.empty()) return {};
  auto pos = p.find_last_not_of('/');
  if (pos == std::string::npos) return {};
  auto start = p.find_last_of('/', pos);
  if (start == std::string::npos)
    start = 0;
  else
    ++start;
  return p.substr(start, pos - start + 1);
}

static bool hasOwnerPermission(
    const std::shared_ptr<drogon::orm::DbClient>& dbClient,
    const std::string& taskId, const std::string& userId) {
  try {
    auto res = dbClient->execSqlSync(
        "SELECT created_by FROM \"task\" WHERE id = $1 LIMIT 1", taskId);
    if (res.empty()) return false;
    if (!res[0]["created_by"].isNull() &&
        res[0]["created_by"].as<std::string>() == userId)
      return true;
    auto r = dbClient->execSqlSync(
        "SELECT 1 FROM \"task_role_assignment\" "
        "WHERE task_id = $1 AND user_id = $2 AND role = $3 "
        "LIMIT 1",
        taskId, userId, std::string("owner"));
    return !r.empty();
  } catch (...) {
    return false;
  }
}

void TaskController::createTask(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback) {
  auto jsonPtr = req->getJsonObject();
  if (!jsonPtr || !jsonPtr->isObject()) {
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Invalid JSON"));
    resp->setStatusCode(k400BadRequest);
    return callback(resp);
  }
  const Json::Value& j = *jsonPtr;

  if (!j.isMember("title") || j["title"].isNull() || !j["title"].isString()) {
    auto resp = HttpResponse::newHttpJsonResponse(
        Json::Value("Missing or invalid title"));
    resp->setStatusCode(k400BadRequest);
    return callback(resp);
  }

  auto attrsPtr = req->attributes();
  if (!attrsPtr || !attrsPtr->find("user_id")) {
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Unauthorized"));
    resp->setStatusCode(k401Unauthorized);
    return callback(resp);
  }
  const std::string userId = attrsPtr->get<std::string>("user_id");
  if (userId.empty()) {
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Unauthorized"));
    resp->setStatusCode(k401Unauthorized);
    return callback(resp);
  }

  auto dbClient = app().getDbClient();
  try {
    dbClient->execSqlSync("BEGIN");

    std::optional<std::string> parentId;
    if (j.isMember("parent_task_id") && !j["parent_task_id"].isNull()) {
      if (!j["parent_task_id"].isString()) {
        auto resp = HttpResponse::newHttpJsonResponse(
            Json::Value("Invalid parent_task_id"));
        resp->setStatusCode(k400BadRequest);
        dbClient->execSqlSync("ROLLBACK");
        return callback(resp);
      }
      parentId = j["parent_task_id"].asString();
      auto parentRes = dbClient->execSqlSync(
          "SELECT id FROM \"task\" WHERE id = $1 LIMIT 1", *parentId);
      if (parentRes.empty()) {
        auto resp = HttpResponse::newHttpJsonResponse(
            Json::Value("parent_task_id not found"));
        resp->setStatusCode(k400BadRequest);
        dbClient->execSqlSync("ROLLBACK");
        return callback(resp);
      }
    }

    drogon_model::project_calendar::CalendarTask task;
    try {
      task.updateByJson(j);
    } catch (...) {
    }
    if (parentId)
      task.setParentTaskId(*parentId);
    else
      task.setParentTaskIdToNull();

    task.setCreatedBy(userId);
    task.setCreatedAt(::trantor::Date::now());
    task.setUpdatedAt(::trantor::Date::now());

    drogon::orm::Mapper<drogon_model::project_calendar::CalendarTask>
        taskMapper(dbClient);
    taskMapper.insert(task);

    std::string taskId;
    try {
      taskId = task.getValueOfId();
    } catch (...) {
      taskId.clear();
    }
    if (taskId.empty()) {
      auto res = dbClient->execSqlSync(
          "SELECT id FROM \"task\" WHERE created_by = $1 AND title = $2 "
          "ORDER BY created_at DESC LIMIT 1",
          userId, task.getValueOfTitle());
      if (res.empty()) {
        dbClient->execSqlSync("ROLLBACK");
        auto resp = HttpResponse::newHttpJsonResponse(
            Json::Value("Failed to determine inserted task id"));
        resp->setStatusCode(k500InternalServerError);
        return callback(resp);
      }
      taskId = res[0]["id"].as<std::string>();
    }

    drogon_model::project_calendar::TaskAssignment ta;
    ta.setTaskId(taskId);
    ta.setUserId(userId);
    ta.setAssignedAt(::trantor::Date::now());
    drogon::orm::Mapper<drogon_model::project_calendar::TaskAssignment>
        taMapper(dbClient);
    taMapper.insert(ta);

    drogon_model::project_calendar::TaskRoleAssignment tra;
    tra.setTaskId(taskId);
    tra.setUserId(userId);
    tra.setRole(std::string("owner"));
    tra.setAssignedAt(::trantor::Date::now());
    drogon::orm::Mapper<drogon_model::project_calendar::TaskRoleAssignment>
        traMapper(dbClient);
    traMapper.insert(tra);

    dbClient->execSqlSync("COMMIT");

    auto finalRes = dbClient->execSqlSync(
        R"sql(
        SELECT id, parent_task_id, title, description, priority, status, estimated_hours,
               start_date::text AS start_date, due_date::text AS due_date,
               project_root_id, created_by, created_at::text AS created_at, updated_at::text AS updated_at
        FROM "task" WHERE id = $1 LIMIT 1
      )sql",
        taskId);
    if (finalRes.empty()) {
      auto resp = HttpResponse::newHttpJsonResponse(
          Json::Value("Task created but cannot fetch it"));
      resp->setStatusCode(k500InternalServerError);
      return callback(resp);
    }
    drogon_model::project_calendar::CalendarTask created(finalRes[0], -1);
    auto out = created.toJson();
    auto resp = HttpResponse::newHttpJsonResponse(out);
    resp->setStatusCode(k201Created);
    return callback(resp);

  } catch (const std::exception& e) {
    LOG_ERROR << "createTask failed: " << e.what();
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

void TaskController::getTasks(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback) {
  auto attrsPtr = req->attributes();
  if (!attrsPtr || !attrsPtr->find("user_id")) {
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Unauthorized"));
    resp->setStatusCode(k401Unauthorized);
    return callback(resp);
  }
  const std::string userId = attrsPtr->get<std::string>("user_id");
  if (userId.empty()) {
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Unauthorized"));
    resp->setStatusCode(k401Unauthorized);
    return callback(resp);
  }

  const std::string parentParam = req->getParameter("parent_task_id");
  const std::string statusParam = req->getParameter("status");
  const std::string priorityParam = req->getParameter("priority");
  int limit = 100;
  int offset = 0;
  const std::string limitParam = req->getParameter("limit");
  const std::string offsetParam = req->getParameter("offset");
  if (!limitParam.empty()) {
    try {
      limit = std::clamp(std::stoi(limitParam), 1, 2000);
    } catch (...) {
      limit = 100;
    }
  }
  if (!offsetParam.empty()) {
    try {
      offset = std::max(0, std::stoi(offsetParam));
    } catch (...) {
      offset = 0;
    }
  }

  std::string sql = R"sql(
    SELECT t.id AS id,
           t.parent_task_id AS parent_task_id,
           t.title AS title,
           t.description AS description,
           t.priority AS priority,
           t.status AS status,
           t.estimated_hours AS estimated_hours,
           t.start_date::text AS start_date,
           t.due_date::text AS due_date,
           t.project_root_id AS project_root_id,
           t.created_by AS created_by,
           t.created_at::text AS created_at,
           t.updated_at::text AS updated_at,
           ta.assigned_hours AS assigned_hours,
           tr.role AS role
    FROM "task" t
    JOIN "task_assignment" ta ON ta.task_id = t.id
    LEFT JOIN "task_role_assignment" tr ON tr.task_id = t.id AND tr.user_id = ta.user_id
    WHERE ta.user_id = $1
      AND ($2 = '' OR ($2 = 'null' AND t.parent_task_id IS NULL) OR t.parent_task_id = $2)
      AND ($3 = '' OR t.status = $3)
      AND ($4 = '' OR t.priority = $4)
    ORDER BY t.created_at DESC
    LIMIT $5 OFFSET $6
  )sql";

  auto dbClient = app().getDbClient();
  try {
    auto tasksRes = dbClient->execSqlSync(sql, userId, parentParam, statusParam,
                                          priorityParam, limit, offset);

    Json::Value out(Json::arrayValue);
    for (const auto& row : tasksRes) {
      Json::Value item(Json::objectValue);
      item["id"] = row["id"].as<std::string>();
      if (!row["parent_task_id"].isNull())
        item["parent_task_id"] = row["parent_task_id"].as<std::string>();
      else
        item["parent_task_id"] = Json::Value();
      item["title"] = row["title"].isNull()
                          ? Json::Value()
                          : Json::Value(row["title"].as<std::string>());
      item["description"] =
          row["description"].isNull()
              ? Json::Value()
              : Json::Value(row["description"].as<std::string>());
      item["priority"] = row["priority"].isNull()
                             ? Json::Value()
                             : Json::Value(row["priority"].as<std::string>());
      item["status"] = row["status"].isNull()
                           ? Json::Value()
                           : Json::Value(row["status"].as<std::string>());
      item["estimated_hours"] =
          row["estimated_hours"].isNull()
              ? Json::Value()
              : Json::Value(row["estimated_hours"].as<std::string>());
      item["start_date"] =
          row["start_date"].isNull()
              ? Json::Value()
              : Json::Value(row["start_date"].as<std::string>());
      item["due_date"] = row["due_date"].isNull()
                             ? Json::Value()
                             : Json::Value(row["due_date"].as<std::string>());
      item["project_root_id"] =
          row["project_root_id"].isNull()
              ? Json::Value()
              : Json::Value(row["project_root_id"].as<std::string>());
      item["created_by"] =
          row["created_by"].isNull()
              ? Json::Value()
              : Json::Value(row["created_by"].as<std::string>());
      item["created_at"] =
          row["created_at"].isNull()
              ? Json::Value()
              : Json::Value(row["created_at"].as<std::string>());
      item["updated_at"] =
          row["updated_at"].isNull()
              ? Json::Value()
              : Json::Value(row["updated_at"].as<std::string>());

      item["assigned_hours"] =
          row["assigned_hours"].isNull()
              ? Json::Value()
              : Json::Value(row["assigned_hours"].as<std::string>());
      item["role"] = row["role"].isNull()
                         ? Json::Value()
                         : Json::Value(row["role"].as<std::string>());

      auto schedules = dbClient->execSqlSync(
          R"sql(
          SELECT ts.id::text AS id,
                 ts.task_id::text AS task_id,
                 ts.start_ts::date::text AS date,
                 ts.start_ts::time::text AS start_time,
                 ts.end_ts::time::text AS end_time,
                 ts.hours
          FROM "task_schedule" ts
          WHERE ts.task_id = $1
          ORDER BY ts.start_ts
        )sql",
          item["id"].asString());
      Json::Value scheduleArr(Json::arrayValue);
      for (const auto& srow : schedules) {
        Json::Value s(Json::objectValue);
        s["id"] = srow["id"].isNull()
                      ? Json::Value()
                      : Json::Value(srow["id"].as<std::string>());
        s["date"] = srow["date"].isNull()
                        ? Json::Value()
                        : Json::Value(srow["date"].as<std::string>());
        s["start_time"] =
            srow["start_time"].isNull()
                ? Json::Value()
                : Json::Value(srow["start_time"].as<std::string>());
        s["end_time"] = srow["end_time"].isNull()
                            ? Json::Value()
                            : Json::Value(srow["end_time"].as<std::string>());
        s["hours"] = srow["hours"].isNull()
                         ? Json::Value()
                         : Json::Value(srow["hours"].as<std::string>());
        scheduleArr.append(s);
      }
      item["schedule"] = scheduleArr;

      out.append(item);
    }

    auto resp = HttpResponse::newHttpJsonResponse(out);
    resp->setStatusCode(k200OK);
    return callback(resp);

  } catch (const std::exception& e) {
    LOG_ERROR << "getTasks failed for user " << userId << ": " << e.what();
    auto resp =
        HttpResponse::newHttpJsonResponse(Json::Value("Internal server error"));
    resp->setStatusCode(k500InternalServerError);
    return callback(resp);
  }
}

void TaskController::updateTask(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback) {
  auto jsonPtr = req->getJsonObject();
  if (!jsonPtr || !jsonPtr->isObject()) {
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Invalid JSON"));
    resp->setStatusCode(k400BadRequest);
    return callback(resp);
  }
  const Json::Value& j = *jsonPtr;

  std::string taskId = getPathVariableCompat(req, "id");
  if (taskId.empty()) {
    if (j.isMember("id") && j["id"].isString()) taskId = j["id"].asString();
  }
  if (taskId.empty()) {
    auto resp =
        HttpResponse::newHttpJsonResponse(Json::Value("Missing task id"));
    resp->setStatusCode(k400BadRequest);
    return callback(resp);
  }

  auto attrsPtr = req->attributes();
  if (!attrsPtr || !attrsPtr->find("user_id")) {
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Unauthorized"));
    resp->setStatusCode(k401Unauthorized);
    return callback(resp);
  }
  const std::string userId = attrsPtr->get<std::string>("user_id");
  if (userId.empty()) {
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Unauthorized"));
    resp->setStatusCode(k401Unauthorized);
    return callback(resp);
  }

  auto dbClient = app().getDbClient();
  try {
    auto exists = dbClient->execSqlSync(
        "SELECT id FROM \"task\" WHERE id = $1 LIMIT 1", taskId);
    if (exists.empty()) {
      auto resp =
          HttpResponse::newHttpJsonResponse(Json::Value("Task not found"));
      resp->setStatusCode(k404NotFound);
      return callback(resp);
    }
    if (!hasOwnerPermission(dbClient, taskId, userId)) {
      auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Forbidden"));
      resp->setStatusCode(k403Forbidden);
      return callback(resp);
    }

    auto res = dbClient->execSqlSync(
        R"sql(
        SELECT id, parent_task_id, title, description, priority, status, estimated_hours,
               start_date::text AS start_date, due_date::text AS due_date,
               project_root_id, created_by, created_at::text AS created_at, updated_at::text AS updated_at
        FROM "task" WHERE id = $1 LIMIT 1
      )sql",
        taskId);
    if (res.empty()) {
      auto resp =
          HttpResponse::newHttpJsonResponse(Json::Value("Task not found"));
      resp->setStatusCode(k404NotFound);
      return callback(resp);
    }
    drogon_model::project_calendar::CalendarTask task(res[0], -1);

    try {
      task.updateByJson(j);
    } catch (...) {
    }
    task.setUpdatedAt(::trantor::Date::now());

    drogon::orm::Mapper<drogon_model::project_calendar::CalendarTask>
        taskMapper(dbClient);
    task.setId(taskId);
    taskMapper.update(task);

    auto finalRes = dbClient->execSqlSync(
        R"sql(
        SELECT id, parent_task_id, title, description, priority, status, estimated_hours,
               start_date::text AS start_date, due_date::text AS due_date,
               project_root_id, created_by, created_at::text AS created_at, updated_at::text AS updated_at
        FROM "task" WHERE id = $1 LIMIT 1
      )sql",
        taskId);
    if (finalRes.empty()) {
      auto resp = HttpResponse::newHttpJsonResponse(
          Json::Value("Task updated but cannot fetch it"));
      resp->setStatusCode(k500InternalServerError);
      return callback(resp);
    }
    drogon_model::project_calendar::CalendarTask updated(finalRes[0], -1);
    auto out = updated.toJson();
    auto resp = HttpResponse::newHttpJsonResponse(out);
    resp->setStatusCode(k200OK);
    return callback(resp);

  } catch (const std::exception& e) {
    LOG_ERROR << "updateTask failed: " << e.what();
    auto resp =
        HttpResponse::newHttpJsonResponse(Json::Value("Internal server error"));
    resp->setStatusCode(k500InternalServerError);
    return callback(resp);
  }
}

void TaskController::deleteTask(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback) {
  std::string taskId = getPathVariableCompat(req, "id");
  if (taskId.empty()) {
    auto resp =
        HttpResponse::newHttpJsonResponse(Json::Value("Missing task id"));
    resp->setStatusCode(k400BadRequest);
    return callback(resp);
  }

  auto attrsPtr = req->attributes();
  if (!attrsPtr || !attrsPtr->find("user_id")) {
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Unauthorized"));
    resp->setStatusCode(k401Unauthorized);
    return callback(resp);
  }
  const std::string userId = attrsPtr->get<std::string>("user_id");
  if (userId.empty()) {
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Unauthorized"));
    resp->setStatusCode(k401Unauthorized);
    return callback(resp);
  }

  auto dbClient = app().getDbClient();
  try {
    auto exists = dbClient->execSqlSync(
        "SELECT id FROM \"task\" WHERE id = $1 LIMIT 1", taskId);
    if (exists.empty()) {
      auto resp =
          HttpResponse::newHttpJsonResponse(Json::Value("Task not found"));
      resp->setStatusCode(k404NotFound);
      return callback(resp);
    }
    if (!hasOwnerPermission(dbClient, taskId, userId)) {
      auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Forbidden"));
      resp->setStatusCode(k403Forbidden);
      return callback(resp);
    }

    dbClient->execSqlSync("BEGIN");
    dbClient->execSqlSync("DELETE FROM \"task_schedule\" WHERE task_id = $1",
                          taskId);
    dbClient->execSqlSync(
        "DELETE FROM \"task_role_assignment\" WHERE task_id = $1", taskId);
    dbClient->execSqlSync("DELETE FROM \"task_assignment\" WHERE task_id = $1",
                          taskId);
    dbClient->execSqlSync("DELETE FROM \"task\" WHERE id = $1", taskId);
    dbClient->execSqlSync("COMMIT");

    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Deleted"));
    resp->setStatusCode(k200OK);
    return callback(resp);
  } catch (const std::exception& e) {
    LOG_ERROR << "deleteTask failed: " << e.what();
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

void TaskController::getSubtasks(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback) {
  auto attrsPtr = req->attributes();
  if (!attrsPtr || !attrsPtr->find("user_id")) {
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Unauthorized"));
    resp->setStatusCode(k401Unauthorized);
    return callback(resp);
  }
  const std::string userId = attrsPtr->get<std::string>("user_id");
  if (userId.empty()) {
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Unauthorized"));
    resp->setStatusCode(k401Unauthorized);
    return callback(resp);
  }

  const std::string parentId = getPathVariableCompat(req, "id");
  if (parentId.empty()) {
    auto resp = HttpResponse::newHttpJsonResponse(
        Json::Value("Missing parent task id"));
    resp->setStatusCode(k400BadRequest);
    return callback(resp);
  }

  auto dbClient = app().getDbClient();
  try {
    auto res = dbClient->execSqlSync(
        R"sql(
        SELECT t.id, t.title, t.description, t.priority, t.status,
               t.start_date::text AS start_date, t.due_date::text AS due_date,
               ta.assigned_hours, tr.role
        FROM "task" t
        JOIN "task_assignment" ta ON ta.task_id = t.id
        LEFT JOIN "task_role_assignment" tr ON tr.task_id = t.id AND tr.user_id = ta.user_id
        WHERE ta.user_id = $1 AND t.parent_task_id = $2
        ORDER BY t.created_at DESC
      )sql",
        userId, parentId);

    Json::Value out(Json::arrayValue);
    for (const auto& row : res) {
      Json::Value it(Json::objectValue);
      it["id"] = row["id"].as<std::string>();
      it["title"] = row["title"].isNull()
                        ? Json::Value()
                        : Json::Value(row["title"].as<std::string>());
      it["description"] =
          row["description"].isNull()
              ? Json::Value()
              : Json::Value(row["description"].as<std::string>());
      it["priority"] = row["priority"].isNull()
                           ? Json::Value()
                           : Json::Value(row["priority"].as<std::string>());
      it["status"] = row["status"].isNull()
                         ? Json::Value()
                         : Json::Value(row["status"].as<std::string>());
      it["start_date"] = row["start_date"].isNull()
                             ? Json::Value()
                             : Json::Value(row["start_date"].as<std::string>());
      it["due_date"] = row["due_date"].isNull()
                           ? Json::Value()
                           : Json::Value(row["due_date"].as<std::string>());
      it["assigned_hours"] =
          row["assigned_hours"].isNull()
              ? Json::Value()
              : Json::Value(row["assigned_hours"].as<std::string>());
      it["role"] = row["role"].isNull()
                       ? Json::Value()
                       : Json::Value(row["role"].as<std::string>());
      out.append(it);
    }

    auto resp = HttpResponse::newHttpJsonResponse(out);
    resp->setStatusCode(k200OK);
    return callback(resp);
  } catch (const std::exception& e) {
    LOG_ERROR << "getSubtasks failed: " << e.what();
    auto resp =
        HttpResponse::newHttpJsonResponse(Json::Value("Internal server error"));
    resp->setStatusCode(k500InternalServerError);
    return callback(resp);
  }
}

void TaskController::createAssignment(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback) {
  auto jsonPtr = req->getJsonObject();
  if (!jsonPtr || !jsonPtr->isObject()) {
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Invalid JSON"));
    resp->setStatusCode(k400BadRequest);
    return callback(resp);
  }
  const Json::Value& j = *jsonPtr;

  if (!j.isMember("task_id") || !j["task_id"].isString() ||
      !j.isMember("user_id") || !j["user_id"].isString()) {
    auto resp = HttpResponse::newHttpJsonResponse(
        Json::Value("Missing task_id or user_id"));
    resp->setStatusCode(k400BadRequest);
    return callback(resp);
  }
  const std::string taskId = j["task_id"].asString();
  const std::string assUserId = j["user_id"].asString();
  std::string role = "contributor";
  if (j.isMember("role") && j["role"].isString()) role = j["role"].asString();

  std::optional<std::string> assignedHours;
  if (j.isMember("assigned_hours") && j["assigned_hours"].isString())
    assignedHours = j["assigned_hours"].asString();

  auto attrsPtr = req->attributes();
  if (!attrsPtr || !attrsPtr->find("user_id")) {
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Unauthorized"));
    resp->setStatusCode(k401Unauthorized);
    return callback(resp);
  }
  const std::string requester = attrsPtr->get<std::string>("user_id");

  auto dbClient = app().getDbClient();
  try {
    if (!hasOwnerPermission(dbClient, taskId, requester)) {
      auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Forbidden"));
      resp->setStatusCode(k403Forbidden);
      return callback(resp);
    }

    auto t = dbClient->execSqlSync(
        "SELECT id FROM \"task\" WHERE id = $1 LIMIT 1", taskId);
    if (t.empty()) {
      auto resp =
          HttpResponse::newHttpJsonResponse(Json::Value("Task not found"));
      resp->setStatusCode(k404NotFound);
      return callback(resp);
    }

    auto ex = dbClient->execSqlSync(
        "SELECT id FROM \"task_assignment\" WHERE task_id = $1 AND user_id = "
        "$2 LIMIT 1",
        taskId, assUserId);
    if (!ex.empty()) {
      auto resp = HttpResponse::newHttpJsonResponse(
          Json::Value("Assignment already exists"));
      resp->setStatusCode(k409Conflict);
      return callback(resp);
    }

    dbClient->execSqlSync("BEGIN");
    drogon_model::project_calendar::TaskAssignment ta;
    ta.setTaskId(taskId);
    ta.setUserId(assUserId);
    if (assignedHours) ta.setAssignedHours(*assignedHours);
    ta.setAssignedAt(::trantor::Date::now());
    drogon::orm::Mapper<drogon_model::project_calendar::TaskAssignment>
        taMapper(dbClient);
    taMapper.insert(ta);

    drogon_model::project_calendar::TaskRoleAssignment tra;
    tra.setTaskId(taskId);
    tra.setUserId(assUserId);
    tra.setRole(role);
    tra.setAssignedAt(::trantor::Date::now());
    drogon::orm::Mapper<drogon_model::project_calendar::TaskRoleAssignment>
        traMapper(dbClient);
    traMapper.insert(tra);

    dbClient->execSqlSync("COMMIT");

    Json::Value out(Json::objectValue);
    out["task_id"] = taskId;
    out["user_id"] = assUserId;
    out["role"] = role;
    out["assigned_hours"] =
        assignedHours ? Json::Value(*assignedHours) : Json::Value();
    out["assigned_at"] = ::trantor::Date::now().toDbStringLocal();

    auto resp = HttpResponse::newHttpJsonResponse(out);
    resp->setStatusCode(k201Created);
    return callback(resp);

  } catch (const std::exception& e) {
    LOG_ERROR << "createAssignment failed: " << e.what();
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

void TaskController::listAssignments(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback) {
  std::string taskId = getPathVariableCompat(req, "task_id");
  if (taskId.empty()) {
    taskId = req->getParameter("task_id");
  }
  if (taskId.empty()) {
    auto resp =
        HttpResponse::newHttpJsonResponse(Json::Value("Missing task_id"));
    resp->setStatusCode(k400BadRequest);
    return callback(resp);
  }

  auto attrsPtr = req->attributes();
  if (!attrsPtr || !attrsPtr->find("user_id")) {
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Unauthorized"));
    resp->setStatusCode(k401Unauthorized);
    return callback(resp);
  }
  const std::string requester = attrsPtr->get<std::string>("user_id");

  auto dbClient = app().getDbClient();
  try {
    auto check = dbClient->execSqlSync(
        "SELECT 1 FROM \"task_assignment\" WHERE task_id = $1 AND user_id = $2 "
        "LIMIT 1",
        taskId, requester);
    auto created = dbClient->execSqlSync(
        "SELECT 1 FROM \"task\" WHERE id = $1 AND created_by = $2 LIMIT 1",
        taskId, requester);
    if (check.empty() && created.empty()) {
      auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Forbidden"));
      resp->setStatusCode(k403Forbidden);
      return callback(resp);
    }

    auto res = dbClient->execSqlSync(
        R"sql(
        SELECT ta.user_id::text AS user_id,
               ta.assigned_hours AS assigned_hours,
               ta.assigned_at::text AS assigned_at,
               tr.role AS role,
               tr.assigned_at::text AS role_assigned_at
        FROM "task_assignment" ta
        LEFT JOIN "task_role_assignment" tr ON tr.task_id = ta.task_id AND tr.user_id = ta.user_id
        WHERE ta.task_id = $1
        ORDER BY ta.assigned_at DESC
      )sql",
        taskId);

    Json::Value out(Json::arrayValue);
    for (const auto& row : res) {
      Json::Value it(Json::objectValue);
      it["user_id"] = row["user_id"].isNull()
                          ? Json::Value()
                          : Json::Value(row["user_id"].as<std::string>());
      it["assigned_hours"] =
          row["assigned_hours"].isNull()
              ? Json::Value()
              : Json::Value(row["assigned_hours"].as<std::string>());
      it["assigned_at"] =
          row["assigned_at"].isNull()
              ? Json::Value()
              : Json::Value(row["assigned_at"].as<std::string>());
      it["role"] = row["role"].isNull()
                       ? Json::Value()
                       : Json::Value(row["role"].as<std::string>());
      it["role_assigned_at"] =
          row["role_assigned_at"].isNull()
              ? Json::Value()
              : Json::Value(row["role_assigned_at"].as<std::string>());
      out.append(it);
    }

    auto resp = HttpResponse::newHttpJsonResponse(out);
    resp->setStatusCode(k200OK);
    return callback(resp);

  } catch (const std::exception& e) {
    LOG_ERROR << "listAssignments failed: " << e.what();
    auto resp =
        HttpResponse::newHttpJsonResponse(Json::Value("Internal server error"));
    resp->setStatusCode(k500InternalServerError);
    return callback(resp);
  }
}

void TaskController::deleteAssignment(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback) {
  std::string taskId = getPathVariableCompat(req, "task_id");
  if (taskId.empty()) taskId = req->getParameter("task_id");
  std::string assUserId = getPathVariableCompat(req, "user_id");
  if (assUserId.empty()) assUserId = req->getParameter("user_id");

  if (taskId.empty() || assUserId.empty()) {
    auto resp = HttpResponse::newHttpJsonResponse(
        Json::Value("Missing task_id or user_id"));
    resp->setStatusCode(k400BadRequest);
    return callback(resp);
  }

  auto attrsPtr = req->attributes();
  if (!attrsPtr || !attrsPtr->find("user_id")) {
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Unauthorized"));
    resp->setStatusCode(k401Unauthorized);
    return callback(resp);
  }
  const std::string requester = attrsPtr->get<std::string>("user_id");

  auto dbClient = app().getDbClient();
  try {
    if (!hasOwnerPermission(dbClient, taskId, requester)) {
      auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Forbidden"));
      resp->setStatusCode(k403Forbidden);
      return callback(resp);
    }

    dbClient->execSqlSync("BEGIN");
    dbClient->execSqlSync(
        "DELETE FROM \"task_role_assignment\" WHERE task_id = $1 AND user_id = "
        "$2",
        taskId, assUserId);
    dbClient->execSqlSync(
        "DELETE FROM \"task_assignment\" WHERE task_id = $1 AND user_id = $2",
        taskId, assUserId);
    dbClient->execSqlSync("COMMIT");

    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Deleted"));
    resp->setStatusCode(k200OK);
    return callback(resp);
  } catch (const std::exception& e) {
    LOG_ERROR << "deleteAssignment failed: " << e.what();
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