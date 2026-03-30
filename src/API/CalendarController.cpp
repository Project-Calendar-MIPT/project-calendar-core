#include "API/CalendarController.h"

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/drogon.h>
#include <json/json.h>
#include <trantor/utils/Logger.h>

#include <any>
#include <exception>
#include <string>
#include <unordered_map>
#include <vector>

using namespace drogon;

void CalendarController::getCalendarTasks(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback) {
  auto attrsPtr = req->attributes();
  if (!attrsPtr || !attrsPtr->find("user_id")) {
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Unauthorized"));
    resp->setStatusCode(k401Unauthorized);
    callback(resp);
    return;
  }
  const std::string userId = attrsPtr->get<std::string>("user_id");
  if (userId.empty()) {
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Unauthorized"));
    resp->setStatusCode(k401Unauthorized);
    callback(resp);
    return;
  }

  const std::string startParam = req->getParameter("start_date");
  const std::string endParam = req->getParameter("end_date");
  if (startParam.empty() || endParam.empty()) {
    auto resp = HttpResponse::newHttpJsonResponse(
        Json::Value("Missing start_date or end_date"));
    resp->setStatusCode(k400BadRequest);
    callback(resp);
    return;
  }
  if (startParam.size() != 10 || endParam.size() != 10) {
    auto resp = HttpResponse::newHttpJsonResponse(
        Json::Value("Invalid date format (expected YYYY-MM-DD)"));
    resp->setStatusCode(k400BadRequest);
    callback(resp);
    return;
  }
  if (startParam > endParam) {
    auto resp = HttpResponse::newHttpJsonResponse(
        Json::Value("start_date must be earlier or equal to end_date"));
    resp->setStatusCode(k400BadRequest);
    callback(resp);
    return;
  }

  auto dbClient = app().getDbClient();

  try {
    auto tasksRes = dbClient->execSqlSync(
        R"sql(
        SELECT t.id AS task_id,
               t.title AS title,
               t.start_date::text AS start_date,
               t.due_date::text   AS end_date,
               a.assigned_hours AS allocated_hours,
               r.role AS role
        FROM task t
        JOIN task_assignment a ON a.task_id = t.id AND a.user_id = $1
        LEFT JOIN task_role_assignment r ON r.task_id = t.id AND r.user_id = $1
        WHERE t.start_date <= $2::date
          AND t.due_date   >= $3::date
        ORDER BY t.start_date, t.title
      )sql",
        userId, endParam, startParam);

    Json::Value out(Json::arrayValue);

    if (tasksRes.size() == 0) {
      auto resp = HttpResponse::newHttpJsonResponse(out);
      resp->setStatusCode(k200OK);
      callback(resp);
      return;
    }

    auto schedulesRes = dbClient->execSqlSync(
        R"sql(
        SELECT ts.task_id::text AS task_id,
               ts.start_ts::text AS start_ts,
               ts.end_ts::text   AS end_ts,
               ts.hours
        FROM task_schedule ts
        JOIN task_assignment a ON a.task_id = ts.task_id AND a.user_id = $1
        WHERE ts.start_ts::date >= $2::date
          AND ts.start_ts::date <= $3::date
        ORDER BY ts.task_id, ts.start_ts
      )sql",
        userId, startParam, endParam);

    std::unordered_map<std::string, std::vector<Json::Value>> schedulesByTask;
    schedulesByTask.reserve(std::max<size_t>(1, schedulesRes.size()));
    for (const auto& srow : schedulesRes) {
      const std::string tid = srow["task_id"].as<std::string>();
      Json::Value s(Json::objectValue);

      if (!srow["start_ts"].isNull()) {
        std::string startTs = srow["start_ts"].as<std::string>();
        auto pos = startTs.find(' ');
        if (pos != std::string::npos) {
          std::string datePart = startTs.substr(0, pos);
          std::string timePart = startTs.substr(pos + 1);
          auto dot = timePart.find('.');
          if (dot != std::string::npos) timePart.erase(dot);
          s["date"] = Json::Value(datePart);
          s["start_time"] = Json::Value(timePart);
        } else {
          s["date"] = Json::Value(startTs);
          s["start_time"] = Json::Value();
        }
      } else {
        s["date"] = Json::Value();
        s["start_time"] = Json::Value();
      }

      if (!srow["end_ts"].isNull()) {
        std::string endTs = srow["end_ts"].as<std::string>();
        auto pos = endTs.find(' ');
        if (pos != std::string::npos) {
          std::string timePart = endTs.substr(pos + 1);
          auto dot = timePart.find('.');
          if (dot != std::string::npos) timePart.erase(dot);
          s["end_time"] = Json::Value(timePart);
        } else {
          s["end_time"] = Json::Value();
        }
      } else {
        s["end_time"] = Json::Value();
      }

      if (!srow["hours"].isNull()) {
        try {
          s["hours"] = Json::Value(srow["hours"].as<std::string>());
        } catch (...) {
          try {
            s["hours"] = Json::Value(srow["hours"].as<double>());
          } catch (...) {
            s["hours"] = Json::Value();
          }
        }
      } else {
        s["hours"] = Json::Value();
      }

      schedulesByTask[tid].push_back(std::move(s));
    }

    for (const auto& row : tasksRes) {
      Json::Value item(Json::objectValue);
      const std::string taskId = row["task_id"].as<std::string>();
      item["task_id"] = taskId;
      item["title"] = row["title"].isNull()
                          ? Json::Value()
                          : Json::Value(row["title"].as<std::string>());
      item["start_date"] =
          row["start_date"].isNull()
              ? Json::Value()
              : Json::Value(row["start_date"].as<std::string>());
      item["end_date"] = row["end_date"].isNull()
                             ? Json::Value()
                             : Json::Value(row["end_date"].as<std::string>());

      if (!row["allocated_hours"].isNull()) {
        try {
          item["allocated_hours"] =
              Json::Value(row["allocated_hours"].as<std::string>());
        } catch (...) {
          try {
            item["allocated_hours"] =
                Json::Value(row["allocated_hours"].as<double>());
          } catch (...) {
            item["allocated_hours"] = Json::Value();
          }
        }
      } else
        item["allocated_hours"] = Json::Value();

      item["role"] = row["role"].isNull()
                         ? Json::Value()
                         : Json::Value(row["role"].as<std::string>());

      Json::Value scheduleArr(Json::arrayValue);
      auto it = schedulesByTask.find(taskId);
      if (it != schedulesByTask.end()) {
        for (const auto& s : it->second) scheduleArr.append(s);
      }
      item["schedule"] = scheduleArr;

      out.append(item);
    }

    auto resp = HttpResponse::newHttpJsonResponse(out);
    resp->setStatusCode(k200OK);
    callback(resp);
    return;

  } catch (const std::exception& e) {
    LOG_ERROR << "getCalendarTasks failed for user "
              << attrsPtr->get<std::string>("user_id") << ": " << e.what();
    auto resp =
        HttpResponse::newHttpJsonResponse(Json::Value("Internal server error"));
    resp->setStatusCode(k500InternalServerError);
    callback(resp);
    return;
  }
}