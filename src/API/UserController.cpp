#include <drogon/HttpResponse.h>
#include <drogon/drogon.h>
#include <drogon/orm/Mapper.h>
#include <json/json.h>
#include <trantor/utils/Logger.h>

#include <chrono>
#include <cstdio>
#include <ctime>
#include <map>
#include <regex>
#include <set>

#include "API/UsersController.h"
#include "models/UserWorkSchedule.h"

using namespace drogon;

static bool isValidTime(const std::string& t) {
  static const std::regex re("^([01]\\d|2[0-3]):([0-5]\\d)$");
  return std::regex_match(t, re);
}

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

static std::string extractUserIdFromPath(const HttpRequestPtr& req,
                                         const std::string& segment) {
  std::string userId = getPathVariableCompat(req, "id");
  if (userId.empty() || userId == segment) {
    const std::string path = req->path();
    const size_t usersPos = path.find("/users/");
    if (usersPos != std::string::npos) {
      const size_t uuidStart = usersPos + 7;
      const size_t uuidEnd = path.find("/", uuidStart);
      if (uuidEnd != std::string::npos) {
        userId = path.substr(uuidStart, uuidEnd - uuidStart);
      }
    }
  }
  return userId;
}

static bool fillDefaultWorkloadRange(
    const std::shared_ptr<drogon::orm::DbClient>& dbClient,
    std::string& startTs, std::string& endTs, std::string& error) {
  if (!startTs.empty() && !endTs.empty()) return true;
  try {
    auto res = dbClient->execSqlSync(
        "SELECT now()::timestamptz::text AS start_ts, "
        "(now() + interval '7 days')::timestamptz::text AS end_ts");
    if (res.empty()) {
      error = "Failed to compute default range";
      return false;
    }
    if (startTs.empty() && !res[0]["start_ts"].isNull()) {
      startTs = res[0]["start_ts"].as<std::string>();
    }
    if (endTs.empty() && !res[0]["end_ts"].isNull()) {
      endTs = res[0]["end_ts"].as<std::string>();
    }
    return !startTs.empty() && !endTs.empty();
  } catch (const std::exception& e) {
    error = e.what();
    return false;
  }
}

static bool isValidWorkloadRange(
    const std::shared_ptr<drogon::orm::DbClient>& dbClient,
    const std::string& startTs, const std::string& endTs, std::string& error) {
  try {
    auto res = dbClient->execSqlSync(
        "SELECT ($1::timestamptz < $2::timestamptz) AS ok", startTs, endTs);
    if (res.empty() || res[0]["ok"].isNull()) return false;
    return res[0]["ok"].as<bool>();
  } catch (const std::exception& e) {
    error = e.what();
    return false;
  }
}

void UsersController::setWorkSchedule(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback) {
  auto attrsPtr = req->attributes();
  if (!attrsPtr || !attrsPtr->find("user_id")) {
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Unauthorized"));
    resp->setStatusCode(k401Unauthorized);
    callback(resp);
    return;
  }
  const std::string requesterId = attrsPtr->get<std::string>("user_id");
  if (requesterId.empty()) {
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Unauthorized"));
    resp->setStatusCode(k401Unauthorized);
    callback(resp);
    return;
  }

  const std::string userId = extractUserIdFromPath(req, "work-schedule");
  if (userId.empty()) {
    auto resp = HttpResponse::newHttpJsonResponse(
        Json::Value("Missing user id in path"));
    resp->setStatusCode(k400BadRequest);
    callback(resp);
    return;
  }
  if (requesterId != userId) {
    auto resp = HttpResponse::newHttpJsonResponse(
        Json::Value("Forbidden: cannot set schedule for another user"));
    resp->setStatusCode(k403Forbidden);
    callback(resp);
    return;
  }

  auto pj = req->getJsonObject();
  if (!pj || !pj->isArray()) {
    auto resp = HttpResponse::newHttpJsonResponse(
        Json::Value("Invalid JSON: expected array of 7 items"));
    resp->setStatusCode(k400BadRequest);
    callback(resp);
    return;
  }
  const Json::Value& arr = *pj;
  if (arr.size() < 1 || arr.size() > 7) {
    auto resp = HttpResponse::newHttpJsonResponse(
        Json::Value("Array must contain 1-7 elements (one per weekday)"));
    resp->setStatusCode(k400BadRequest);
    callback(resp);
    return;
  }

  std::set<int> daysSet;
  for (Json::UInt i = 0; i < arr.size(); ++i) {
    const Json::Value& el = arr[i];
    if (!el.isObject()) {
      auto resp = HttpResponse::newHttpJsonResponse(
          Json::Value("Each array element must be an object"));
      resp->setStatusCode(k400BadRequest);
      callback(resp);
      return;
    }
    if (!el.isMember("day_of_week") || !el["day_of_week"].isInt()) {
      auto resp = HttpResponse::newHttpJsonResponse(
          Json::Value("Each element must have integer day_of_week"));
      resp->setStatusCode(k400BadRequest);
      callback(resp);
      return;
    }
    int dow = el["day_of_week"].asInt();
    if (dow < 1 || dow > 7) {
      auto resp = HttpResponse::newHttpJsonResponse(
          Json::Value("day_of_week must be in range 1..7"));
      resp->setStatusCode(k400BadRequest);
      callback(resp);
      return;
    }
    if (dow == 7) dow = 0;  // ISO Sunday(7) → DB Sunday(0)
    if (daysSet.find(dow) != daysSet.end()) {
      auto resp = HttpResponse::newHttpJsonResponse(
          Json::Value("Duplicate day_of_week values are not allowed"));
      resp->setStatusCode(k400BadRequest);
      callback(resp);
      return;
    }
    daysSet.insert(dow);

    if (!el.isMember("is_working_day") || !el["is_working_day"].isBool()) {
      auto resp = HttpResponse::newHttpJsonResponse(
          Json::Value("Each element must have boolean is_working_day"));
      resp->setStatusCode(k400BadRequest);
      callback(resp);
      return;
    }
    bool isWorking = el["is_working_day"].asBool();
    if (isWorking) {
      if (!el.isMember("start_time") || !el.isMember("end_time") ||
          !el["start_time"].isString() || !el["end_time"].isString()) {
        auto resp = HttpResponse::newHttpJsonResponse(
            Json::Value("Working day entries must include start_time and "
                        "end_time strings"));
        resp->setStatusCode(k400BadRequest);
        callback(resp);
        return;
      }
      std::string st = el["start_time"].asString();
      std::string et = el["end_time"].asString();
      if (!isValidTime(st) || !isValidTime(et)) {
        auto resp = HttpResponse::newHttpJsonResponse(
            Json::Value("start_time and end_time must be in HH:MM format"));
        resp->setStatusCode(k400BadRequest);
        callback(resp);
        return;
      }
      if (st >= et) {
        auto resp = HttpResponse::newHttpJsonResponse(
            Json::Value("start_time must be earlier than end_time"));
        resp->setStatusCode(k400BadRequest);
        callback(resp);
        return;
      }
    } else {
      if (el.isMember("start_time") && !el["start_time"].isNull()) {
        auto resp = HttpResponse::newHttpJsonResponse(
            Json::Value("Non-working day should not include start_time"));
        resp->setStatusCode(k400BadRequest);
        callback(resp);
        return;
      }
      if (el.isMember("end_time") && !el["end_time"].isNull()) {
        auto resp = HttpResponse::newHttpJsonResponse(
            Json::Value("Non-working day should not include end_time"));
        resp->setStatusCode(k400BadRequest);
        callback(resp);
        return;
      }
    }
  }

  auto dbClient = app().getDbClient();
  try {
    auto trans = dbClient->newTransaction();
    trans->execSqlSync("DELETE FROM user_work_schedule WHERE user_id = $1",
                       userId);

    drogon::orm::Mapper<drogon_model::project_calendar::UserWorkSchedule>
        wsMapper(trans);
    Json::Value createdArr(Json::arrayValue);

    for (Json::UInt i = 0; i < arr.size(); ++i) {
      const Json::Value& el = arr[i];
      int dow = el["day_of_week"].asInt();
      if (dow == 7) dow = 0;  // ISO Sunday(7) → DB Sunday(0)
      bool isWorking = el["is_working_day"].asBool();

      // Only store working days — non-working days have no DB row
      if (!isWorking) {
        continue;
      }

      drogon_model::project_calendar::UserWorkSchedule ws;
      ws.setUserId(userId);
      ws.setWeekday(static_cast<int32_t>(dow));
      ws.setStartTime(el["start_time"].asString());
      ws.setEndTime(el["end_time"].asString());

      wsMapper.insert(ws);

      Json::Value outItem;
      outItem["id"] = ws.getId() ? ws.getValueOfId() : Json::Value();
      outItem["user_id"] = userId;
      outItem["day_of_week"] = dow;
      if (isWorking) {
        outItem["is_working_day"] = true;
        outItem["start_time"] =
            ws.getStartTime() ? ws.getValueOfStartTime() : Json::Value();
        outItem["end_time"] =
            ws.getEndTime() ? ws.getValueOfEndTime() : Json::Value();
      } else {
        outItem["is_working_day"] = false;
        outItem["start_time"] = Json::Value();
        outItem["end_time"] = Json::Value();
      }
      createdArr.append(outItem);
    }

    trans.reset();

    auto resp = HttpResponse::newHttpJsonResponse(createdArr);
    resp->setStatusCode(k201Created);
    callback(resp);
    return;

  } catch (const std::exception& e) {
    LOG_ERROR << "setWorkSchedule failed for user " << userId << ": "
              << e.what();
    auto resp =
        HttpResponse::newHttpJsonResponse(Json::Value("Internal server error"));
    resp->setStatusCode(k500InternalServerError);
    callback(resp);
    return;
  }
}

void UsersController::getWorkSchedule(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback) {
  const std::string userId = extractUserIdFromPath(req, "work-schedule");
  if (userId.empty()) {
    auto resp =
        HttpResponse::newHttpJsonResponse(Json::Value("Missing user id"));
    resp->setStatusCode(k400BadRequest);
    callback(resp);
    return;
  }

  auto dbClient = app().getDbClient();
  try {
    auto res = dbClient->execSqlSync(
        "SELECT id, user_id, weekday, start_time::text AS start_time, "
        "end_time::text AS end_time "
        "FROM user_work_schedule WHERE user_id = $1 ORDER BY weekday ASC",
        userId);

    Json::Value out(Json::arrayValue);
    out.resize(res.size());
    for (size_t i = 0; i < res.size(); ++i) {
      const auto& row = res[i];
      Json::Value item;
      if (!row["id"].isNull())
        item["id"] = row["id"].as<std::string>();
      else
        item["id"] = Json::Value();
      item["user_id"] = row["user_id"].as<std::string>();
      item["day_of_week"] = row["weekday"].as<int>();
      bool hasTimes = !row["start_time"].isNull() && !row["end_time"].isNull();
      item["is_working_day"] = hasTimes;
      if (hasTimes) {
        item["start_time"] = row["start_time"].as<std::string>();
        item["end_time"] = row["end_time"].as<std::string>();
      } else {
        item["start_time"] = Json::Value();
        item["end_time"] = Json::Value();
      }
      out[static_cast<Json::UInt>(i)] = item;
    }

    auto resp = HttpResponse::newHttpJsonResponse(out);
    resp->setStatusCode(k200OK);
    callback(resp);
    return;
  } catch (const std::exception& e) {
    LOG_ERROR << "getWorkSchedule failed for user " << userId << ": "
              << e.what();
    auto resp =
        HttpResponse::newHttpJsonResponse(Json::Value("Internal server error"));
    resp->setStatusCode(k500InternalServerError);
    callback(resp);
    return;
  }
}

void UsersController::getUserWorkload(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback) {
  auto attrsPtr = req->attributes();
  if (!attrsPtr || !attrsPtr->find("user_id")) {
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Unauthorized"));
    resp->setStatusCode(k401Unauthorized);
    callback(resp);
    return;
  }
  const std::string requesterId = attrsPtr->get<std::string>("user_id");
  if (requesterId.empty()) {
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Unauthorized"));
    resp->setStatusCode(k401Unauthorized);
    callback(resp);
    return;
  }

  const std::string userId = extractUserIdFromPath(req, "workload");
  if (userId.empty()) {
    auto resp = HttpResponse::newHttpJsonResponse(
        Json::Value("Missing user id in path"));
    resp->setStatusCode(k400BadRequest);
    callback(resp);
    return;
  }

  if (requesterId != userId) {
    // Allow if requester is owner/supervisor of any project where userId is a member
    auto dbClient2 = app().getDbClient();
    auto permitted = dbClient2->execSqlSync(
        R"sql(
        SELECT 1
        FROM task_role_assignment requester_role
        JOIN task proj ON proj.id = requester_role.task_id
                       AND proj.parent_task_id IS NULL
        WHERE requester_role.user_id = $1::uuid
          AND requester_role.role IN ('owner', 'supervisor')
          AND EXISTS (
            SELECT 1 FROM task_assignment ta
            JOIN task t ON t.id = ta.task_id
            WHERE ta.user_id = $2::uuid
              AND (t.project_root_id = proj.id OR t.id = proj.id)
          )
        LIMIT 1
        )sql",
        requesterId, userId);
    if (permitted.empty()) {
      auto resp = HttpResponse::newHttpJsonResponse(
          Json::Value("Forbidden: cannot read workload for another user"));
      resp->setStatusCode(k403Forbidden);
      callback(resp);
      return;
    }
  }

  std::string startTs = req->getParameter("start_ts");
  std::string endTs = req->getParameter("end_ts");

  auto dbClient = app().getDbClient();
  try {
    std::string rangeError;
    if (!fillDefaultWorkloadRange(dbClient, startTs, endTs, rangeError)) {
      LOG_ERROR << "getUserWorkload default range failed for user " << userId
                << ": " << rangeError;
      auto resp = HttpResponse::newHttpJsonResponse(
          Json::Value("Internal server error"));
      resp->setStatusCode(k500InternalServerError);
      callback(resp);
      return;
    }

    if (!isValidWorkloadRange(dbClient, startTs, endTs, rangeError)) {
      auto resp = HttpResponse::newHttpJsonResponse(
          Json::Value("Invalid range: start_ts must be earlier than end_ts"));
      resp->setStatusCode(k400BadRequest);
      callback(resp);
      return;
    }

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
        SELECT COALESCE(SUM(EXTRACT(EPOCH FROM (LEAST(end_ts, $3::timestamptz)-GREATEST(start_ts, $2::timestamptz)))/3600.0),0.0) AS busy_hours,
               COUNT(DISTINCT task_id) AS scheduled_tasks
        FROM task_schedule
        WHERE user_id=$1::uuid AND start_ts < $3::timestamptz AND end_ts > $2::timestamptz
      )sql",
        userId, startTs, endTs);

    const double capacityHours =
        capRes.empty() || capRes[0]["capacity_hours"].isNull()
            ? 0.0
            : capRes[0]["capacity_hours"].as<double>();
    const double busyHours =
        busyRes.empty() || busyRes[0]["busy_hours"].isNull()
            ? 0.0
            : busyRes[0]["busy_hours"].as<double>();
    const int scheduledTasks =
        busyRes.empty() || busyRes[0]["scheduled_tasks"].isNull()
            ? 0
            : busyRes[0]["scheduled_tasks"].as<int>();
    const double availableHours =
        (capacityHours - busyHours) > 0.0 ? (capacityHours - busyHours) : 0.0;
    const double loadPercent =
        capacityHours > 0.0 ? (busyHours / capacityHours) * 100.0 : 0.0;

    Json::Value out;
    out["user_id"] = userId;
    out["start_ts"] = startTs;
    out["end_ts"] = endTs;
    out["capacity_hours"] = capacityHours;
    out["busy_hours"] = busyHours;
    out["available_hours"] = availableHours;
    out["load_percent"] = loadPercent;
    out["scheduled_tasks"] = scheduledTasks;

    auto resp = HttpResponse::newHttpJsonResponse(out);
    resp->setStatusCode(k200OK);
    callback(resp);
    return;
  } catch (const std::exception& e) {
    LOG_ERROR << "getUserWorkload failed for user " << userId << ": "
              << e.what();
    auto resp =
        HttpResponse::newHttpJsonResponse(Json::Value("Internal server error"));
    resp->setStatusCode(k500InternalServerError);
    callback(resp);
    return;
  }
}

void UsersController::getNotificationSettings(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback) {
  auto attrsPtr = req->attributes();
  if (!attrsPtr || !attrsPtr->find("user_id")) {
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Unauthorized"));
    resp->setStatusCode(k401Unauthorized);
    return callback(resp);
  }
  const std::string requesterId = attrsPtr->get<std::string>("user_id");
  const std::string userId = extractUserIdFromPath(req, "notification-settings");
  if (userId.empty()) {
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Missing user id"));
    resp->setStatusCode(k400BadRequest);
    return callback(resp);
  }
  if (requesterId != userId) {
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Forbidden"));
    resp->setStatusCode(k403Forbidden);
    return callback(resp);
  }

  auto dbClient = app().getDbClient();
  try {
    auto res = dbClient->execSqlSync(
        "SELECT deadline_reminders_enabled, reminder_days_before, "
        "reminder_hours_before "
        "FROM user_notification_settings WHERE user_id = $1",
        userId);

    Json::Value out;
    if (res.empty()) {
      out["deadline_reminders_enabled"] = true;
      Json::Value days(Json::arrayValue);
      days.append(1); days.append(3); days.append(7);
      out["reminder_days_before"] = days;
      Json::Value hours(Json::arrayValue);
      out["reminder_hours_before"] = hours;
    } else {
      out["deadline_reminders_enabled"] = res[0]["deadline_reminders_enabled"].as<bool>();
      auto parseArr = [](const std::string& raw) {
        Json::Value arr(Json::arrayValue);
        std::string s = raw;
        if (!s.empty() && s.front() == '{') s = s.substr(1);
        if (!s.empty() && s.back() == '}') s.pop_back();
        std::stringstream ss(s);
        std::string item;
        while (std::getline(ss, item, ',')) {
          try { arr.append(std::stoi(item)); } catch (...) {}
        }
        return arr;
      };
      out["reminder_days_before"] =
          parseArr(res[0]["reminder_days_before"].as<std::string>());
      out["reminder_hours_before"] =
          res[0]["reminder_hours_before"].isNull()
              ? Json::Value(Json::arrayValue)
              : parseArr(res[0]["reminder_hours_before"].as<std::string>());
    }

    auto resp = HttpResponse::newHttpJsonResponse(out);
    resp->setStatusCode(k200OK);
    return callback(resp);
  } catch (const std::exception& e) {
    LOG_ERROR << "getNotificationSettings failed: " << e.what();
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Internal server error"));
    resp->setStatusCode(k500InternalServerError);
    return callback(resp);
  }
}

void UsersController::setNotificationSettings(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback) {
  auto attrsPtr = req->attributes();
  if (!attrsPtr || !attrsPtr->find("user_id")) {
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Unauthorized"));
    resp->setStatusCode(k401Unauthorized);
    return callback(resp);
  }
  const std::string requesterId = attrsPtr->get<std::string>("user_id");
  const std::string userId = extractUserIdFromPath(req, "notification-settings");
  if (userId.empty()) {
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Missing user id"));
    resp->setStatusCode(k400BadRequest);
    return callback(resp);
  }
  if (requesterId != userId) {
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Forbidden"));
    resp->setStatusCode(k403Forbidden);
    return callback(resp);
  }

  auto pj = req->getJsonObject();
  if (!pj || !pj->isObject()) {
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Invalid JSON"));
    resp->setStatusCode(k400BadRequest);
    return callback(resp);
  }
  const Json::Value& j = *pj;

  if (!j.isMember("deadline_reminders_enabled") || !j["deadline_reminders_enabled"].isBool()) {
    auto resp = HttpResponse::newHttpJsonResponse(
        Json::Value("deadline_reminders_enabled (bool) is required"));
    resp->setStatusCode(k400BadRequest);
    return callback(resp);
  }

  const bool enabled = j["deadline_reminders_enabled"].asBool();

  auto buildPgArray = [&](const Json::Value& arr, int maxVal) -> std::pair<bool, std::string> {
    std::string pg = "{";
    for (Json::UInt i = 0; i < arr.size(); ++i) {
      if (!arr[i].isInt()) return {false, ""};
      int v = arr[i].asInt();
      if (v < 1 || v > maxVal) return {false, ""};
      if (i > 0) pg += ",";
      pg += std::to_string(v);
    }
    pg += "}";
    return {true, pg};
  };

  std::string daysPg = "{}";
  if (j.isMember("reminder_days_before") && j["reminder_days_before"].isArray()) {
    auto [ok, pg] = buildPgArray(j["reminder_days_before"], 365);
    if (!ok) {
      auto resp = HttpResponse::newHttpJsonResponse(
          Json::Value("reminder_days_before must be integers 1-365"));
      resp->setStatusCode(k400BadRequest);
      return callback(resp);
    }
    daysPg = pg;
  }

  std::string hoursPg = "{}";
  if (j.isMember("reminder_hours_before") && j["reminder_hours_before"].isArray()) {
    auto [ok, pg] = buildPgArray(j["reminder_hours_before"], 72);
    if (!ok) {
      auto resp = HttpResponse::newHttpJsonResponse(
          Json::Value("reminder_hours_before must be integers 1-72"));
      resp->setStatusCode(k400BadRequest);
      return callback(resp);
    }
    hoursPg = pg;
  }

  auto dbClient = app().getDbClient();
  try {
    dbClient->execSqlSync(
        "INSERT INTO user_notification_settings "
        "(user_id, deadline_reminders_enabled, reminder_days_before, reminder_hours_before, updated_at) "
        "VALUES ($1::uuid, $2, $3::integer[], $4::integer[], NOW()) "
        "ON CONFLICT (user_id) DO UPDATE SET "
        "  deadline_reminders_enabled = EXCLUDED.deadline_reminders_enabled, "
        "  reminder_days_before = EXCLUDED.reminder_days_before, "
        "  reminder_hours_before = EXCLUDED.reminder_hours_before, "
        "  updated_at = NOW()",
        userId, enabled, daysPg, hoursPg);

    Json::Value out;
    out["deadline_reminders_enabled"] = enabled;
    out["reminder_days_before"] =
        j.isMember("reminder_days_before") ? j["reminder_days_before"] : Json::Value(Json::arrayValue);
    out["reminder_hours_before"] =
        j.isMember("reminder_hours_before") ? j["reminder_hours_before"] : Json::Value(Json::arrayValue);
    auto resp = HttpResponse::newHttpJsonResponse(out);
    resp->setStatusCode(k200OK);
    return callback(resp);
  } catch (const std::exception& e) {
    LOG_ERROR << "setNotificationSettings failed: " << e.what();
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Internal server error"));
    resp->setStatusCode(k500InternalServerError);
    return callback(resp);
  }
}

void UsersController::searchUsers(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback) {
  const std::string q = req->getParameter("search");
  if (q.empty()) {
    auto resp =
        HttpResponse::newHttpJsonResponse(Json::Value(Json::arrayValue));
    resp->setStatusCode(k200OK);
    callback(resp);
    return;
  }

  std::string pattern = "%" + q + "%";

  auto dbClient = app().getDbClient();
  try {
    auto res = dbClient->execSqlSync(
        "SELECT id, email, display_name, name, surname, locale, "
        "created_at::text AS created_at "
        "FROM app_user "
        "WHERE email ILIKE $1 OR display_name ILIKE $1 OR name ILIKE $1 OR "
        "surname ILIKE $1 "
        "LIMIT 20",
        pattern);

    Json::Value out(Json::arrayValue);
    for (const auto& row : res) {
      Json::Value u;
      u["id"] = row["id"].as<std::string>();
      u["email"] = row["email"].isNull()
                       ? Json::Value()
                       : Json::Value(row["email"].as<std::string>());
      u["display_name"] =
          row["display_name"].isNull()
              ? Json::Value()
              : Json::Value(row["display_name"].as<std::string>());
      u["name"] = row["name"].isNull()
                      ? Json::Value()
                      : Json::Value(row["name"].as<std::string>());
      u["surname"] = row["surname"].isNull()
                         ? Json::Value()
                         : Json::Value(row["surname"].as<std::string>());
      u["locale"] = row["locale"].isNull()
                        ? Json::Value()
                        : Json::Value(row["locale"].as<std::string>());
      if (!row["created_at"].isNull())
        u["created_at"] = row["created_at"].as<std::string>();
      out.append(u);
    }

    auto resp = HttpResponse::newHttpJsonResponse(out);
    resp->setStatusCode(k200OK);
    callback(resp);
    return;

  } catch (const std::exception& e) {
    LOG_ERROR << "searchUsers failed: " << e.what();
    auto resp =
        HttpResponse::newHttpJsonResponse(Json::Value("Internal server error"));
    resp->setStatusCode(k500InternalServerError);
    callback(resp);
    return;
  }
}

void UsersController::getMySubordinates(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback) {
  auto attrsPtr = req->attributes();
  if (!attrsPtr || !attrsPtr->find("user_id")) {
    auto r = HttpResponse::newHttpJsonResponse(Json::Value("Unauthorized"));
    r->setStatusCode(k401Unauthorized); return callback(r);
  }
  const std::string me = attrsPtr->get<std::string>("user_id");
  auto db = drogon::app().getDbClient();
  try {
    auto res = db->execSqlSync(
      R"sql(
        SELECT DISTINCT u.id::text, u.display_name, u.email
        FROM app_user u
        JOIN task_role_assignment ra2 ON ra2.user_id = u.id AND ra2.role = 'executor'
        JOIN task_role_assignment ra1 ON ra1.task_id = ra2.task_id
             AND ra1.user_id = $1::uuid AND ra1.role IN ('owner', 'supervisor')
        WHERE u.id != $1::uuid
        ORDER BY u.display_name
      )sql", me);
    Json::Value arr(Json::arrayValue);
    for (const auto& row : res) {
      Json::Value u;
      u["id"]           = row["id"].as<std::string>();
      u["display_name"] = row["display_name"].isNull() ? "" : row["display_name"].as<std::string>();
      u["email"]        = row["email"].isNull() ? "" : row["email"].as<std::string>();
      arr.append(u);
    }
    auto r = HttpResponse::newHttpJsonResponse(arr);
    r->setStatusCode(k200OK); callback(r);
  } catch (const std::exception& e) {
    LOG_ERROR << "getMySubordinates: " << e.what();
    auto r = HttpResponse::newHttpJsonResponse(Json::Value("Internal server error"));
    r->setStatusCode(k500InternalServerError); callback(r);
  }
}

void UsersController::isSubordinate(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback) {
  auto attrsPtr = req->attributes();
  if (!attrsPtr || !attrsPtr->find("user_id")) {
    auto r = HttpResponse::newHttpJsonResponse(Json::Value("Unauthorized"));
    r->setStatusCode(k401Unauthorized); return callback(r);
  }
  const std::string me = attrsPtr->get<std::string>("user_id");
  const std::string targetId = extractUserIdFromPath(req, "is-subordinate");
  if (targetId.empty()) {
    auto r = HttpResponse::newHttpJsonResponse(Json::Value("Missing user id"));
    r->setStatusCode(k400BadRequest); return callback(r);
  }
  auto db = drogon::app().getDbClient();
  try {
    auto res = db->execSqlSync(
      R"sql(
        SELECT EXISTS (
          SELECT 1 FROM task_role_assignment ra1
          JOIN task_role_assignment ra2 ON ra1.task_id = ra2.task_id
          WHERE ra1.user_id = $1::uuid AND ra1.role IN ('owner','supervisor')
            AND ra2.user_id = $2::uuid AND ra2.role = 'executor'
        ) AS is_sub
      )sql", me, targetId);
    Json::Value j;
    j["is_subordinate"] = res[0]["is_sub"].as<bool>();
    auto r = HttpResponse::newHttpJsonResponse(j);
    r->setStatusCode(k200OK); callback(r);
  } catch (const std::exception& e) {
    LOG_ERROR << "isSubordinate: " << e.what();
    auto r = HttpResponse::newHttpJsonResponse(Json::Value("Internal server error"));
    r->setStatusCode(k500InternalServerError); callback(r);
  }
}

void UsersController::getUserAvailability(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback) {
  auto attrsPtr = req->attributes();
  if (!attrsPtr || !attrsPtr->find("user_id")) {
    auto r = HttpResponse::newHttpJsonResponse(Json::Value("Unauthorized"));
    r->setStatusCode(k401Unauthorized); return callback(r);
  }
  const std::string targetId = extractUserIdFromPath(req, "availability");
  if (targetId.empty()) {
    auto r = HttpResponse::newHttpJsonResponse(Json::Value("Missing user id"));
    r->setStatusCode(k400BadRequest); return callback(r);
  }
  std::string weekParam = req->getParameter("week");
  if (weekParam.empty()) {
    auto now = std::chrono::system_clock::now();
    auto t   = std::chrono::system_clock::to_time_t(now);
    char buf[12]; std::strftime(buf, sizeof(buf), "%Y-%m-%d", std::gmtime(&t));
    weekParam = buf;
  }
  auto db = drogon::app().getDbClient();
  try {
    // Tasks: any task where user is assigned, with start_date in the week
    auto taskRes = db->execSqlSync(
      R"sql(
        SELECT DISTINCT t.start_date::text AS d
        FROM task t
        JOIN task_role_assignment ra ON ra.task_id = t.id
        WHERE ra.user_id = $1::uuid
          AND t.start_date >= date_trunc('week', $2::date)
          AND t.start_date <  date_trunc('week', $2::date) + INTERVAL '7 days'
          AND t.status NOT IN ('completed','cancelled')
      )sql", targetId, weekParam);

    // Meetings where user is participant
    auto meetRes = db->execSqlSync(
      R"sql(
        SELECT (m.start_at AT TIME ZONE 'UTC')::text AS start_utc,
               m.duration_min
        FROM meeting m
        JOIN meeting_participant mp ON mp.meeting_id = m.id
        WHERE mp.user_id = $1::uuid
          AND m.start_at >= date_trunc('week', $2::date::timestamptz)
          AND m.start_at <  date_trunc('week', $2::date::timestamptz) + INTERVAL '7 days'
      )sql", targetId, weekParam);

    // Work schedule for this user
    auto schedRes = db->execSqlSync(
      R"sql(
        SELECT day_of_week, start_time::text, end_time::text, is_working_day
        FROM work_schedule WHERE user_id = $1::uuid
      )sql", targetId);

    // Build schedule map: day_of_week (1=Mon) -> {start, end}
    std::map<int, std::pair<std::string,std::string>> sched;
    for (const auto& row : schedRes) {
      if (!row["is_working_day"].as<bool>()) continue;
      int dow = row["day_of_week"].as<int>();
      std::string st = row["start_time"].isNull() ? "09:00" : row["start_time"].as<std::string>().substr(0,5);
      std::string et = row["end_time"].isNull()   ? "18:00" : row["end_time"].as<std::string>().substr(0,5);
      sched[dow] = {st, et};
    }
    // Default schedule if none set
    if (sched.empty()) {
      for (int d = 1; d <= 5; d++) sched[d] = {"09:00", "18:00"};
    }

    std::map<std::string, std::vector<std::pair<std::string,std::string>>> busyByDate;

    // Tasks: mark full work day as busy
    for (const auto& row : taskRes) {
      std::string dateStr = row["d"].as<std::string>();
      auto dowRes = db->execSqlSync(
          "SELECT EXTRACT(ISODOW FROM $1::date)::int AS dow", dateStr);
      if (dowRes.empty()) continue;
      int dow = dowRes[0]["dow"].as<int>();
      auto it = sched.find(dow);
      if (it != sched.end()) {
        busyByDate[dateStr].emplace_back(it->second.first, it->second.second);
      }
    }

    // Meetings: exact time slots
    for (const auto& row : meetRes) {
      if (row["start_utc"].isNull()) continue;
      std::string startUtc = row["start_utc"].as<std::string>(); // "2026-05-12 10:00:00"
      int durMin = row["duration_min"].as<int>();
      std::string dateStr = startUtc.substr(0,10);
      std::string startTime = startUtc.length() >= 16 ? startUtc.substr(11,5) : "00:00";
      int h = std::stoi(startTime.substr(0,2));
      int m_val = std::stoi(startTime.substr(3,2));
      m_val += durMin; h += m_val/60; m_val %= 60;
      if (h > 23) h = 23;
      char endBuf[6]; std::snprintf(endBuf, sizeof(endBuf), "%02d:%02d", h, m_val);
      busyByDate[dateStr].emplace_back(startTime, std::string(endBuf));
    }

    Json::Value result(Json::arrayValue);
    for (auto& [date, slots] : busyByDate) {
      Json::Value day;
      day["date"] = date;
      Json::Value slotsArr(Json::arrayValue);
      for (auto& [s, e] : slots) {
        Json::Value slot;
        slot["start"] = s; slot["end"] = e;
        slotsArr.append(slot);
      }
      day["busy_slots"] = slotsArr;
      result.append(day);
    }
    auto r = HttpResponse::newHttpJsonResponse(result);
    r->setStatusCode(k200OK); callback(r);
  } catch (const std::exception& ex) {
    LOG_ERROR << "getUserAvailability: " << ex.what();
    auto r = HttpResponse::newHttpJsonResponse(Json::Value("Internal server error"));
    r->setStatusCode(k500InternalServerError); callback(r);
  }
}

void UsersController::getUserProfile(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback) {
  const std::string userId = getPathVariableCompat(req, "id");
  if (userId.empty()) {
    auto resp =
        HttpResponse::newHttpJsonResponse(Json::Value("Missing user id"));
    resp->setStatusCode(k400BadRequest);
    callback(resp);
    return;
  }

  auto dbClient = app().getDbClient();
  try {
    auto res = dbClient->execSqlSync(
        "SELECT id, email, display_name, name, surname, phone, telegram, "
        "locale, "
        "created_at::text AS created_at, updated_at::text AS updated_at "
        "FROM app_user WHERE id = $1 LIMIT 1",
        userId);

    if (res.size() == 0) {
      auto resp =
          HttpResponse::newHttpJsonResponse(Json::Value("User not found"));
      resp->setStatusCode(k404NotFound);
      callback(resp);
      return;
    }

    const auto& row = res[0];
    Json::Value out;
    out["id"] = row["id"].as<std::string>();
    out["email"] = row["email"].isNull()
                       ? Json::Value()
                       : Json::Value(row["email"].as<std::string>());
    out["display_name"] =
        row["display_name"].isNull()
            ? Json::Value()
            : Json::Value(row["display_name"].as<std::string>());
    out["name"] = row["name"].isNull()
                      ? Json::Value()
                      : Json::Value(row["name"].as<std::string>());
    out["surname"] = row["surname"].isNull()
                         ? Json::Value()
                         : Json::Value(row["surname"].as<std::string>());
    out["phone"] = row["phone"].isNull()
                       ? Json::Value()
                       : Json::Value(row["phone"].as<std::string>());
    out["telegram"] = row["telegram"].isNull()
                          ? Json::Value()
                          : Json::Value(row["telegram"].as<std::string>());
    out["locale"] = row["locale"].isNull()
                        ? Json::Value()
                        : Json::Value(row["locale"].as<std::string>());
    if (!row["created_at"].isNull())
      out["created_at"] = row["created_at"].as<std::string>();
    if (!row["updated_at"].isNull())
      out["updated_at"] = row["updated_at"].as<std::string>();

    auto resp = HttpResponse::newHttpJsonResponse(out);
    resp->setStatusCode(k200OK);
    callback(resp);
    return;
  } catch (const std::exception& e) {
    const std::string what = e.what() ? e.what() : std::string();
    LOG_ERROR << "getUserProfile failed for user " << userId << ": " << what;
    // PostgreSQL UUID parse errors → 400 Bad Request
    if (what.find("uuid") != std::string::npos ||
        what.find("invalid input syntax") != std::string::npos) {
      auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Invalid user id format"));
      resp->setStatusCode(k400BadRequest);
      callback(resp);
      return;
    }
    auto resp =
        HttpResponse::newHttpJsonResponse(Json::Value("Internal server error"));
    resp->setStatusCode(k500InternalServerError);
    callback(resp);
    return;
  }
}