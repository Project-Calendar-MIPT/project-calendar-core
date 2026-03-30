#include "API/AuthController.h"

#include <bcrypt.h>
#include <drogon/drogon.h>
#include <drogon/orm/DbClient.h>
#include <drogon/orm/Mapper.h>
#include <drogon/orm/Result.h>
#include <json/json.h>
#include <jwt-cpp/jwt.h>
#include <trantor/utils/Logger.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <exception>
#include <functional>
#include <utility>

#include "models/AppUser.h"
#include "models/UserWorkSchedule.h"

using drogon_model::project_calendar::AppUser;
using drogon_model::project_calendar::UserWorkSchedule;

static bool containsCaseInsensitive(const std::string& hay,
                                    const std::string& needle) {
  if (needle.empty()) return true;
  auto it = std::search(hay.begin(), hay.end(), needle.begin(), needle.end(),
                        [](char a, char b) {
                          return std::tolower(static_cast<unsigned char>(a)) ==
                                 std::tolower(static_cast<unsigned char>(b));
                        });
  return it != hay.end();
}

static std::string getJwtSecret() {
  const char* envSecret = std::getenv("JWT_SECRET");
  return (envSecret && *envSecret) ? std::string(envSecret)
                                   : std::string("replace_with_real_secret");
}

void AuthController::registerUser(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback) {
  auto jsonPtr = req->getJsonObject();
  if (!jsonPtr || !jsonPtr->isObject()) {
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Invalid JSON"));
    resp->setStatusCode(k400BadRequest);
    callback(resp);
    return;
  }
  const Json::Value& j = *jsonPtr;

  if (!j.isMember("email") || !j.isMember("password") ||
      !j.isMember("display_name") || !j.isMember("work_schedule")) {
    auto resp = HttpResponse::newHttpJsonResponse(
        Json::Value("Missing required fields"));
    resp->setStatusCode(k400BadRequest);
    callback(resp);
    return;
  }

  const std::string email = j["email"].asString();
  const std::string password = j["password"].asString();
  const std::string displayName = j["display_name"].asString();
  const std::string name = j.isMember("name") ? j["name"].asString() : "";
  const std::string surname =
      j.isMember("surname") ? j["surname"].asString() : "";
  const std::string phone = j.isMember("phone") ? j["phone"].asString() : "";
  const std::string telegram =
      j.isMember("telegram") ? j["telegram"].asString() : "";
  const std::string locale = j.isMember("locale") ? j["locale"].asString() : "";
  const Json::Value workScheduleJson = j["work_schedule"];

  if (password.size() < 8) {
    auto resp = HttpResponse::newHttpJsonResponse(
        Json::Value("Password must be at least 8 characters"));
    resp->setStatusCode(k400BadRequest);
    callback(resp);
    return;
  }

  if (!workScheduleJson.isArray() || workScheduleJson.size() == 0) {
    auto resp = HttpResponse::newHttpJsonResponse(
        Json::Value("work_schedule must be a non-empty array"));
    resp->setStatusCode(k400BadRequest);
    callback(resp);
    return;
  }

  auto dbClient = app().getDbClient();
  try {
    auto res = dbClient->execSqlSync(
        "SELECT id FROM app_user WHERE email = $1 LIMIT 1", email);
    if (res.size() > 0) {
      auto resp = HttpResponse::newHttpJsonResponse(
          Json::Value("Email already exists"));
      resp->setStatusCode(k409Conflict);
      callback(resp);
      return;
    }

    const std::string hash = bcrypt::generateHash(password);

    dbClient->execSqlSync("BEGIN");

    AppUser user;
    user.setEmail(email);
    user.setPasswordHash(hash);
    user.setDisplayName(displayName);
    if (!name.empty()) user.setName(name);
    if (!surname.empty()) user.setSurname(surname);
    if (!phone.empty()) user.setPhone(phone);
    if (!telegram.empty()) user.setTelegram(telegram);
    if (!locale.empty()) user.setLocale(locale);
    user.setCreatedAt(::trantor::Date::now());
    user.setUpdatedAt(::trantor::Date::now());

    drogon::orm::Mapper<AppUser> usersMapper(dbClient);
    usersMapper.insert(user);

    std::string createdUserId;
    try {
      createdUserId = user.getValueOfId();
    } catch (...) {
      createdUserId.clear();
    }
    if (createdUserId.empty()) {
      auto idRes = dbClient->execSqlSync(
          "SELECT id FROM app_user WHERE email = $1 LIMIT 1", email);
      if (idRes.size() == 0) {
        dbClient->execSqlSync("ROLLBACK");
        LOG_ERROR
            << "registerUser: inserted user but cannot determine id for email "
            << email;
        auto resp = HttpResponse::newHttpJsonResponse(
            Json::Value("Internal server error"));
        resp->setStatusCode(k500InternalServerError);
        callback(resp);
        return;
      }
      createdUserId = idRes[0]["id"].as<std::string>();
    }

    drogon::orm::Mapper<UserWorkSchedule> wsMapper(dbClient);
    for (Json::UInt i = 0; i < workScheduleJson.size(); ++i) {
      const Json::Value item = workScheduleJson[i];
      if (!item.isObject()) {
        dbClient->execSqlSync("ROLLBACK");
        auto resp = HttpResponse::newHttpJsonResponse(
            Json::Value("Invalid work_schedule item"));
        resp->setStatusCode(k400BadRequest);
        callback(resp);
        return;
      }
      if (!item.isMember("weekday")) {
        dbClient->execSqlSync("ROLLBACK");
        auto resp = HttpResponse::newHttpJsonResponse(
            Json::Value("Each work_schedule item must contain weekday"));
        resp->setStatusCode(k400BadRequest);
        callback(resp);
        return;
      }
      UserWorkSchedule ws;
      ws.setUserId(createdUserId);
      ws.setWeekday(static_cast<int32_t>(item["weekday"].asInt()));
      if (item.isMember("start_time") && item["start_time"].isString()) {
        ws.setStartTime(item["start_time"].asString());
      }
      if (item.isMember("end_time") && item["end_time"].isString()) {
        ws.setEndTime(item["end_time"].asString());
      }
      wsMapper.insert(ws);
    }

    dbClient->execSqlSync("COMMIT");

    const std::string token =
        jwt::create()
            .set_issuer("project-calendar")
            .set_type("JWT")
            .set_payload_claim("user_id", jwt::claim(createdUserId))
            .set_expires_at(std::chrono::system_clock::now() +
                            std::chrono::hours{24 * 7})
            .sign(jwt::algorithm::hs256{getJwtSecret()});

    Json::Value response;
    response["success"] = true;
    response["token"] = token;
    Json::Value userJson;
    userJson["id"] = createdUserId;
    userJson["email"] = email;
    userJson["display_name"] = displayName;
    if (!name.empty()) userJson["name"] = name;
    if (!surname.empty()) userJson["surname"] = surname;
    response["user"] = userJson;
    response["work_schedule"] = workScheduleJson;

    auto resp = HttpResponse::newHttpJsonResponse(response);
    resp->setStatusCode(k201Created);
    callback(resp);
    return;
  } catch (const std::exception& e) {
    try {
      dbClient->execSqlSync("ROLLBACK");
    } catch (...) {
    }
    const std::string what = e.what() ? e.what() : std::string();
    if (containsCaseInsensitive(what, "duplicate") ||
        containsCaseInsensitive(what, "unique")) {
      LOG_WARN << "registerUser conflict (email): " << what;
      auto resp = HttpResponse::newHttpJsonResponse(
          Json::Value("Email already exists"));
      resp->setStatusCode(k409Conflict);
      callback(resp);
      return;
    }
    LOG_ERROR << "registerUser failed: " << what;
    auto resp =
        HttpResponse::newHttpJsonResponse(Json::Value("Internal server error"));
    resp->setStatusCode(k500InternalServerError);
    callback(resp);
    return;
  }
}

void AuthController::login(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback) {
  auto jsonPtr = req->getJsonObject();
  if (!jsonPtr || !jsonPtr->isObject()) {
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Invalid JSON"));
    resp->setStatusCode(k400BadRequest);
    callback(resp);
    return;
  }
  const Json::Value& j = *jsonPtr;

  if (!j.isMember("email") || !j.isMember("password")) {
    auto resp = HttpResponse::newHttpJsonResponse(
        Json::Value("Missing email or password"));
    resp->setStatusCode(k400BadRequest);
    callback(resp);
    return;
  }

  const std::string email = j["email"].asString();
  const std::string password = j["password"].asString();

  auto dbClient = app().getDbClient();

  auto callbackCopy = std::move(callback);

  std::function<void(const drogon::orm::Result&)> loginResultCb =
      [callbackCopy, password](const drogon::orm::Result& r) mutable {
        try {
          if (r.size() == 0) {
            auto resp =
                HttpResponse::newHttpJsonResponse(Json::Value("Unauthorized"));
            resp->setStatusCode(k401Unauthorized);
            callbackCopy(resp);
            return;
          }

          const auto& row = r[0];
          if (row["password_hash"].isNull()) {
            auto resp =
                HttpResponse::newHttpJsonResponse(Json::Value("Unauthorized"));
            resp->setStatusCode(k401Unauthorized);
            callbackCopy(resp);
            return;
          }
          const std::string passHash = row["password_hash"].as<std::string>();

          bool ok = bcrypt::validatePassword(password, passHash);
          if (!ok) {
            auto resp =
                HttpResponse::newHttpJsonResponse(Json::Value("Unauthorized"));
            resp->setStatusCode(k401Unauthorized);
            callbackCopy(resp);
            return;
          }

          const std::string userId =
              row["id"].isNull() ? std::string() : row["id"].as<std::string>();
          const std::string displayName =
              row["display_name"].isNull()
                  ? std::string()
                  : row["display_name"].as<std::string>();
          const std::string emailVal = row["email"].isNull()
                                           ? std::string()
                                           : row["email"].as<std::string>();

          using namespace std::chrono;
          const auto now = system_clock::now();
          const auto expires = now + hours(24);

          const std::string token =
              jwt::create()
                  .set_issued_at(now)
                  .set_expires_at(expires)
                  .set_type("JWT")
                  .set_issuer("project-calendar")
                  .set_payload_claim("sub", jwt::claim(userId))
                  .set_payload_claim("display_name", jwt::claim(displayName))
                  .set_payload_claim("email", jwt::claim(emailVal))
                  .sign(jwt::algorithm::hs256{getJwtSecret()});

          Json::Value respJson;
          respJson["token"] = token;
          Json::Value userJson;
          userJson["id"] = userId;
          if (!displayName.empty()) userJson["display_name"] = displayName;
          if (!emailVal.empty()) userJson["email"] = emailVal;
          respJson["user"] = userJson;

          auto resp = HttpResponse::newHttpJsonResponse(respJson);
          resp->setStatusCode(k200OK);
          callbackCopy(resp);
        } catch (const std::exception& ex) {
          LOG_ERROR << "login handler failed: " << ex.what();
          auto resp = HttpResponse::newHttpJsonResponse(
              Json::Value("Internal server error"));
          resp->setStatusCode(k500InternalServerError);
          callbackCopy(resp);
        }
      };

  auto exceptPtrCb = [](const std::exception_ptr& ep) {
    try {
      if (ep) std::rethrow_exception(ep);
    } catch (const std::exception& e) {
      LOG_WARN << "DB error in execSqlAsync (login): " << e.what();
    } catch (...) {
      LOG_WARN << "Unknown DB error in execSqlAsync (login)";
    }
  };

  dbClient->execSqlAsync(
      "SELECT id, password_hash, display_name, email, created_at::text AS "
      "created_at, updated_at::text AS updated_at "
      "FROM app_user WHERE email = $1 LIMIT 1",
      std::move(loginResultCb), exceptPtrCb, email);
}

void AuthController::me(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback) {
  const std::string authHeader = req->getHeader("Authorization");
  const std::string bearerPrefix = "Bearer ";
  if (authHeader.size() <= bearerPrefix.size() ||
      authHeader.compare(0, bearerPrefix.size(), bearerPrefix) != 0) {
    auto resp = HttpResponse::newHttpJsonResponse(
        Json::Value("Missing or invalid Authorization header"));
    resp->setStatusCode(k401Unauthorized);
    callback(resp);
    return;
  }

  const std::string token = authHeader.substr(bearerPrefix.size());

  try {
    auto decoded = jwt::decode(token);
    auto verifier =
        jwt::verify().allow_algorithm(jwt::algorithm::hs256{getJwtSecret()});
    verifier.verify(decoded);

    std::string userId;
    if (decoded.has_payload_claim("sub")) {
      userId = decoded.get_payload_claim("sub").as_string();
    } else if (decoded.has_payload_claim("user_id")) {
      userId = decoded.get_payload_claim("user_id").as_string();
    } else {
      auto resp = HttpResponse::newHttpJsonResponse(
          Json::Value("Token does not contain user id"));
      resp->setStatusCode(k401Unauthorized);
      callback(resp);
      return;
    }

    auto dbClient = app().getDbClient();

    auto callbackCopy = std::move(callback);

    std::function<void(const drogon::orm::Result&)> meResultCb =
        [callbackCopy](const drogon::orm::Result& r) {
          try {
            if (r.size() == 0) {
              auto resp = HttpResponse::newHttpJsonResponse(
                  Json::Value("User not found"));
              resp->setStatusCode(k404NotFound);
              callbackCopy(resp);
              return;
            }
            const auto& row = r[0];
            Json::Value userJson;
            userJson["id"] = row["id"].as<std::string>();
            if (!row["display_name"].isNull())
              userJson["display_name"] = row["display_name"].as<std::string>();
            if (!row["email"].isNull())
              userJson["email"] = row["email"].as<std::string>();
            if (!row["created_at"].isNull())
              userJson["created_at"] = row["created_at"].as<std::string>();
            if (!row["updated_at"].isNull())
              userJson["updated_at"] = row["updated_at"].as<std::string>();
            auto resp = HttpResponse::newHttpJsonResponse(userJson);
            resp->setStatusCode(k200OK);
            callbackCopy(resp);
          } catch (const std::exception& ex) {
            LOG_ERROR << "me handler DB callback failed: " << ex.what();
            auto resp = HttpResponse::newHttpJsonResponse(
                Json::Value("Internal server error"));
            resp->setStatusCode(k500InternalServerError);
            callbackCopy(resp);
          }
        };

    auto exceptPtrCb = [](const std::exception_ptr& ep) {
      try {
        if (ep) std::rethrow_exception(ep);
      } catch (const std::exception& e) {
        LOG_WARN << "DB error in execSqlAsync (me): " << e.what();
      } catch (...) {
        LOG_WARN << "Unknown DB error in execSqlAsync (me)";
      }
    };

    dbClient->execSqlAsync(
        "SELECT id, display_name, email, created_at::text AS created_at, "
        "updated_at::text AS updated_at "
        "FROM app_user WHERE id = $1 LIMIT 1",
        std::move(meResultCb), exceptPtrCb, userId);
  } catch (const std::exception& e) {
    LOG_WARN << "me token verification failed: " << e.what();
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Invalid token"));
    resp->setStatusCode(k401Unauthorized);
    callback(resp);
    return;
  }
}