#include <drogon/HttpResponse.h>
#include <drogon/drogon.h>
#include <drogon/orm/Mapper.h>
#include <json/json.h>
#include <trantor/utils/Logger.h>

#include <any>
#include <regex>
#include <set>
#include <vector>

#include "../models/AppUser.h"
#include "../models/UserWorkSchedule.h"
#include "UsersController.h"

using namespace drogon;
using drogon_model::project_calendar::AppUser;
using drogon_model::project_calendar::UserWorkSchedule;

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

  const std::string userId = getPathVariableCompat(req, "id");
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
  if (arr.size() != 7) {
    auto resp = HttpResponse::newHttpJsonResponse(
        Json::Value("Array must contain 7 elements (one per weekday)"));
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
    dbClient->execSqlSync("BEGIN");
    dbClient->execSqlSync("DELETE FROM user_work_schedule WHERE user_id = $1",
                          userId);

    drogon::orm::Mapper<drogon_model::project_calendar::UserWorkSchedule>
        wsMapper(dbClient);
    Json::Value createdArr(Json::arrayValue);

    for (Json::UInt i = 0; i < arr.size(); ++i) {
      const Json::Value& el = arr[i];
      int dow = el["day_of_week"].asInt();
      bool isWorking = el["is_working_day"].asBool();

      drogon_model::project_calendar::UserWorkSchedule ws;
      ws.setUserId(userId);
      ws.setWeekday(static_cast<int32_t>(dow));
      if (isWorking) {
        ws.setStartTime(el["start_time"].asString());
        ws.setEndTime(el["end_time"].asString());
      }

      try {
        wsMapper.insert(ws);
      } catch (const std::exception& insertEx) {
        LOG_ERROR << "UserWorkSchedule insert failed for user " << userId
                  << ": " << insertEx.what();
        try {
          dbClient->execSqlSync("ROLLBACK");
        } catch (...) {
        }
        auto resp = HttpResponse::newHttpJsonResponse(
            Json::Value("Internal server error"));
        resp->setStatusCode(k500InternalServerError);
        callback(resp);
        return;
      }

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

    dbClient->execSqlSync("COMMIT");

    auto resp = HttpResponse::newHttpJsonResponse(createdArr);
    resp->setStatusCode(k201Created);
    callback(resp);
    return;

  } catch (const std::exception& e) {
    LOG_ERROR << "setWorkSchedule failed for user " << userId << ": "
              << e.what();
    try {
      dbClient->execSqlSync("ROLLBACK");
    } catch (...) {
    }
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

void UsersController::getUserProfile(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback) {
  const std::string requestedUserId = getPathVariableCompat(req, "id");
  if (requestedUserId.empty()) {
    auto resp =
        HttpResponse::newHttpJsonResponse(Json::Value("Missing user id"));
    resp->setStatusCode(k400BadRequest);
    callback(resp);
    return;
  }

  std::string callerUserId;
  try {
    callerUserId = req->attributes()->get<std::string>("user_id");
  } catch (const std::exception& e) {
    LOG_ERROR << "Failed to get user_id from attributes: " << e.what();
    callerUserId = "";
  }

  auto dbClient = app().getDbClient();

  try {
    drogon::orm::Mapper<AppUser> mapper(dbClient);

    AppUser user = mapper.findByPrimaryKey(requestedUserId);

    Json::Value out = user.toJson();

    out.removeMember("password_hash");

    bool isOwner = (callerUserId == requestedUserId);

    bool isVisible = user.getValueOfVisibility();

    if (!isOwner && !isVisible) {
      out.removeMember("phone");
      out.removeMember("telegram");
    }

    auto resp = HttpResponse::newHttpJsonResponse(out);
    resp->setStatusCode(k200OK);
    callback(resp);

  } catch (const drogon::orm::UnexpectedRows& e) {
    auto resp =
        HttpResponse::newHttpJsonResponse(Json::Value("User not found"));
    resp->setStatusCode(k404NotFound);
    callback(resp);
  } catch (const std::exception& e) {
    LOG_ERROR << "getUserProfile failed: " << e.what();
    auto resp =
        HttpResponse::newHttpJsonResponse(Json::Value("Internal server error"));
    resp->setStatusCode(k500InternalServerError);
    callback(resp);
  }
}

void UsersController::updateUserProfile(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback) {
  std::string userId;
  try {
    userId = req->attributes()->get<std::string>("user_id");
  } catch (...) {
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Unauthorized"));
    resp->setStatusCode(k401Unauthorized);
    callback(resp);
    return;
  }

  auto jsonPtr = req->getJsonObject();
  if (!jsonPtr || !jsonPtr->isObject()) {
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Invalid JSON"));
    resp->setStatusCode(k400BadRequest);
    callback(resp);
    return;
  }
  const Json::Value& j = *jsonPtr;

  auto dbClient = app().getDbClient();

  try {
    auto trans = dbClient->newTransaction();
    drogon::orm::Mapper<AppUser> userMapper(trans);
    drogon::orm::Mapper<UserWorkSchedule> wsMapper(trans);

    AppUser user = userMapper.findByPrimaryKey(userId);

    if (j.isMember("display_name"))
      user.setDisplayName(j["display_name"].asString());
    if (j.isMember("name")) user.setName(j["name"].asString());
    if (j.isMember("surname")) user.setSurname(j["surname"].asString());
    if (j.isMember("patronymic"))
      user.setPatronymic(j["patronymic"].asString());
    if (j.isMember("phone")) user.setPhone(j["phone"].asString());
    if (j.isMember("telegram")) user.setTelegram(j["telegram"].asString());
    if (j.isMember("locale")) user.setLocale(j["locale"].asString());
    if (j.isMember("visibility") && j["visibility"].isBool()) {
      user.setVisibility(j["visibility"].asBool());
    }

    user.setUpdatedAt(::trantor::Date::now());

    userMapper.update(user);

    if (j.isMember("work_schedule")) {
      const Json::Value& workScheduleJson = j["work_schedule"];

      if (!workScheduleJson.isArray() || workScheduleJson.size() == 0) {
        auto resp = HttpResponse::newHttpJsonResponse(
            Json::Value("work_schedule must be a non-empty array"));
        resp->setStatusCode(k400BadRequest);
        callback(resp);
        return;
      }

      wsMapper.deleteBy(drogon::orm::Criteria(UserWorkSchedule::Cols::_user_id,
                                              drogon::orm::CompareOperator::EQ,
                                              userId));

      for (Json::UInt i = 0; i < workScheduleJson.size(); ++i) {
        const Json::Value item = workScheduleJson[i];

        if (!item.isObject() || !item.isMember("weekday") ||
            !item.isMember("start_time") || !item.isMember("end_time")) {
          trans->rollback();

          auto resp = HttpResponse::newHttpJsonResponse(
              Json::Value("Invalid work_schedule item"));
          resp->setStatusCode(k400BadRequest);
          callback(resp);
          return;
        }

        UserWorkSchedule ws;
        ws.setUserId(userId);
        ws.setWeekday(static_cast<int32_t>(item["weekday"].asInt()));
        ws.setStartTime(item["start_time"].asString());
        ws.setEndTime(item["end_time"].asString());
        wsMapper.insert(ws);
      }
    }

    Json::Value out = user.toJson();
    out.removeMember("password_hash");

    if (j.isMember("work_schedule")) {
      out["work_schedule"] = j["work_schedule"];
    }

    auto resp = HttpResponse::newHttpJsonResponse(out);
    resp->setStatusCode(k200OK);
    callback(resp);

  } catch (const drogon::orm::UnexpectedRows& e) {
    auto resp =
        HttpResponse::newHttpJsonResponse(Json::Value("User not found"));
    resp->setStatusCode(k404NotFound);
    callback(resp);
  } catch (const std::exception& e) {
    LOG_ERROR << "updateUserProfile failed: " << e.what();
    auto resp =
        HttpResponse::newHttpJsonResponse(Json::Value("Internal server error"));
    resp->setStatusCode(k500InternalServerError);
    callback(resp);
  }
}