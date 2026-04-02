#include "API/TaskController.h"
#include "API/RbacHelpers.h"

#include <drogon/HttpResponse.h>
#include <drogon/drogon.h>
#include <drogon/orm/Mapper.h>
#include <drogon/orm/Result.h>
#include <json/json.h>
#include <trantor/utils/Logger.h>

#include <algorithm>
#include <exception>
#include <optional>
#include <string>
#include <vector>

#include "models/Task.h"
#include "models/TaskAssignment.h"
#include "models/TaskRoleAssignment.h"
#include "models/TaskSchedule.h"

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

static std::string extractTaskIdFromPath(const HttpRequestPtr& req,
                                         const std::string& segment) {
  std::string taskId = getPathVariableCompat(req, "task_id");
  if (taskId.empty() || taskId == segment) {
    std::string path = req->path();
    size_t tasksPos = path.find("/tasks/");
    if (tasksPos != std::string::npos) {
      size_t uuidStart = tasksPos + 7;
      size_t uuidEnd = path.find("/", uuidStart);
      if (uuidEnd != std::string::npos) {
        taskId = path.substr(uuidStart, uuidEnd - uuidStart);
      }
    }
  }
  return taskId;
}

static bool validateDatesAgainstProject(
    const std::shared_ptr<drogon::orm::DbClient>& dbClient,
    const std::string& projectRootId, const std::string& taskStartDate,
    const std::string& taskDueDate, std::string& errorMsg) {
  if (projectRootId.empty()) return true;
  try {
    auto projectRes = dbClient->execSqlSync(
        "SELECT start_date::text AS start_date, due_date::text AS due_date "
        "FROM \"task\" WHERE id = $1 LIMIT 1",
        projectRootId);
    if (projectRes.empty()) return true;
    std::string projStart = projectRes[0]["start_date"].isNull()
                                ? ""
                                : projectRes[0]["start_date"].as<std::string>();
    std::string projDue = projectRes[0]["due_date"].isNull()
                              ? ""
                              : projectRes[0]["due_date"].as<std::string>();
    if (!taskStartDate.empty() && !projStart.empty() &&
        taskStartDate < projStart) {
      errorMsg = "Task start_date cannot be before project start_date";
      return false;
    }
    if (!taskDueDate.empty() && !projDue.empty() && taskDueDate > projDue) {
      errorMsg = "Task due_date cannot be after project due_date";
      return false;
    }
  } catch (...) {
  }
  return true;
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

static bool isAutoPromotableStatus(const std::optional<std::string>& status) {
  if (!status.has_value()) return true;
  return *status == "open" || *status == "pending";
}

static bool hasAnyDate(const std::string& startDate,
                       const std::string& dueDate) {
  return !startDate.empty() || !dueDate.empty();
}

static bool isValidAssigneeField(const Json::Value& j,
                                 std::optional<std::string>& assignee,
                                 std::string& error) {
  if (!j.isMember("assignee_user_id") || j["assignee_user_id"].isNull()) {
    assignee.reset();
    return true;
  }
  if (!j["assignee_user_id"].isString() ||
      j["assignee_user_id"].asString().empty()) {
    error = "assignee_user_id must be a non-empty string";
    return false;
  }
  assignee = j["assignee_user_id"].asString();
  return true;
}

struct AvailabilityStats {
  double capacityHours{0.0};
  double busyHours{0.0};
  double availableHours{0.0};
};

static bool computeAvailabilityStats(
    const std::shared_ptr<drogon::orm::DbClient>& dbClient,
    const std::string& userId, const std::string& startTs,
    const std::string& endTs, AvailabilityStats& stats, std::string& error) {
  try {
    auto capRes = dbClient->execSqlSync(
        R"sql(
        WITH days AS (
          SELECT gs::date AS day
          FROM generate_series($2::timestamptz::date, $3::timestamptz::date, interval '1 day') AS gs
        ),
        windows AS (
          SELECT
            GREATEST((d.day + ws.start_time)::timestamptz, $2::timestamptz) AS st,
            LEAST((d.day + ws.end_time)::timestamptz, $3::timestamptz) AS et
          FROM days d
          JOIN user_work_schedule ws
            ON ws.user_id = $1::uuid
           AND ws.weekday = EXTRACT(ISODOW FROM d.day)::int
        )
        SELECT COALESCE(SUM(EXTRACT(EPOCH FROM (et - st)) / 3600.0), 0.0) AS capacity_hours
        FROM windows
        WHERE et > st
      )sql",
        userId, startTs, endTs);

    auto busyRes = dbClient->execSqlSync(
        R"sql(
        SELECT COALESCE(
                 SUM(EXTRACT(EPOCH FROM (LEAST(end_ts, $3::timestamptz) -
                                         GREATEST(start_ts, $2::timestamptz))) / 3600.0),
                 0.0
               ) AS busy_hours
        FROM task_schedule
        WHERE user_id = $1::uuid
          AND start_ts < $3::timestamptz
          AND end_ts > $2::timestamptz
      )sql",
        userId, startTs, endTs);

    stats.capacityHours = capRes.empty() || capRes[0]["capacity_hours"].isNull()
                              ? 0.0
                              : capRes[0]["capacity_hours"].as<double>();
    stats.busyHours = busyRes.empty() || busyRes[0]["busy_hours"].isNull()
                          ? 0.0
                          : busyRes[0]["busy_hours"].as<double>();
    stats.availableHours = std::max(0.0, stats.capacityHours - stats.busyHours);
    return true;
  } catch (const std::exception& e) {
    error = e.what();
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

  if (!j.isMember("description") || j["description"].isNull() ||
      !j["description"].isString() || j["description"].asString().empty()) {
    auto resp = HttpResponse::newHttpJsonResponse(
        Json::Value("Missing or empty description"));
    resp->setStatusCode(k400BadRequest);
    return callback(resp);
  }

  if (j.isMember("estimated_hours") && !j["estimated_hours"].isNull()) {
    double hours = 0;
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
    if (hours < 0) {
      auto resp = HttpResponse::newHttpJsonResponse(
          Json::Value("estimated_hours must be >= 0"));
      resp->setStatusCode(k400BadRequest);
      return callback(resp);
    }
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

  std::optional<std::string> assigneeUserId;
  std::string assigneeError;
  if (!isValidAssigneeField(j, assigneeUserId, assigneeError)) {
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value(assigneeError));
    resp->setStatusCode(k400BadRequest);
    return callback(resp);
  }

  const std::string payloadStartDate =
      (j.isMember("start_date") && j["start_date"].isString())
          ? j["start_date"].asString()
          : "";
  const std::string payloadDueDate =
      (j.isMember("due_date") && j["due_date"].isString())
          ? j["due_date"].asString()
          : "";
  const bool payloadHasAnyDate = hasAnyDate(payloadStartDate, payloadDueDate);
  if (payloadHasAnyDate && !assigneeUserId.has_value()) {
    auto resp = HttpResponse::newHttpJsonResponse(
        Json::Value("assignee_user_id is required when task has dates"));
    resp->setStatusCode(k400BadRequest);
    return callback(resp);
  }

  std::optional<std::string> payloadStatus =
      (j.isMember("status") && j["status"].isString())
          ? std::make_optional(j["status"].asString())
          : std::nullopt;
  const bool statusProvided = j.isMember("status");
  const bool shouldAutoPromoteToInProgress =
      payloadHasAnyDate &&
      (!statusProvided || (statusProvided && j["status"].isNull()) ||
       (statusProvided && j["status"].isString() &&
        isAutoPromotableStatus(payloadStatus)));

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

    // Validate task dates against project dates
    {
      std::string projectRootId =
          (j.isMember("project_root_id") && j["project_root_id"].isString())
              ? j["project_root_id"].asString()
              : "";
      std::string dateError;
      if (!validateDatesAgainstProject(dbClient, projectRootId,
                                       payloadStartDate, payloadDueDate,
                                       dateError)) {
        dbClient->execSqlSync("ROLLBACK");
        auto resp = HttpResponse::newHttpJsonResponse(Json::Value(dateError));
        resp->setStatusCode(k400BadRequest);
        return callback(resp);
      }
    }

    drogon_model::project_calendar::Task task;
    try {
      task.updateByJson(j);
    } catch (...) {
    }
    if (parentId)
      task.setParentTaskId(*parentId);
    else
      task.setParentTaskIdToNull();

    if (shouldAutoPromoteToInProgress) {
      task.setStatus(std::string("in_progress"));
    }

    task.setCreatedBy(userId);
    task.setCreatedAt(::trantor::Date::now());
    task.setUpdatedAt(::trantor::Date::now());

    drogon::orm::Mapper<drogon_model::project_calendar::Task> taskMapper(
        dbClient);
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

    if (assigneeUserId.has_value() && *assigneeUserId != userId) {
      auto existingAssignee = dbClient->execSqlSync(
          "SELECT 1 FROM \"task_assignment\" WHERE task_id = $1 AND user_id "
          "= $2 LIMIT 1",
          taskId, *assigneeUserId);
      if (existingAssignee.empty()) {
        drogon_model::project_calendar::TaskAssignment assigneeTa;
        assigneeTa.setTaskId(taskId);
        assigneeTa.setUserId(*assigneeUserId);
        assigneeTa.setAssignedAt(::trantor::Date::now());
        taMapper.insert(assigneeTa);

        drogon_model::project_calendar::TaskRoleAssignment assigneeTra;
        assigneeTra.setTaskId(taskId);
        assigneeTra.setUserId(*assigneeUserId);
        assigneeTra.setRole(std::string("executor"));
        assigneeTra.setAssignedAt(::trantor::Date::now());
        traMapper.insert(assigneeTra);
      }
    }

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
    drogon_model::project_calendar::Task created(finalRes[0], -1);
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
  const std::string hasTimeParam = req->getParameter("has_time");
  int64_t limit = 100;
  int64_t offset = 0;
  const std::string limitParam = req->getParameter("limit");
  const std::string offsetParam = req->getParameter("offset");
  if (!limitParam.empty()) {
    try {
      limit = std::clamp(static_cast<int64_t>(std::stoll(limitParam)),
                         int64_t(1), int64_t(2000));
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

  // Build SQL with LIMIT/OFFSET embedded (since Drogon has issues with int
  // parameters)
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
    WHERE ta.user_id = $1::uuid
      AND (CASE WHEN $2 = '' THEN TRUE WHEN $2 = 'null' THEN t.parent_task_id IS NULL ELSE t.parent_task_id = $2::uuid END)
      AND ($3 = '' OR t.status::text = $3)
      AND ($4 = '' OR t.priority::text = $4)
      AND ($5 = '' OR ($5 = 'true' AND (t.start_date IS NOT NULL OR t.due_date IS NOT NULL)) OR ($5 = 'false' AND t.start_date IS NULL AND t.due_date IS NULL))
    ORDER BY t.created_at DESC
    LIMIT )sql" + std::to_string(limit) +
                    " OFFSET " + std::to_string(offset);

  auto dbClient = app().getDbClient();
  try {
    auto tasksRes = dbClient->execSqlSync(sql, userId, parentParam, statusParam,
                                          priorityParam, hasTimeParam);

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
      item["has_time"] =
          !row["start_date"].isNull() || !row["due_date"].isNull();
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

void TaskController::calculateAvailability(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback) {
  auto attrsPtr = req->attributes();
  if (!attrsPtr || !attrsPtr->find("user_id")) {
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Unauthorized"));
    resp->setStatusCode(k401Unauthorized);
    return callback(resp);
  }
  const std::string requesterUserId = attrsPtr->get<std::string>("user_id");
  if (requesterUserId.empty()) {
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
  if (!j.isMember("user_id") || !j["user_id"].isString() ||
      j["user_id"].asString().empty() || !j.isMember("start_ts") ||
      !j["start_ts"].isString() || j["start_ts"].asString().empty() ||
      !j.isMember("end_ts") || !j["end_ts"].isString() ||
      j["end_ts"].asString().empty()) {
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value(
        "user_id, start_ts and end_ts are required non-empty strings"));
    resp->setStatusCode(k400BadRequest);
    return callback(resp);
  }

  const std::string userId = j["user_id"].asString();
  const std::string startTs = j["start_ts"].asString();
  const std::string endTs = j["end_ts"].asString();

  auto dbClient = app().getDbClient();
  try {
    auto orderRes = dbClient->execSqlSync(
        "SELECT ($1::timestamptz < $2::timestamptz) AS ok", startTs, endTs);
    const bool ok = !orderRes.empty() && !orderRes[0]["ok"].isNull() &&
                    orderRes[0]["ok"].as<bool>();
    if (!ok) {
      auto resp = HttpResponse::newHttpJsonResponse(
          Json::Value("Invalid start_ts/end_ts format or ordering"));
      resp->setStatusCode(k400BadRequest);
      return callback(resp);
    }

    AvailabilityStats stats;
    std::string availabilityError;
    if (!computeAvailabilityStats(dbClient, userId, startTs, endTs, stats,
                                  availabilityError)) {
      LOG_ERROR << "calculateAvailability failed: " << availabilityError;
      auto resp = HttpResponse::newHttpJsonResponse(
          Json::Value("Internal server error"));
      resp->setStatusCode(k500InternalServerError);
      return callback(resp);
    }

    Json::Value out(Json::objectValue);
    out["user_id"] = userId;
    out["start_ts"] = startTs;
    out["end_ts"] = endTs;
    out["capacity_hours"] = stats.capacityHours;
    out["busy_hours"] = stats.busyHours;
    out["available_hours"] = stats.availableHours;
    out["load_percent"] = (stats.capacityHours <= 0.0)
                              ? 0.0
                              : (stats.busyHours / stats.capacityHours) * 100.0;

    auto resp = HttpResponse::newHttpJsonResponse(out);
    resp->setStatusCode(k200OK);
    return callback(resp);
  } catch (const std::exception& e) {
    LOG_ERROR << "calculateAvailability failed for user " << userId << ": "
              << e.what();
    auto resp = HttpResponse::newHttpJsonResponse(
        Json::Value("Invalid start_ts/end_ts format or ordering"));
    resp->setStatusCode(k400BadRequest);
    return callback(resp);
  }
}

void TaskController::getCandidateAssignees(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback) {
  auto attrsPtr = req->attributes();
  if (!attrsPtr || !attrsPtr->find("user_id")) {
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Unauthorized"));
    resp->setStatusCode(k401Unauthorized);
    return callback(resp);
  }
  const std::string requesterUserId = attrsPtr->get<std::string>("user_id");
  if (requesterUserId.empty()) {
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Unauthorized"));
    resp->setStatusCode(k401Unauthorized);
    return callback(resp);
  }

  std::string taskId = extractTaskIdFromPath(req, "candidate-assignees");
  if (taskId.empty()) {
    auto resp =
        HttpResponse::newHttpJsonResponse(Json::Value("Missing task id"));
    resp->setStatusCode(k400BadRequest);
    return callback(resp);
  }

  auto dbClient = app().getDbClient();
  try {
    auto exists = dbClient->execSqlSync(
        "SELECT start_date::text AS start_date, due_date::text AS due_date "
        "FROM \"task\" WHERE id = $1 LIMIT 1",
        taskId);
    if (exists.empty()) {
      auto resp =
          HttpResponse::newHttpJsonResponse(Json::Value("Task not found"));
      resp->setStatusCode(k404NotFound);
      return callback(resp);
    }

    if (!hasOwnerPermission(dbClient, taskId, requesterUserId)) {
      auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Forbidden"));
      resp->setStatusCode(k403Forbidden);
      return callback(resp);
    }

    auto jsonPtr = req->getJsonObject();
    if (!jsonPtr || !jsonPtr->isObject()) {
      auto resp =
          HttpResponse::newHttpJsonResponse(Json::Value("Invalid JSON"));
      resp->setStatusCode(k400BadRequest);
      return callback(resp);
    }
    const Json::Value& j = *jsonPtr;

    std::vector<std::string> requiredSkills;
    if (j.isMember("required_skills") && !j["required_skills"].isNull()) {
      if (!j["required_skills"].isArray()) {
        auto resp = HttpResponse::newHttpJsonResponse(
            Json::Value("required_skills must be an array of strings"));
        resp->setStatusCode(k400BadRequest);
        return callback(resp);
      }
      for (const auto& skill : j["required_skills"]) {
        if (!skill.isString()) {
          auto resp = HttpResponse::newHttpJsonResponse(
              Json::Value("required_skills must be an array of strings"));
          resp->setStatusCode(k400BadRequest);
          return callback(resp);
        }
        if (!skill.asString().empty()) {
          requiredSkills.push_back(skill.asString());
        }
      }
    }

    std::string startTs;
    std::string endTs;

    if (j.isMember("start_ts") && !j["start_ts"].isNull()) {
      if (!j["start_ts"].isString() || j["start_ts"].asString().empty()) {
        auto resp = HttpResponse::newHttpJsonResponse(
            Json::Value("start_ts must be a non-empty string"));
        resp->setStatusCode(k400BadRequest);
        return callback(resp);
      }
      startTs = j["start_ts"].asString();
    }
    if (j.isMember("end_ts") && !j["end_ts"].isNull()) {
      if (!j["end_ts"].isString() || j["end_ts"].asString().empty()) {
        auto resp = HttpResponse::newHttpJsonResponse(
            Json::Value("end_ts must be a non-empty string"));
        resp->setStatusCode(k400BadRequest);
        return callback(resp);
      }
      endTs = j["end_ts"].asString();
    }

    if (startTs.empty() || endTs.empty()) {
      std::string taskStartDate =
          exists[0]["start_date"].isNull()
              ? ""
              : exists[0]["start_date"].as<std::string>();
      std::string taskDueDate = exists[0]["due_date"].isNull()
                                    ? ""
                                    : exists[0]["due_date"].as<std::string>();

      if (taskStartDate.empty() || taskDueDate.empty()) {
        auto resp = HttpResponse::newHttpJsonResponse(Json::Value(
            "start_ts and end_ts are required when task has no dates"));
        resp->setStatusCode(k400BadRequest);
        return callback(resp);
      }

      startTs = taskStartDate + "T00:00:00Z";
      endTs = taskDueDate + "T23:59:59Z";
    }

    auto orderRes = dbClient->execSqlSync(
        "SELECT ($1::timestamptz < $2::timestamptz) AS ok", startTs, endTs);
    const bool ok = !orderRes.empty() && !orderRes[0]["ok"].isNull() &&
                    orderRes[0]["ok"].as<bool>();
    if (!ok) {
      auto resp = HttpResponse::newHttpJsonResponse(
          Json::Value("Invalid start_ts/end_ts format or ordering"));
      resp->setStatusCode(k400BadRequest);
      return callback(resp);
    }

    std::string requiredSkillsCsv;
    for (size_t i = 0; i < requiredSkills.size(); ++i) {
      if (i > 0) requiredSkillsCsv += ",";
      requiredSkillsCsv += requiredSkills[i];
    }

    auto usersRes = dbClient->execSqlSync(
        "SELECT id::text AS user_id, display_name, email "
        "FROM app_user ORDER BY display_name NULLS LAST, email");

    std::vector<std::pair<double, Json::Value>> rankedCandidates;
    rankedCandidates.reserve(usersRes.size());

    for (const auto& row : usersRes) {
      const std::string candidateUserId = row["user_id"].as<std::string>();

      AvailabilityStats stats;
      std::string availabilityError;
      if (!computeAvailabilityStats(dbClient, candidateUserId, startTs, endTs,
                                    stats, availabilityError)) {
        LOG_ERROR << "getCandidateAssignees availability failed: "
                  << availabilityError;
        auto resp = HttpResponse::newHttpJsonResponse(
            Json::Value("Internal server error"));
        resp->setStatusCode(k500InternalServerError);
        return callback(resp);
      }

      auto skillRes = dbClient->execSqlSync(
          R"sql(
          WITH req AS (
            SELECT trim(value) AS skill
            FROM unnest(string_to_array($2, ',')) AS value
            WHERE trim(value) <> ''
          )
          SELECT COALESCE(
                   SUM(
                     CASE
                       WHEN us.proficiency >= 3 THEN 1.0 + (us.proficiency - 3) * 0.25
                       ELSE 0.5
                     END
                   ),
                   0.0
                 ) AS skill_score,
                 COUNT(us.skill_key) AS matched_skills
          FROM req
          LEFT JOIN user_skill us
            ON us.user_id = $1::uuid
           AND lower(us.skill_key) = lower(req.skill)
        )sql",
          candidateUserId, requiredSkillsCsv);

      const double skillScore =
          skillRes.empty() || skillRes[0]["skill_score"].isNull()
              ? 0.0
              : skillRes[0]["skill_score"].as<double>();
      const int matchedSkills =
          skillRes.empty() || skillRes[0]["matched_skills"].isNull()
              ? 0
              : skillRes[0]["matched_skills"].as<int>();

      const double relevanceScore = skillScore * 100.0 + stats.availableHours;

      Json::Value candidate(Json::objectValue);
      candidate["user_id"] = candidateUserId;
      candidate["display_name"] =
          row["display_name"].isNull()
              ? Json::Value()
              : Json::Value(row["display_name"].as<std::string>());
      candidate["email"] = row["email"].isNull()
                               ? Json::Value()
                               : Json::Value(row["email"].as<std::string>());
      candidate["skill_score"] = skillScore;
      candidate["matched_skills"] = matchedSkills;
      candidate["capacity_hours"] = stats.capacityHours;
      candidate["busy_hours"] = stats.busyHours;
      candidate["available_hours"] = stats.availableHours;
      candidate["load_percent"] =
          (stats.capacityHours <= 0.0)
              ? 0.0
              : (stats.busyHours / stats.capacityHours) * 100.0;
      candidate["relevance_score"] = relevanceScore;

      rankedCandidates.emplace_back(relevanceScore, candidate);
    }

    std::sort(rankedCandidates.begin(), rankedCandidates.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });

    Json::Value candidates(Json::arrayValue);
    for (const auto& item : rankedCandidates) {
      candidates.append(item.second);
    }

    Json::Value out(Json::objectValue);
    out["task_id"] = taskId;
    out["start_ts"] = startTs;
    out["end_ts"] = endTs;
    out["candidates"] = candidates;

    auto resp = HttpResponse::newHttpJsonResponse(out);
    resp->setStatusCode(k200OK);
    return callback(resp);
  } catch (const std::exception& e) {
    LOG_ERROR << "getCandidateAssignees failed: " << e.what();
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

  std::optional<std::string> assigneeUserId;
  std::string assigneeError;
  if (!isValidAssigneeField(j, assigneeUserId, assigneeError)) {
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value(assigneeError));
    resp->setStatusCode(k400BadRequest);
    return callback(resp);
  }

  // Validate description: if provided, must not be empty
  if (j.isMember("description") && !j["description"].isNull()) {
    if (!j["description"].isString() || j["description"].asString().empty()) {
      auto resp = HttpResponse::newHttpJsonResponse(
          Json::Value("description must be a non-empty string"));
      resp->setStatusCode(k400BadRequest);
      return callback(resp);
    }
  }

  // Validate estimated_hours >= 0
  if (j.isMember("estimated_hours") && !j["estimated_hours"].isNull()) {
    double hours = 0;
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
    if (hours < 0) {
      auto resp = HttpResponse::newHttpJsonResponse(
          Json::Value("estimated_hours must be >= 0"));
      resp->setStatusCode(k400BadRequest);
      return callback(resp);
    }
  }

  std::string taskId = getPathVariableCompat(req, "task_id");
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

    // Validate parent_task_id if being changed
    if (j.isMember("parent_task_id") && !j["parent_task_id"].isNull()) {
      if (!j["parent_task_id"].isString()) {
        auto resp = HttpResponse::newHttpJsonResponse(
            Json::Value("Invalid parent_task_id"));
        resp->setStatusCode(k400BadRequest);
        return callback(resp);
      }
      std::string newParent = j["parent_task_id"].asString();
      if (newParent == taskId) {
        auto resp = HttpResponse::newHttpJsonResponse(
            Json::Value("Task cannot be its own parent"));
        resp->setStatusCode(k400BadRequest);
        return callback(resp);
      }
      auto parentRes = dbClient->execSqlSync(
          "SELECT id FROM \"task\" WHERE id = $1 LIMIT 1", newParent);
      if (parentRes.empty()) {
        auto resp = HttpResponse::newHttpJsonResponse(
            Json::Value("parent_task_id not found"));
        resp->setStatusCode(k400BadRequest);
        return callback(resp);
      }
    }

    // Validate dates against project
    std::string effectiveProjectRootId;
    if (j.isMember("project_root_id") && !j["project_root_id"].isNull() &&
        j["project_root_id"].isString()) {
      effectiveProjectRootId = j["project_root_id"].asString();
    } else if (!res[0]["project_root_id"].isNull()) {
      effectiveProjectRootId = res[0]["project_root_id"].as<std::string>();
    }
    std::string effectiveStartDate;
    if (j.isMember("start_date") && !j["start_date"].isNull() &&
        j["start_date"].isString()) {
      effectiveStartDate = j["start_date"].asString();
    } else if (!res[0]["start_date"].isNull()) {
      effectiveStartDate = res[0]["start_date"].as<std::string>();
    }
    std::string effectiveDueDate;
    if (j.isMember("due_date") && !j["due_date"].isNull() &&
        j["due_date"].isString()) {
      effectiveDueDate = j["due_date"].asString();
    } else if (!res[0]["due_date"].isNull()) {
      effectiveDueDate = res[0]["due_date"].as<std::string>();
    }
    std::string dateError;
    if (!validateDatesAgainstProject(dbClient, effectiveProjectRootId,
                                     effectiveStartDate, effectiveDueDate,
                                     dateError)) {
      auto resp = HttpResponse::newHttpJsonResponse(Json::Value(dateError));
      resp->setStatusCode(k400BadRequest);
      return callback(resp);
    }

    const bool hasEffectiveAnyDate =
        hasAnyDate(effectiveStartDate, effectiveDueDate);
    const bool assigneeProvidedInPayload =
        j.isMember("assignee_user_id") && !j["assignee_user_id"].isNull();
    if (hasEffectiveAnyDate && !assigneeProvidedInPayload) {
      auto assignmentRes = dbClient->execSqlSync(
          "SELECT 1 FROM \"task_assignment\" WHERE task_id = $1 LIMIT 1",
          taskId);
      if (assignmentRes.empty()) {
        auto resp = HttpResponse::newHttpJsonResponse(
            Json::Value("assignee_user_id is required when task has dates"));
        resp->setStatusCode(k400BadRequest);
        return callback(resp);
      }
    }

    drogon_model::project_calendar::Task task(res[0], -1);

    try {
      task.updateByJson(j);
    } catch (...) {
    }

    // Explicitly handle nullable fields for full field editing
    if (j.isMember("parent_task_id")) {
      if (j["parent_task_id"].isNull())
        task.setParentTaskIdToNull();
      else if (j["parent_task_id"].isString())
        task.setParentTaskId(j["parent_task_id"].asString());
    }
    if (j.isMember("project_root_id")) {
      if (j["project_root_id"].isNull())
        task.setProjectRootIdToNull();
      else if (j["project_root_id"].isString())
        task.setProjectRootId(j["project_root_id"].asString());
    }
    if (j.isMember("description")) {
      if (j["description"].isNull())
        task.setDescriptionToNull();
      else if (j["description"].isString())
        task.setDescription(j["description"].asString());
    }
    if (j.isMember("priority")) {
      if (j["priority"].isNull())
        task.setPriorityToNull();
      else if (j["priority"].isString())
        task.setPriority(j["priority"].asString());
    }
    if (j.isMember("status")) {
      if (j["status"].isNull())
        task.setStatusToNull();
      else if (j["status"].isString())
        task.setStatus(j["status"].asString());
    }
    if (j.isMember("estimated_hours")) {
      if (j["estimated_hours"].isNull())
        task.setEstimatedHoursToNull();
      else if (j["estimated_hours"].isNumeric())
        task.setEstimatedHours(std::to_string(j["estimated_hours"].asDouble()));
      else if (j["estimated_hours"].isString())
        task.setEstimatedHours(j["estimated_hours"].asString());
    }
    if (j.isMember("start_date")) {
      if (j["start_date"].isNull()) task.setStartDateToNull();
    }
    if (j.isMember("due_date")) {
      if (j["due_date"].isNull()) task.setDueDateToNull();
    }

    if (hasEffectiveAnyDate) {
      const bool statusProvided = j.isMember("status");
      const bool statusNull = statusProvided && j["status"].isNull();
      const bool statusAutoPromotable =
          statusProvided && j["status"].isString() &&
          isAutoPromotableStatus(std::make_optional(j["status"].asString()));
      if (!statusProvided || statusNull || statusAutoPromotable) {
        task.setStatus(std::string("in_progress"));
      }
    }

    task.setUpdatedAt(::trantor::Date::now());

    drogon::orm::Mapper<drogon_model::project_calendar::Task> taskMapper(
        dbClient);
    task.setId(taskId);
    taskMapper.update(task);

    if (assigneeUserId.has_value()) {
      drogon::orm::Mapper<drogon_model::project_calendar::TaskAssignment>
          taMapper(dbClient);
      drogon::orm::Mapper<drogon_model::project_calendar::TaskRoleAssignment>
          traMapper(dbClient);

      auto assignmentExists = dbClient->execSqlSync(
          "SELECT 1 FROM \"task_assignment\" WHERE task_id = $1 AND user_id "
          "= $2 LIMIT 1",
          taskId, *assigneeUserId);
      if (assignmentExists.empty()) {
        drogon_model::project_calendar::TaskAssignment ta;
        ta.setTaskId(taskId);
        ta.setUserId(*assigneeUserId);
        ta.setAssignedAt(::trantor::Date::now());
        taMapper.insert(ta);
      }

      auto roleExists = dbClient->execSqlSync(
          "SELECT 1 FROM \"task_role_assignment\" WHERE task_id = $1 AND "
          "user_id = $2 AND role = $3 LIMIT 1",
          taskId, *assigneeUserId, std::string("executor"));
      if (roleExists.empty()) {
        drogon_model::project_calendar::TaskRoleAssignment tra;
        tra.setTaskId(taskId);
        tra.setUserId(*assigneeUserId);
        tra.setRole(std::string("executor"));
        tra.setAssignedAt(::trantor::Date::now());
        traMapper.insert(tra);
      }
    }

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
    drogon_model::project_calendar::Task updated(finalRes[0], -1);
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
  std::string taskId = getPathVariableCompat(req, "task_id");
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

  const std::string parentId = extractTaskIdFromPath(req, "subtasks");
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
        SELECT t.id, t.parent_task_id, t.title, t.description, t.priority, t.status,
               t.estimated_hours,
               t.start_date::text AS start_date, t.due_date::text AS due_date,
               t.project_root_id, t.created_by,
               t.created_at::text AS created_at, t.updated_at::text AS updated_at,
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
      it["parent_task_id"] =
          row["parent_task_id"].isNull()
              ? Json::Value()
              : Json::Value(row["parent_task_id"].as<std::string>());
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
      it["estimated_hours"] =
          row["estimated_hours"].isNull()
              ? Json::Value()
              : Json::Value(row["estimated_hours"].as<std::string>());
      it["start_date"] = row["start_date"].isNull()
                             ? Json::Value()
                             : Json::Value(row["start_date"].as<std::string>());
      it["due_date"] = row["due_date"].isNull()
                           ? Json::Value()
                           : Json::Value(row["due_date"].as<std::string>());
      it["project_root_id"] =
          row["project_root_id"].isNull()
              ? Json::Value()
              : Json::Value(row["project_root_id"].as<std::string>());
      it["created_by"] = row["created_by"].isNull()
                             ? Json::Value()
                             : Json::Value(row["created_by"].as<std::string>());
      it["created_at"] = row["created_at"].isNull()
                             ? Json::Value()
                             : Json::Value(row["created_at"].as<std::string>());
      it["updated_at"] = row["updated_at"].isNull()
                             ? Json::Value()
                             : Json::Value(row["updated_at"].as<std::string>());
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
  // Get task_id from URL path
  std::string taskId = getPathVariableCompat(req, "task_id");

  // Fallback: parse from path manually if routing fails
  if (taskId.empty() || taskId == "assignments") {
    std::string path = req->path();
    // Path format: /api/tasks/{uuid}/assignments
    size_t tasksPos = path.find("/tasks/");
    if (tasksPos != std::string::npos) {
      size_t uuidStart = tasksPos + 7;  // length of "/tasks/"
      size_t uuidEnd = path.find("/", uuidStart);
      if (uuidEnd != std::string::npos) {
        taskId = path.substr(uuidStart, uuidEnd - uuidStart);
      }
    }
  }

  if (taskId.empty()) {
    auto resp =
        HttpResponse::newHttpJsonResponse(Json::Value("Missing task_id"));
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

  // Get user from auth context
  auto attrsPtr = req->attributes();
  if (!attrsPtr || !attrsPtr->find("user_id")) {
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Unauthorized"));
    resp->setStatusCode(k401Unauthorized);
    return callback(resp);
  }
  const std::string requester = attrsPtr->get<std::string>("user_id");

  // user_id defaults to requester, or can be specified in JSON
  std::string assUserId = requester;
  if (j.isMember("user_id") && j["user_id"].isString()) {
    assUserId = j["user_id"].asString();
  }

  std::string role = "executor";
  if (j.isMember("role") && j["role"].isString()) role = j["role"].asString();

  std::optional<double> assignedHours;
  if (j.isMember("assigned_hours")) {
    if (j["assigned_hours"].isDouble() || j["assigned_hours"].isInt()) {
      assignedHours = j["assigned_hours"].asDouble();
    }
  }

  auto dbClient = app().getDbClient();
  try {
    // Determine the project root for this task and gate on OWNER/ADMIN
    auto projCheck = dbClient->execSqlSync(
        "SELECT COALESCE(project_root_id, id)::text AS proj_id "
        "FROM task WHERE id = $1::uuid LIMIT 1",
        taskId);
    if (!projCheck.empty() && !projCheck[0]["proj_id"].isNull()) {
      const std::string projId = projCheck[0]["proj_id"].as<std::string>();
      const std::string callerRole = getCallerProjectRole(dbClient, projId, requester);
      if (!isOwnerOrAdmin(callerRole)) {
        auto resp = HttpResponse::newHttpJsonResponse(
            Json::Value("Insufficient permissions"));
        resp->setStatusCode(k403Forbidden);
        return callback(resp);
      }
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

    if (role == "owner") {
      auto taskInfo = dbClient->execSqlSync(
          "SELECT parent_task_id FROM \"task\" WHERE id = $1 LIMIT 1", taskId);
      if (taskInfo.empty()) {
        auto resp =
            HttpResponse::newHttpJsonResponse(Json::Value("Task not found"));
        resp->setStatusCode(k404NotFound);
        return callback(resp);
      }

      const bool isProject = taskInfo[0]["parent_task_id"].isNull();
      if (isProject) {
        auto owners = dbClient->execSqlSync(
            "SELECT 1 FROM \"task_role_assignment\" WHERE task_id = $1 AND role = "
            "'owner' LIMIT 1",
            taskId);
        if (!owners.empty()) {
          auto resp = HttpResponse::newHttpJsonResponse(
              Json::Value("Project must have exactly one owner"));
          resp->setStatusCode(k400BadRequest);
          return callback(resp);
        }
      }
    }

    dbClient->execSqlSync("BEGIN");
    drogon_model::project_calendar::TaskAssignment ta;
    ta.setTaskId(taskId);
    ta.setUserId(assUserId);
    if (assignedHours) ta.setAssignedHours(std::to_string(*assignedHours));
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

  // Fallback: parse from path manually if routing fails
  if (taskId.empty() || taskId == "assignments") {
    std::string path = req->path();
    // Path format: /api/tasks/{uuid}/assignments
    size_t tasksPos = path.find("/tasks/");
    if (tasksPos != std::string::npos) {
      size_t uuidStart = tasksPos + 7;  // length of "/tasks/"
      size_t uuidEnd = path.find("/", uuidStart);
      if (uuidEnd != std::string::npos) {
        taskId = path.substr(uuidStart, uuidEnd - uuidStart);
      }
    }
  }

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
  std::string assId = getPathVariableCompat(req, "assignment_id");
  if (assId.empty()) {
    assId = req->getParameter("assignment_id");
  }
  if (assId.empty()) {
    auto resp =
        HttpResponse::newHttpJsonResponse(Json::Value("Missing assignment id"));
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
    auto taskRes = dbClient->execSqlSync(
        "SELECT task_id FROM \"task_assignment\" WHERE id = $1", assId);
    if (taskRes.empty()) {
      auto resp = HttpResponse::newHttpJsonResponse(
          Json::Value("Assignment not found"));
      resp->setStatusCode(k404NotFound);
      return callback(resp);
    }
    const std::string taskId = taskRes[0]["task_id"].as<std::string>();

    // Determine the project root for this task and gate on OWNER/ADMIN
    auto projCheck = dbClient->execSqlSync(
        "SELECT COALESCE(project_root_id, id)::text AS proj_id "
        "FROM task WHERE id = $1::uuid LIMIT 1",
        taskId);
    if (!projCheck.empty() && !projCheck[0]["proj_id"].isNull()) {
      const std::string projId = projCheck[0]["proj_id"].as<std::string>();
      const std::string callerRole = getCallerProjectRole(dbClient, projId, requester);
      if (!isOwnerOrAdmin(callerRole)) {
        auto resp = HttpResponse::newHttpJsonResponse(
            Json::Value("Insufficient permissions"));
        resp->setStatusCode(k403Forbidden);
        return callback(resp);
      }
    }

    auto userRes = dbClient->execSqlSync(
        "SELECT user_id FROM \"task_assignment\" WHERE id = $1", assId);
    if (userRes.empty()) {
      auto resp = HttpResponse::newHttpJsonResponse(
          Json::Value("Assignment not found"));
      resp->setStatusCode(k404NotFound);
      return callback(resp);
    }
    const std::string assUserId = userRes[0]["user_id"].as<std::string>();

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

// ============================================================================
// Notes CRUD
// ============================================================================

void TaskController::createNote(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback) {
  auto jsonPtr = req->getJsonObject();
  if (!jsonPtr || !jsonPtr->isObject()) {
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Invalid JSON"));
    resp->setStatusCode(k400BadRequest);
    return callback(resp);
  }
  const Json::Value& j = *jsonPtr;

  if (!j.isMember("content") || j["content"].isNull() ||
      !j["content"].isString() || j["content"].asString().empty()) {
    auto resp = HttpResponse::newHttpJsonResponse(
        Json::Value("Missing or empty content"));
    resp->setStatusCode(k400BadRequest);
    return callback(resp);
  }

  std::string taskId = extractTaskIdFromPath(req, "notes");
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
  const std::string userId = attrsPtr->get<std::string>("user_id");

  auto dbClient = app().getDbClient();
  try {
    auto taskExists = dbClient->execSqlSync(
        "SELECT id FROM \"task\" WHERE id = $1 LIMIT 1", taskId);
    if (taskExists.empty()) {
      auto resp =
          HttpResponse::newHttpJsonResponse(Json::Value("Task not found"));
      resp->setStatusCode(k404NotFound);
      return callback(resp);
    }

    auto check = dbClient->execSqlSync(
        "SELECT 1 FROM \"task_assignment\" WHERE task_id = $1 AND user_id = $2 "
        "LIMIT 1",
        taskId, userId);
    auto created = dbClient->execSqlSync(
        "SELECT 1 FROM \"task\" WHERE id = $1 AND created_by = $2 LIMIT 1",
        taskId, userId);
    if (check.empty() && created.empty()) {
      auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Forbidden"));
      resp->setStatusCode(k403Forbidden);
      return callback(resp);
    }

    std::string content = j["content"].asString();

    auto res = dbClient->execSqlSync(
        R"sql(
        INSERT INTO "task_note" (task_id, user_id, content)
        VALUES ($1, $2, $3)
        RETURNING id::text, task_id::text, user_id::text, content,
                  created_at::text, updated_at::text
      )sql",
        taskId, userId, content);

    if (res.empty()) {
      auto resp = HttpResponse::newHttpJsonResponse(
          Json::Value("Failed to create note"));
      resp->setStatusCode(k500InternalServerError);
      return callback(resp);
    }

    Json::Value out(Json::objectValue);
    out["id"] = res[0]["id"].as<std::string>();
    out["task_id"] = res[0]["task_id"].as<std::string>();
    out["user_id"] = res[0]["user_id"].as<std::string>();
    out["content"] = res[0]["content"].as<std::string>();
    out["created_at"] = res[0]["created_at"].as<std::string>();
    out["updated_at"] = res[0]["updated_at"].as<std::string>();

    auto resp = HttpResponse::newHttpJsonResponse(out);
    resp->setStatusCode(k201Created);
    return callback(resp);

  } catch (const std::exception& e) {
    LOG_ERROR << "createNote failed: " << e.what();
    auto resp =
        HttpResponse::newHttpJsonResponse(Json::Value("Internal server error"));
    resp->setStatusCode(k500InternalServerError);
    return callback(resp);
  }
}

void TaskController::getNotes(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback) {
  std::string taskId = extractTaskIdFromPath(req, "notes");
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
  const std::string userId = attrsPtr->get<std::string>("user_id");

  auto dbClient = app().getDbClient();
  try {
    auto check = dbClient->execSqlSync(
        "SELECT 1 FROM \"task_assignment\" WHERE task_id = $1 AND user_id = $2 "
        "LIMIT 1",
        taskId, userId);
    auto created = dbClient->execSqlSync(
        "SELECT 1 FROM \"task\" WHERE id = $1 AND created_by = $2 LIMIT 1",
        taskId, userId);
    if (check.empty() && created.empty()) {
      auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Forbidden"));
      resp->setStatusCode(k403Forbidden);
      return callback(resp);
    }

    auto res = dbClient->execSqlSync(
        R"sql(
        SELECT n.id::text, n.task_id::text, n.user_id::text, n.content,
               n.created_at::text, n.updated_at::text,
               u.display_name AS author_name
        FROM "task_note" n
        LEFT JOIN "app_user" u ON u.id = n.user_id
        WHERE n.task_id = $1
        ORDER BY n.created_at DESC
      )sql",
        taskId);

    Json::Value out(Json::arrayValue);
    for (const auto& row : res) {
      Json::Value it(Json::objectValue);
      it["id"] = row["id"].as<std::string>();
      it["task_id"] = row["task_id"].as<std::string>();
      it["user_id"] = row["user_id"].as<std::string>();
      it["content"] = row["content"].as<std::string>();
      it["created_at"] = row["created_at"].as<std::string>();
      it["updated_at"] = row["updated_at"].as<std::string>();
      it["author_name"] =
          row["author_name"].isNull()
              ? Json::Value()
              : Json::Value(row["author_name"].as<std::string>());
      out.append(it);
    }

    auto resp = HttpResponse::newHttpJsonResponse(out);
    resp->setStatusCode(k200OK);
    return callback(resp);

  } catch (const std::exception& e) {
    LOG_ERROR << "getNotes failed: " << e.what();
    auto resp =
        HttpResponse::newHttpJsonResponse(Json::Value("Internal server error"));
    resp->setStatusCode(k500InternalServerError);
    return callback(resp);
  }
}

void TaskController::updateNote(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback) {
  auto jsonPtr = req->getJsonObject();
  if (!jsonPtr || !jsonPtr->isObject()) {
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Invalid JSON"));
    resp->setStatusCode(k400BadRequest);
    return callback(resp);
  }
  const Json::Value& j = *jsonPtr;

  if (!j.isMember("content") || j["content"].isNull() ||
      !j["content"].isString() || j["content"].asString().empty()) {
    auto resp = HttpResponse::newHttpJsonResponse(
        Json::Value("Missing or empty content"));
    resp->setStatusCode(k400BadRequest);
    return callback(resp);
  }

  std::string noteId = getPathVariableCompat(req, "note_id");
  if (noteId.empty()) {
    auto resp =
        HttpResponse::newHttpJsonResponse(Json::Value("Missing note_id"));
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

  auto dbClient = app().getDbClient();
  try {
    auto noteRes = dbClient->execSqlSync(
        "SELECT user_id FROM \"task_note\" WHERE id = $1 LIMIT 1", noteId);
    if (noteRes.empty()) {
      auto resp =
          HttpResponse::newHttpJsonResponse(Json::Value("Note not found"));
      resp->setStatusCode(k404NotFound);
      return callback(resp);
    }
    if (noteRes[0]["user_id"].as<std::string>() != userId) {
      auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Forbidden"));
      resp->setStatusCode(k403Forbidden);
      return callback(resp);
    }

    std::string content = j["content"].asString();

    auto res = dbClient->execSqlSync(
        R"sql(
        UPDATE "task_note"
        SET content = $2, updated_at = NOW()
        WHERE id = $1
        RETURNING id::text, task_id::text, user_id::text, content,
                  created_at::text, updated_at::text
      )sql",
        noteId, content);

    if (res.empty()) {
      auto resp =
          HttpResponse::newHttpJsonResponse(Json::Value("Failed to update"));
      resp->setStatusCode(k500InternalServerError);
      return callback(resp);
    }

    Json::Value out(Json::objectValue);
    out["id"] = res[0]["id"].as<std::string>();
    out["task_id"] = res[0]["task_id"].as<std::string>();
    out["user_id"] = res[0]["user_id"].as<std::string>();
    out["content"] = res[0]["content"].as<std::string>();
    out["created_at"] = res[0]["created_at"].as<std::string>();
    out["updated_at"] = res[0]["updated_at"].as<std::string>();

    auto resp = HttpResponse::newHttpJsonResponse(out);
    resp->setStatusCode(k200OK);
    return callback(resp);

  } catch (const std::exception& e) {
    LOG_ERROR << "updateNote failed: " << e.what();
    auto resp =
        HttpResponse::newHttpJsonResponse(Json::Value("Internal server error"));
    resp->setStatusCode(k500InternalServerError);
    return callback(resp);
  }
}

void TaskController::deleteNote(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback) {
  std::string noteId = getPathVariableCompat(req, "note_id");
  if (noteId.empty()) {
    auto resp =
        HttpResponse::newHttpJsonResponse(Json::Value("Missing note_id"));
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

  auto dbClient = app().getDbClient();
  try {
    auto noteRes = dbClient->execSqlSync(
        "SELECT user_id, task_id FROM \"task_note\" WHERE id = $1 LIMIT 1",
        noteId);
    if (noteRes.empty()) {
      auto resp =
          HttpResponse::newHttpJsonResponse(Json::Value("Note not found"));
      resp->setStatusCode(k404NotFound);
      return callback(resp);
    }

    std::string noteAuthor = noteRes[0]["user_id"].as<std::string>();
    std::string taskId = noteRes[0]["task_id"].as<std::string>();

    if (noteAuthor != userId && !hasOwnerPermission(dbClient, taskId, userId)) {
      auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Forbidden"));
      resp->setStatusCode(k403Forbidden);
      return callback(resp);
    }

    dbClient->execSqlSync("DELETE FROM \"task_note\" WHERE id = $1", noteId);

    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Deleted"));
    resp->setStatusCode(k200OK);
    return callback(resp);

  } catch (const std::exception& e) {
    LOG_ERROR << "deleteNote failed: " << e.what();
    auto resp =
        HttpResponse::newHttpJsonResponse(Json::Value("Internal server error"));
    resp->setStatusCode(k500InternalServerError);
    return callback(resp);
  }
}