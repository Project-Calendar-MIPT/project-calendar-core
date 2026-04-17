#include "API/AuthController.h"

#include <drogon/drogon.h>
#include <drogon/orm/DbClient.h>
#include <drogon/orm/Mapper.h>
#include <drogon/orm/Result.h>
#include <json/json.h>
#include <jwt-cpp/jwt.h>
#include <trantor/utils/Logger.h>

#include <algorithm>
#include <bcrypt.h>
#include <cctype>
#include <cstdlib>
#include <exception>
#include <functional>
#include <utility>

#include "models/AppUser.h"
#include "models/UserSkill.h"
#include "models/UserWorkSchedule.h"

using drogon_model::project_calendar::AppUser;
using drogon_model::project_calendar::UserSkill;
using drogon_model::project_calendar::UserWorkSchedule;

static constexpr int kTokenExpiryDays = 7;

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

  if (!j.isMember("email") || !j.isMember("password")) {
    auto resp = HttpResponse::newHttpJsonResponse(
        Json::Value("Missing required fields: email and password"));
    resp->setStatusCode(k400BadRequest);
    callback(resp);
    return;
  }

  const std::string email = j["email"].asString();
  const std::string password = j["password"].asString();

  const std::string name = j.isMember("name") ? j["name"].asString()
                           : j.isMember("first_name")
                               ? j["first_name"].asString()
                               : "";
  const std::string surname = j.isMember("surname") ? j["surname"].asString()
                              : j.isMember("last_name")
                                  ? j["last_name"].asString()
                                  : "";
  const std::string middleName =
      j.isMember("middle_name") ? j["middle_name"].asString() : "";

  std::string displayName;
  if (j.isMember("display_name") && j["display_name"].isString() &&
      !j["display_name"].asString().empty()) {
    displayName = j["display_name"].asString();
  } else if (!name.empty() || !surname.empty()) {
    displayName = name + (name.empty() || surname.empty() ? "" : " ") + surname;
    displayName.erase(0, displayName.find_first_not_of(" "));
    displayName.erase(displayName.find_last_not_of(" ") + 1);
  } else {
    displayName = email.substr(0, email.find('@'));
  }

  const std::string phone = j.isMember("phone") ? j["phone"].asString() : "";
  const std::string telegram =
      j.isMember("telegram") ? j["telegram"].asString() : "";
  const std::string locale = j.isMember("locale") ? j["locale"].asString() : "";

  const std::string timezone =
      j.isMember("timezone") ? j["timezone"].asString() : "Europe/Moscow";
  const bool contactsVisible =
      j.isMember("contacts_visible") ? j["contacts_visible"].asBool() : true;
  const std::string experienceLevel =
      j.isMember("experience_level") ? j["experience_level"].asString() : "";

  Json::Value workScheduleJson(Json::arrayValue);
  if (j.isMember("work_schedule") && j["work_schedule"].isArray()) {
    workScheduleJson = j["work_schedule"];
  }

  Json::Value stackJson(Json::arrayValue);
  if (j.isMember("stack") && j["stack"].isArray()) {
    stackJson = j["stack"];
  }

  if (password.size() < 8) {
    auto resp = HttpResponse::newHttpJsonResponse(
        Json::Value("Password must be at least 8 characters"));
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

    auto trans = dbClient->newTransaction();

    AppUser user;
    user.setEmail(email);
    user.setPasswordHash(hash);
    user.setDisplayName(displayName);
    if (!name.empty()) user.setName(name);
    if (!surname.empty()) user.setSurname(surname);
    if (!middleName.empty()) user.setMiddleName(middleName);
    if (!phone.empty()) user.setPhone(phone);
    if (!telegram.empty()) user.setTelegram(telegram);
    if (!locale.empty()) user.setLocale(locale);

    user.setTimezone(timezone);
    user.setContactsVisible(contactsVisible);
    if (!experienceLevel.empty()) user.setExperienceLevel(experienceLevel);

    user.setCreatedAt(::trantor::Date::now());
    user.setUpdatedAt(::trantor::Date::now());

    drogon::orm::Mapper<AppUser> usersMapper(trans);
    usersMapper.insert(user);

    std::string createdUserId;
    try {
      createdUserId = user.getValueOfId();
    } catch (...) {
      createdUserId.clear();
    }

    if (createdUserId.empty()) {
      auto idRes = trans->execSqlSync(
          "SELECT id FROM app_user WHERE email = $1 LIMIT 1", email);
      if (idRes.size() == 0) {
        trans->rollback();
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

    if (!workScheduleJson.empty()) {
      drogon::orm::Mapper<UserWorkSchedule> wsMapper(trans);
      for (Json::UInt i = 0; i < workScheduleJson.size(); ++i) {
        const Json::Value item = workScheduleJson[i];
        if (!item.isObject() || !item.isMember("weekday")) continue;

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
    }

    if (!stackJson.empty()) {
      drogon::orm::Mapper<UserSkill> skillMapper(trans);
      for (Json::UInt i = 0; i < stackJson.size(); ++i) {
        const Json::Value item = stackJson[i];
        if (!item.isObject() || !item.isMember("name")) continue;

        UserSkill skill;
        skill.setUserId(createdUserId);
        skill.setName(item["name"].asString());
        if (item.isMember("experience_level") &&
            item["experience_level"].isString()) {
          skill.setExperienceLevel(item["experience_level"].asString());
        }
        skillMapper.insert(skill);
      }
    }

    trans.reset();

    const std::string token =
        jwt::create()
            .set_issuer("project-calendar")
            .set_type("JWT")
            .set_payload_claim("user_id", jwt::claim(createdUserId))
            .set_expires_at(std::chrono::system_clock::now() +
                            std::chrono::hours{24 * kTokenExpiryDays})
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
    if (!middleName.empty()) userJson["middle_name"] = middleName;

    userJson["timezone"] = timezone;
    userJson["contacts_visible"] = contactsVisible;
    if (!experienceLevel.empty())
      userJson["experience_level"] = experienceLevel;
    if (!stackJson.empty()) userJson["stack"] = stackJson;

    response["user"] = userJson;
    if (!workScheduleJson.empty()) {
      response["work_schedule"] = workScheduleJson;
    }

    auto resp = HttpResponse::newHttpJsonResponse(response);
    resp->setStatusCode(k201Created);
    callback(resp);
    return;
  } catch (const std::exception& e) {
    const std::string what = e.what() ? e.what() : std::string();
    if (containsCaseInsensitive(what, "duplicate") ||
        containsCaseInsensitive(what, "unique")) {
      LOG_WARN << "registerUser conflict (email/skill): " << what;
      auto resp = HttpResponse::newHttpJsonResponse(
          Json::Value("Email or constraint conflict"));
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
          const auto expires = now + hours(24 * kTokenExpiryDays);

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
        [callbackCopy, dbClient, userId](const drogon::orm::Result& r) {
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
            if (!row["name"].isNull())
              userJson["name"] = row["name"].as<std::string>();
            if (!row["surname"].isNull())
              userJson["surname"] = row["surname"].as<std::string>();
            if (!row["middle_name"].isNull())
              userJson["middle_name"] = row["middle_name"].as<std::string>();
            if (!row["phone"].isNull())
              userJson["phone"] = row["phone"].as<std::string>();
            if (!row["telegram"].isNull())
              userJson["telegram"] = row["telegram"].as<std::string>();
            if (!row["locale"].isNull())
              userJson["locale"] = row["locale"].as<std::string>();
            if (!row["timezone"].isNull())
              userJson["timezone"] = row["timezone"].as<std::string>();
            if (!row["contacts_visible"].isNull())
              userJson["contacts_visible"] = row["contacts_visible"].as<bool>();
            if (!row["experience_level"].isNull())
              userJson["experience_level"] =
                  row["experience_level"].as<std::string>();

            if (!row["created_at"].isNull())
              userJson["created_at"] = row["created_at"].as<std::string>();
            if (!row["updated_at"].isNull())
              userJson["updated_at"] = row["updated_at"].as<std::string>();

            dbClient->execSqlAsync(
                "SELECT name, experience_level FROM user_skill WHERE user_id = "
                "$1",
                [callbackCopy,
                 userJson](const drogon::orm::Result& skillRes) mutable {
                  Json::Value stackJson(Json::arrayValue);
                  for (const auto& skillRow : skillRes) {
                    Json::Value skillItem;
                    skillItem["name"] = skillRow["name"].as<std::string>();
                    if (!skillRow["experience_level"].isNull()) {
                      skillItem["experience_level"] =
                          skillRow["experience_level"].as<std::string>();
                    }
                    stackJson.append(skillItem);
                  }

                  userJson["stack"] = stackJson;

                  auto resp = HttpResponse::newHttpJsonResponse(userJson);
                  resp->setStatusCode(k200OK);
                  callbackCopy(resp);
                },
                [callbackCopy](const std::exception_ptr& ep) {
                  try {
                    if (ep) std::rethrow_exception(ep);
                  } catch (const std::exception& e) {
                    LOG_ERROR << "DB error fetching skills in me handler: "
                              << e.what();
                  }
                  auto resp = HttpResponse::newHttpJsonResponse(
                      Json::Value("Internal server error"));
                  resp->setStatusCode(k500InternalServerError);
                  callbackCopy(resp);
                },
                userId);

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
        "SELECT id, display_name, email, name, surname, middle_name, phone, "
        "telegram, locale, "
        "timezone, contacts_visible, experience_level, "
        "created_at::text AS created_at, updated_at::text AS updated_at "
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