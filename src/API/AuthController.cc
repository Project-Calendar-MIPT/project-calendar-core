#include "AuthController.h"

#include <bcrypt.h>
#include <drogon/drogon.h>
#include <drogon/orm/DbClient.h>
#include <drogon/orm/Mapper.h>
#include <drogon/orm/Result.h>
#include <drogon/utils/Utilities.h>
#include <json/json.h>
#include <jwt-cpp/jwt.h>
#include <trantor/utils/Logger.h>

#include <algorithm>
#include <cctype>
#include <exception>
#include <functional>
#include <regex>
#include <unordered_set>
#include <utility>

#include "../models/AppUser.h"
#include "../models/UserWorkSchedule.h"

using drogon_model::project_calendar::AppUser;
using drogon_model::project_calendar::UserWorkSchedule;

static void sendConfirmationEmailLog(const std::string& email,
                                     const std::string& token) {
  std::string confirmUrl =
      "http://localhost:8080/api/auth/confirm?token=" + token;
  LOG_INFO << "=================================================";
  LOG_INFO << "ИМИТАЦИЯ ОТПРАВКИ EMAIL ПОДТВЕРЖДЕНИЯ:";
  LOG_INFO << "Кому: " << email;
  LOG_INFO << "Ссылка для подтверждения: " << confirmUrl;
  LOG_INFO << "=================================================";
}

static void sendLoginEmailLog(const std::string& email,
                              const std::string& name) {
  LOG_INFO << "=================================================";
  LOG_INFO << "ИМИТАЦИЯ ОТПРАВКИ EMAIL УВЕДОМЛЕНИЯ О ВХОДЕ:";
  LOG_INFO << "Кому: " << email;
  LOG_INFO << "Сообщение: Пользователь " << name << " успешно вошел в систему.";
  LOG_INFO << "=================================================";
}

static void sendPasswordResetEmailLog(const std::string& email,
                                      const std::string& token) {
  std::string resetUrl = "http://localhost:8080/reset-password?token=" + token;
  LOG_INFO << "=================================================";
  LOG_INFO << "ИМИТАЦИЯ ОТПРАВКИ EMAIL (СБРОС ПАРОЛЯ):";
  LOG_INFO << "Кому: " << email;
  LOG_INFO << "Ссылка для восстановления: " << resetUrl;
  LOG_INFO << "=================================================";
}

static bool isValidEmail(const std::string& email, std::string& errMsg) {
  const std::regex emailRegex(
      R"(^[a-zA-Z0-9_.+-]+@([a-zA-Z0-9-]+\.[a-zA-Z0-9-.]+)$)");
  std::smatch emailMatch;

  if (!std::regex_match(email, emailMatch, emailRegex)) {
    errMsg = "Invalid email format";
    return false;
  }

  std::string domain = emailMatch[1].str();
  std::unordered_set<std::string> allowedDomains = {
      "gmail.com", "yandex.ru", "mail.ru", "vk.com", "ya.ru", "phystech.edu"};

  if (allowedDomains.find(domain) == allowedDomains.end()) {
    errMsg = "Email domain is not allowed";
    return false;
  }
  return true;
}

static bool isStrongPassword(const std::string& password, std::string& errMsg) {
  if (password.size() < 8) {
    errMsg = "Password must be at least 8 characters long";
    return false;
  }
  bool hasDigit = std::any_of(password.begin(), password.end(), ::isdigit);
  bool hasSpecial = std::any_of(password.begin(), password.end(), ::ispunct);
  if (!hasDigit || !hasSpecial) {
    errMsg =
        "Password must contain at least one digit and one special character";
    return false;
  }
  return true;
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

  if (!j.isMember("email") || !j.isMember("password") || !j.isMember("name") ||
      !j.isMember("surname") || !j.isMember("display_name") ||
      !j.isMember("work_schedule")) {
    auto resp = HttpResponse::newHttpJsonResponse(
        Json::Value("Missing required fields"));
    resp->setStatusCode(k400BadRequest);
    callback(resp);
    return;
  }

  const std::string email = j["email"].asString();
  const std::string password = j["password"].asString();
  const std::string displayName = j["display_name"].asString();
  const std::string name = j["name"].asString();
  const std::string surname = j["surname"].asString();
  const std::string patronymic =
      j.isMember("patronymic") ? j["patronymic"].asString() : "";
  const std::string phone = j.isMember("phone") ? j["phone"].asString() : "";
  const std::string telegram =
      j.isMember("telegram") ? j["telegram"].asString() : "";
  const bool visibility = j.isMember("visibility") && j["visibility"].isBool()
                              ? j["visibility"].asBool()
                              : false;
  const std::string locale = j.isMember("locale") ? j["locale"].asString() : "";
  const Json::Value workScheduleJson = j["work_schedule"];
  const std::string experienceLevel =
      j.isMember("experience_level") ? j["experience_level"].asString() : "";

  Json::Value skillsJson(Json::arrayValue);
  if (j.isMember("skills") && j["skills"].isArray()) {
    skillsJson = j["skills"];
  }

  std::string errMsg;
  if (!isValidEmail(email, errMsg)) {
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value(errMsg));
    resp->setStatusCode(k400BadRequest);
    return callback(resp);
  }

  if (!isStrongPassword(password, errMsg)) {
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value(errMsg));
    resp->setStatusCode(k400BadRequest);
    return callback(resp);
  }

  if (!workScheduleJson.isArray() || workScheduleJson.size() != 7) {
    auto resp = HttpResponse::newHttpJsonResponse(
        Json::Value("work_schedule must be an array of exactly 7 items"));
    resp->setStatusCode(k400BadRequest);
    callback(resp);
    return;
  }

  for (Json::UInt i = 0; i < workScheduleJson.size(); ++i) {
    const Json::Value& item = workScheduleJson[i];
    if (!item.isObject() || !item.isMember("day_of_week") ||
        !item.isMember("time_slots")) {
      auto resp = HttpResponse::newHttpJsonResponse(
          Json::Value("Each work_schedule item must contain 'day_of_week' and "
                      "'time_slots'"));
      resp->setStatusCode(k400BadRequest);
      callback(resp);
      return;
    }
    if (!item["time_slots"].isArray()) {
      auto resp = HttpResponse::newHttpJsonResponse(
          Json::Value("'time_slots' must be an array"));
      resp->setStatusCode(k400BadRequest);
      callback(resp);
      return;
    }
  }

  auto dbClient = app().getDbClient();

  try {
    drogon::orm::Mapper<AppUser> usersMapper(dbClient);
    drogon::orm::Mapper<UserWorkSchedule> wsMapper(dbClient);

    auto criteria = drogon::orm::Criteria(
        AppUser::Cols::_email, drogon::orm::CompareOperator::EQ, email);
    if (usersMapper.count(criteria) > 0) {
      auto resp = HttpResponse::newHttpJsonResponse(
          Json::Value("Email already exists"));
      resp->setStatusCode(k409Conflict);
      callback(resp);
      return;
    }

    auto trans = dbClient->newTransaction();

    drogon::orm::Mapper<AppUser> transUsersMapper(trans);
    drogon::orm::Mapper<UserWorkSchedule> transWsMapper(trans);

    const std::string hash = bcrypt::generateHash(password);
    AppUser user;
    user.setEmail(email);
    user.setPasswordHash(hash);
    user.setDisplayName(displayName);
    user.setName(name);
    user.setSurname(surname);
    if (!patronymic.empty()) user.setPatronymic(patronymic);
    if (!phone.empty()) user.setPhone(phone);
    if (!telegram.empty()) user.setTelegram(telegram);
    if (!locale.empty()) user.setLocale(locale);
    user.setVisibility(visibility);
    user.setCreatedAt(::trantor::Date::now());
    user.setUpdatedAt(::trantor::Date::now());

    std::string confirmationToken = drogon::utils::getUuid();
    user.setConfirmationToken(confirmationToken);
    user.setIsVerified(false);

    if (!experienceLevel.empty()) {
      user.setExperienceLevel(experienceLevel);
    }

    Json::FastWriter writer;
    std::string skillsStr = writer.write(skillsJson);
    user.setSkills(skillsStr);

    transUsersMapper.insert(user);
    std::string createdUserId = user.getValueOfId();

    for (Json::UInt i = 0; i < workScheduleJson.size(); ++i) {
      const Json::Value& item = workScheduleJson[i];

      UserWorkSchedule ws;
      ws.setUserId(createdUserId);
      ws.setWeekday(static_cast<int32_t>(item["day_of_week"].asInt()));

      std::string slotsStr = writer.write(item["time_slots"]);
      ws.setTimeSlots(slotsStr);

      transWsMapper.insert(ws);
    }

    sendConfirmationEmailLog(email, confirmationToken);

    Json::Value response;
    response["success"] = true;
    response["message"] =
        "Registration successful. Please check your email to verify your "
        "account.";

    Json::Value userJson;
    userJson["id"] = createdUserId;
    userJson["email"] = email;
    userJson["display_name"] = displayName;
    userJson["name"] = name;
    userJson["surname"] = surname;

    response["user"] = userJson;
    response["work_schedule"] = workScheduleJson;

    userJson["experience_level"] = experienceLevel;
    userJson["skills"] = skillsJson;

    auto resp = HttpResponse::newHttpJsonResponse(response);
    resp->setStatusCode(k201Created);
    callback(resp);

  } catch (const std::exception& e) {
    LOG_ERROR << "registerUser failed: " << e.what();

    auto resp =
        HttpResponse::newHttpJsonResponse(Json::Value("Internal server error"));
    resp->setStatusCode(k500InternalServerError);
    callback(resp);
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

  drogon::orm::Mapper<AppUser> mapper(dbClient);
  auto criteria = drogon::orm::Criteria(
      AppUser::Cols::_email, drogon::orm::CompareOperator::EQ, email);

  mapper.findBy(
      criteria,
      [callbackCopy, password](const std::vector<AppUser>& users) {
        if (users.empty()) {
          auto resp =
              HttpResponse::newHttpJsonResponse(Json::Value("Unauthorized"));
          resp->setStatusCode(k401Unauthorized);
          callbackCopy(resp);
          return;
        }

        const AppUser& user = users[0];

        std::string passHash = user.getValueOfPasswordHash();
        if (passHash.empty()) {
          auto resp =
              HttpResponse::newHttpJsonResponse(Json::Value("Unauthorized"));
          resp->setStatusCode(k401Unauthorized);
          callbackCopy(resp);
          return;
        }

        bool ok = bcrypt::validatePassword(password, passHash);
        if (!ok) {
          auto resp =
              HttpResponse::newHttpJsonResponse(Json::Value("Unauthorized"));
          resp->setStatusCode(k401Unauthorized);
          callbackCopy(resp);
          return;
        }

        bool isVerified = user.getValueOfIsVerified();
        if (!isVerified) {
          auto resp = HttpResponse::newHttpJsonResponse(
              Json::Value("Email is not verified. Please check your inbox."));
          resp->setStatusCode(k403Forbidden);
          callbackCopy(resp);
          return;
        }

        const char* envSecret = std::getenv("JWT_SECRET");
        const std::string secret = envSecret ? envSecret : "secret_key";

        using namespace std::chrono;
        auto now = system_clock::now();
        auto expires = now + hours(24);

        const std::string userId = user.getValueOfId();
        const std::string displayName = user.getValueOfDisplayName();
        const std::string emailVal = user.getValueOfEmail();

        auto token =
            jwt::create()
                .set_issued_at(now)
                .set_expires_at(expires)
                .set_type("JWT")
                .set_issuer("project-calendar")
                .set_payload_claim("sub", jwt::claim(userId))
                .set_payload_claim("display_name", jwt::claim(displayName))
                .set_payload_claim("email", jwt::claim(emailVal))
                .sign(jwt::algorithm::hs256{secret});

        Json::Value respJson;
        respJson["token"] = token;
        Json::Value userJson;
        userJson["id"] = userId;
        if (!displayName.empty()) userJson["display_name"] = displayName;
        if (!emailVal.empty()) userJson["email"] = emailVal;
        respJson["user"] = userJson;

        sendLoginEmailLog(emailVal, displayName);

        auto resp = HttpResponse::newHttpJsonResponse(respJson);
        resp->setStatusCode(k200OK);
        callbackCopy(resp);
      },
      [callbackCopy](const drogon::orm::DrogonDbException& e) {
        LOG_ERROR << "DB error in login: " << e.base().what();
        auto resp = HttpResponse::newHttpJsonResponse(
            Json::Value("Internal server error"));
        resp->setStatusCode(k500InternalServerError);
        callbackCopy(resp);
      });
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

  const char* envSecret = std::getenv("JWT_SECRET");
  const std::string secret = envSecret ? envSecret : "secret_key";

  try {
    auto decoded = jwt::decode(token);
    auto verifier =
        jwt::verify().allow_algorithm(jwt::algorithm::hs256{secret});
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

    drogon::orm::Mapper<AppUser> mapper(dbClient);

    auto criteria = drogon::orm::Criteria(
        AppUser::Cols::_id, drogon::orm::CompareOperator::EQ, userId);

    mapper.findBy(
        criteria,
        [callbackCopy](const std::vector<AppUser>& users) {
          if (users.empty()) {
            auto resp = HttpResponse::newHttpJsonResponse(
                Json::Value("User not found"));
            resp->setStatusCode(k404NotFound);
            callbackCopy(resp);
            return;
          }

          const AppUser& user = users[0];

          Json::Value userJson;
          userJson["id"] = user.getValueOfId();

          userJson["display_name"] = user.getValueOfDisplayName();
          userJson["email"] = user.getValueOfEmail();

          userJson["created_at"] = user.getValueOfCreatedAt().toDbString();
          userJson["updated_at"] = user.getValueOfUpdatedAt().toDbString();

          userJson["is_verified"] = user.getValueOfIsVerified();

          auto resp = HttpResponse::newHttpJsonResponse(userJson);
          resp->setStatusCode(k200OK);
          callbackCopy(resp);
        },
        [callbackCopy](const drogon::orm::DrogonDbException& e) {
          LOG_ERROR << "DB error in me handler: " << e.base().what();
          auto resp = HttpResponse::newHttpJsonResponse(
              Json::Value("Internal server error"));
          resp->setStatusCode(k500InternalServerError);
          callbackCopy(resp);
        });

  } catch (const std::exception& e) {
    LOG_WARN << "me token verification failed: " << e.what();
    auto resp = HttpResponse::newHttpJsonResponse(
        Json::Value("Invalid or expired token"));
    resp->setStatusCode(k401Unauthorized);
    callback(resp);
    return;
  }
}

void AuthController::confirmEmail(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback) {
  std::string token = req->getParameter("token");
  if (token.empty()) {
    auto resp =
        HttpResponse::newHttpJsonResponse(Json::Value("Token is missing"));
    resp->setStatusCode(k400BadRequest);
    callback(resp);
    return;
  }

  auto dbClient = app().getDbClient();
  auto callbackCopy = std::move(callback);

  drogon::orm::Mapper<AppUser> mapper(dbClient);

  auto criteria =
      drogon::orm::Criteria(AppUser::Cols::_confirmation_token,
                            drogon::orm::CompareOperator::EQ, token);

  mapper.findBy(
      criteria,
      [callbackCopy, mapper](const std::vector<AppUser>& users) mutable {
        if (users.empty()) {
          auto resp = HttpResponse::newHttpJsonResponse(
              Json::Value("Invalid or expired token"));
          resp->setStatusCode(k400BadRequest);
          callbackCopy(resp);
          return;
        }

        AppUser user = users[0];

        user.setIsVerified(true);

        user.setConfirmationTokenToNull();

        mapper.update(
            user,
            [callbackCopy](const size_t count) {
              Json::Value respJson;
              respJson["success"] = true;
              respJson["message"] = "Email successfully verified";
              auto resp = HttpResponse::newHttpJsonResponse(respJson);
              resp->setStatusCode(k200OK);
              callbackCopy(resp);
            },
            [callbackCopy](const drogon::orm::DrogonDbException& e) {
              LOG_ERROR << "Database error during user update: "
                        << e.base().what();
              auto resp = HttpResponse::newHttpJsonResponse(
                  Json::Value("Internal server error"));
              resp->setStatusCode(k500InternalServerError);
              callbackCopy(resp);
            });
      },
      [callbackCopy](const drogon::orm::DrogonDbException& e) {
        LOG_ERROR << "Database error during findBy token: " << e.base().what();
        auto resp = HttpResponse::newHttpJsonResponse(
            Json::Value("Internal server error"));
        resp->setStatusCode(k500InternalServerError);
        callbackCopy(resp);
      });
}

void AuthController::forgotPassword(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback) {
  auto jsonPtr = req->getJsonObject();
  if (!jsonPtr || !jsonPtr->isObject() || !jsonPtr->isMember("email")) {
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Missing email"));
    resp->setStatusCode(k400BadRequest);
    return callback(resp);
  }

  std::string email = (*jsonPtr)["email"].asString();

  auto dbClient = app().getDbClient();
  auto callbackCopy = std::move(callback);

  drogon::orm::Mapper<AppUser> mapper(dbClient);
  auto criteria = drogon::orm::Criteria(
      AppUser::Cols::_email, drogon::orm::CompareOperator::EQ, email);

  mapper.findBy(
      criteria,
      [callbackCopy, email](const std::vector<AppUser>& users) {
        if (!users.empty()) {
          const AppUser& user = users[0];

          const char* envSecret = std::getenv("JWT_SECRET");
          const std::string secret = envSecret ? envSecret : "secret_key";

          auto now = std::chrono::system_clock::now();
          auto token =
              jwt::create()
                  .set_issued_at(now)
                  .set_expires_at(now + std::chrono::minutes(15))
                  .set_type("JWT")
                  .set_payload_claim("sub", jwt::claim(user.getValueOfId()))
                  .set_payload_claim("purpose",
                                     jwt::claim(std::string("password_reset")))
                  .sign(jwt::algorithm::hs256{secret});

          sendPasswordResetEmailLog(email, token);
        }

        auto resp = HttpResponse::newHttpJsonResponse(
            Json::Value("If an account with this email exists, a password "
                        "reset link has been sent."));
        resp->setStatusCode(k200OK);
        callbackCopy(resp);
      },
      [callbackCopy](const drogon::orm::DrogonDbException& e) {
        LOG_ERROR << "forgotPassword DB error: " << e.base().what();
        auto resp = HttpResponse::newHttpJsonResponse(
            Json::Value("Internal server error"));
        resp->setStatusCode(k500InternalServerError);
        callbackCopy(resp);
      });
}

void AuthController::resetPassword(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback) {
  auto jsonPtr = req->getJsonObject();
  if (!jsonPtr || !jsonPtr->isObject() || !jsonPtr->isMember("token") ||
      !jsonPtr->isMember("new_password")) {
    auto resp = HttpResponse::newHttpJsonResponse(
        Json::Value("Missing token or new_password"));
    resp->setStatusCode(k400BadRequest);
    return callback(resp);
  }

  std::string token = (*jsonPtr)["token"].asString();
  std::string newPassword = (*jsonPtr)["new_password"].asString();

  std::string errMsg;
  if (!isStrongPassword(newPassword, errMsg)) {
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value(errMsg));
    resp->setStatusCode(k400BadRequest);
    return callback(resp);
  }

  const char* envSecret = std::getenv("JWT_SECRET");
  const std::string secret = envSecret ? envSecret : "secret_key";

  std::string userId;
  try {
    auto decoded = jwt::decode(token);
    auto verifier =
        jwt::verify().allow_algorithm(jwt::algorithm::hs256{secret});
    verifier.verify(decoded);

    if (!decoded.has_payload_claim("purpose") ||
        decoded.get_payload_claim("purpose").as_string() != "password_reset") {
      throw std::runtime_error("Invalid token purpose");
    }

    userId = decoded.get_payload_claim("sub").as_string();

  } catch (const std::exception& e) {
    LOG_WARN << "resetPassword token verification failed: " << e.what();
    auto resp = HttpResponse::newHttpJsonResponse(
        Json::Value("Invalid or expired token"));
    resp->setStatusCode(k400BadRequest);
    return callback(resp);
  }

  auto dbClient = app().getDbClient();
  auto callbackCopy = std::move(callback);

  drogon::orm::Mapper<AppUser> mapper(dbClient);

  mapper.findByPrimaryKey(
      userId,
      [callbackCopy, newPassword, mapper](AppUser user) mutable {
        std::string newHash = bcrypt::generateHash(newPassword);
        user.setPasswordHash(newHash);
        user.setUpdatedAt(::trantor::Date::now());

        mapper.update(
            user,
            [callbackCopy](const size_t count) {
              auto resp = HttpResponse::newHttpJsonResponse(
                  Json::Value("Password successfully reset"));
              resp->setStatusCode(k200OK);
              callbackCopy(resp);
            },
            [callbackCopy](const drogon::orm::DrogonDbException& e) {
              LOG_ERROR << "Database error updating password: "
                        << e.base().what();
              auto resp = HttpResponse::newHttpJsonResponse(
                  Json::Value("Internal server error"));
              resp->setStatusCode(k500InternalServerError);
              callbackCopy(resp);
            });
      },
      [callbackCopy](const drogon::orm::DrogonDbException& e) {
        LOG_ERROR << "User not found or DB error in resetPassword: "
                  << e.base().what();
        auto resp = HttpResponse::newHttpJsonResponse(
            Json::Value("User not found or database error"));
        resp->setStatusCode(k400BadRequest);
        callbackCopy(resp);
      });
}