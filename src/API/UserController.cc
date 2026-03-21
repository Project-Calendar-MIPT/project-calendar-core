#include "UserController.h"

#include <bcrypt.h>
#include <drogon/HttpResponse.h>
#include <drogon/drogon.h>
#include <drogon/orm/Mapper.h>
#include <json/json.h>
#include <jwt-cpp/jwt.h>
#include <trantor/utils/Logger.h>

#include <any>
#include <regex>
#include <set>
#include <vector>

#include "../models/AppUser.h"
#include "../models/UserWorkSchedule.h"

using namespace drogon;
using drogon_model::project_calendar::AppUser;
using drogon_model::project_calendar::UserWorkSchedule;

static bool isValidTime(const std::string& t) {
  static const std::regex re("^([01]\\d|2[0-3]):([0-5]\\d)$");
  return std::regex_match(t, re);
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

static void sendEmailChangeLog(const std::string& email,
                               const std::string& token) {
  std::string confirmUrl =
      "http://localhost:8080/api/users/email/confirm?token=" + token;
  LOG_INFO << "=================================================";
  LOG_INFO << "ИМИТАЦИЯ ОТПРАВКИ EMAIL (СМЕНА ПОЧТЫ):";
  LOG_INFO << "Кому: " << email;
  LOG_INFO << "Ссылка для подтверждения: " << confirmUrl;
  LOG_INFO << "=================================================";
}

void UsersController::getSchedule(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback) {
  std::string userId;
  try {
    userId = req->attributes()->get<std::string>("user_id");
  } catch (...) {
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Unauthorized"));
    resp->setStatusCode(k401Unauthorized);
    return callback(resp);
  }

  auto dbClient = app().getDbClient();
  try {
    drogon::orm::Mapper<drogon_model::project_calendar::UserWorkSchedule>
        wsMapper(dbClient);

    wsMapper.orderBy(
        drogon_model::project_calendar::UserWorkSchedule::Cols::_weekday,
        drogon::orm::SortOrder::ASC);

    auto schedules = wsMapper.findBy(drogon::orm::Criteria(
        drogon_model::project_calendar::UserWorkSchedule::Cols::_user_id,
        drogon::orm::CompareOperator::EQ, userId));

    Json::Value outArr(Json::arrayValue);
    Json::Reader reader;

    for (const auto& ws : schedules) {
      Json::Value item;
      item["day_of_week"] = ws.getValueOfWeekday();

      std::string slotsStr = ws.getValueOfTimeSlots();
      Json::Value slotsJson;
      if (reader.parse(slotsStr, slotsJson)) {
        item["time_slots"] = slotsJson;
      } else {
        item["time_slots"] = Json::arrayValue;
      }

      outArr.append(item);
    }

    auto resp = HttpResponse::newHttpJsonResponse(outArr);
    resp->setStatusCode(k200OK);
    callback(resp);

  } catch (const std::exception& e) {
    LOG_ERROR << "getSchedule failed for user " << userId << ": " << e.what();
    auto resp =
        HttpResponse::newHttpJsonResponse(Json::Value("Internal server error"));
    resp->setStatusCode(k500InternalServerError);
    callback(resp);
  }
}

void UsersController::updateSchedule(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback) {
  std::string userId;
  try {
    userId = req->attributes()->get<std::string>("user_id");
  } catch (...) {
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Unauthorized"));
    resp->setStatusCode(k401Unauthorized);
    return callback(resp);
  }

  auto pj = req->getJsonObject();
  if (!pj || !pj->isArray() || pj->size() != 7) {
    auto resp = HttpResponse::newHttpJsonResponse(
        Json::Value("Invalid JSON: expected array of exactly 7 items"));
    resp->setStatusCode(k400BadRequest);
    return callback(resp);
  }

  const Json::Value& arr = *pj;
  std::set<int> daysSet;

  for (Json::UInt i = 0; i < arr.size(); ++i) {
    const Json::Value& el = arr[i];
    if (!el.isObject() || !el.isMember("day_of_week") ||
        !el.isMember("time_slots")) {
      auto resp = HttpResponse::newHttpJsonResponse(
          Json::Value("Each element must have 'day_of_week' and 'time_slots'"));
      resp->setStatusCode(k400BadRequest);
      return callback(resp);
    }
    int dow = el["day_of_week"].asInt();
    if (dow < 1 || dow > 7) {
      auto resp = HttpResponse::newHttpJsonResponse(
          Json::Value("day_of_week must be in range 1..7"));
      resp->setStatusCode(k400BadRequest);
      return callback(resp);
    }
    if (daysSet.find(dow) != daysSet.end()) {
      auto resp = HttpResponse::newHttpJsonResponse(
          Json::Value("Duplicate day_of_week values"));
      resp->setStatusCode(k400BadRequest);
      return callback(resp);
    }
    daysSet.insert(dow);

    if (!el["time_slots"].isArray()) {
      auto resp = HttpResponse::newHttpJsonResponse(
          Json::Value("time_slots must be an array"));
      resp->setStatusCode(k400BadRequest);
      return callback(resp);
    }
  }

  auto dbClient = app().getDbClient();
  try {
    auto trans = dbClient->newTransaction();

    drogon::orm::Mapper<drogon_model::project_calendar::UserWorkSchedule>
        transWsMapper(trans);

    transWsMapper.deleteBy(drogon::orm::Criteria(
        drogon_model::project_calendar::UserWorkSchedule::Cols::_user_id,
        drogon::orm::CompareOperator::EQ, userId));

    Json::FastWriter writer;
    Json::Value createdArr(Json::arrayValue);

    for (Json::UInt i = 0; i < arr.size(); ++i) {
      const Json::Value& el = arr[i];

      drogon_model::project_calendar::UserWorkSchedule ws;
      ws.setUserId(userId);
      ws.setWeekday(static_cast<int32_t>(el["day_of_week"].asInt()));

      std::string slotsStr = writer.write(el["time_slots"]);
      ws.setTimeSlots(slotsStr);

      transWsMapper.insert(ws);
      createdArr.append(el);
    }

    auto resp = HttpResponse::newHttpJsonResponse(createdArr);
    resp->setStatusCode(k200OK);
    callback(resp);

  } catch (const std::exception& e) {
    LOG_ERROR << "updateSchedule failed for user " << userId << ": "
              << e.what();
    auto resp =
        HttpResponse::newHttpJsonResponse(Json::Value("Internal server error"));
    resp->setStatusCode(k500InternalServerError);
    callback(resp);
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

    if (j.isMember("experience_level")) {
      user.setExperienceLevel(j["experience_level"].asString());
    }

    if (j.isMember("skills") && j["skills"].isArray()) {
      Json::FastWriter writer;
      user.setSkills(writer.write(j["skills"]));
    }

    user.setUpdatedAt(::trantor::Date::now());

    userMapper.update(user);

    if (j.isMember("work_schedule")) {
      const Json::Value& workScheduleJson = j["work_schedule"];

      if (!workScheduleJson.isArray() || workScheduleJson.size() != 7) {
        trans->rollback();
        auto resp = HttpResponse::newHttpJsonResponse(
            Json::Value("work_schedule must be an array of exactly 7 items"));
        resp->setStatusCode(k400BadRequest);
        return callback(resp);
      }

      wsMapper.deleteBy(drogon::orm::Criteria(UserWorkSchedule::Cols::_user_id,
                                              drogon::orm::CompareOperator::EQ,
                                              userId));

      Json::FastWriter writer;
      for (Json::UInt i = 0; i < workScheduleJson.size(); ++i) {
        const Json::Value item = workScheduleJson[i];

        if (!item.isObject() || !item.isMember("day_of_week") ||
            !item.isMember("time_slots") || !item["time_slots"].isArray()) {
          trans->rollback();
          auto resp = HttpResponse::newHttpJsonResponse(
              Json::Value("Invalid work_schedule item structure"));
          resp->setStatusCode(k400BadRequest);
          return callback(resp);
        }

        UserWorkSchedule ws;
        ws.setUserId(userId);
        ws.setWeekday(static_cast<int32_t>(item["day_of_week"].asInt()));
        ws.setTimeSlots(writer.write(item["time_slots"]));
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

void UsersController::uploadAvatar(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback) {
  std::string userId;
  try {
    userId = req->attributes()->get<std::string>("user_id");
  } catch (...) {
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Unauthorized"));
    resp->setStatusCode(k401Unauthorized);
    return callback(resp);
  }

  drogon::MultiPartParser fileUpload;
  if (fileUpload.parse(req) != 0 || fileUpload.getFiles().empty()) {
    auto resp =
        HttpResponse::newHttpJsonResponse(Json::Value("No file uploaded"));
    resp->setStatusCode(k400BadRequest);
    return callback(resp);
  }

  auto& file = fileUpload.getFiles()[0];

  std::string ext = std::string(file.getFileExtension());
  if (ext != "jpg" && ext != "jpeg" && ext != "png" && ext != "webp") {
    auto resp =
        HttpResponse::newHttpJsonResponse(Json::Value("Invalid file format"));
    resp->setStatusCode(k400BadRequest);
    return callback(resp);
  }

  std::string fileName = drogon::utils::getUuid() + "." + ext;

  file.saveAs("./uploads/avatars/" + fileName);

  std::string avatarUrl = "/uploads/avatars/" + fileName;

  auto dbClient = app().getDbClient();
  try {
    drogon::orm::Mapper<AppUser> userMapper(dbClient);
    AppUser user = userMapper.findByPrimaryKey(userId);

    user.setAvatarUrl(avatarUrl);
    user.setUpdatedAt(::trantor::Date::now());

    userMapper.update(user);

    Json::Value out;
    out["message"] = "Avatar uploaded successfully";
    out["avatar_url"] = avatarUrl;

    auto resp = HttpResponse::newHttpJsonResponse(out);
    resp->setStatusCode(k200OK);
    callback(resp);

  } catch (const drogon::orm::UnexpectedRows& e) {
    auto resp =
        HttpResponse::newHttpJsonResponse(Json::Value("User not found"));
    resp->setStatusCode(k404NotFound);
    callback(resp);
  } catch (const std::exception& e) {
    LOG_ERROR << "Avatar upload failed: " << e.what();
    auto resp =
        HttpResponse::newHttpJsonResponse(Json::Value("Internal server error"));
    resp->setStatusCode(k500InternalServerError);
    callback(resp);
  }
}

void UsersController::changePassword(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback) {
  std::string userId;
  try {
    userId = req->attributes()->get<std::string>("user_id");
  } catch (...) {
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Unauthorized"));
    resp->setStatusCode(k401Unauthorized);
    return callback(resp);
  }

  auto jsonPtr = req->getJsonObject();
  if (!jsonPtr || !jsonPtr->isObject() || !jsonPtr->isMember("old_password") ||
      !jsonPtr->isMember("new_password")) {
    auto resp = HttpResponse::newHttpJsonResponse(
        Json::Value("Missing old_password or new_password"));
    resp->setStatusCode(k400BadRequest);
    return callback(resp);
  }

  std::string oldPassword = (*jsonPtr)["old_password"].asString();
  std::string newPassword = (*jsonPtr)["new_password"].asString();

  std::string errMsg;
  if (!isStrongPassword(newPassword, errMsg)) {
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value(errMsg));
    resp->setStatusCode(k400BadRequest);
    return callback(resp);
  }

  auto dbClient = app().getDbClient();
  try {
    drogon::orm::Mapper<AppUser> userMapper(dbClient);
    AppUser user = userMapper.findByPrimaryKey(userId);

    if (!bcrypt::validatePassword(oldPassword, user.getValueOfPasswordHash())) {
      auto resp = HttpResponse::newHttpJsonResponse(
          Json::Value("Incorrect old password"));
      resp->setStatusCode(k403Forbidden);
      return callback(resp);
    }

    std::string newHash = bcrypt::generateHash(newPassword);
    user.setPasswordHash(newHash);
    user.setUpdatedAt(::trantor::Date::now());

    userMapper.update(user);

    auto resp = HttpResponse::newHttpJsonResponse(
        Json::Value("Password updated successfully"));
    resp->setStatusCode(k200OK);
    callback(resp);

  } catch (const std::exception& e) {
    LOG_ERROR << "changePassword failed: " << e.what();
    auto resp =
        HttpResponse::newHttpJsonResponse(Json::Value("Internal server error"));
    resp->setStatusCode(k500InternalServerError);
    callback(resp);
  }
}

void UsersController::requestEmailChange(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback) {
  std::string userId;
  try {
    userId = req->attributes()->get<std::string>("user_id");
  } catch (...) {
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Unauthorized"));
    resp->setStatusCode(k401Unauthorized);
    return callback(resp);
  }

  auto jsonPtr = req->getJsonObject();
  if (!jsonPtr || !jsonPtr->isMember("new_email")) {
    auto resp =
        HttpResponse::newHttpJsonResponse(Json::Value("Missing new_email"));
    resp->setStatusCode(k400BadRequest);
    return callback(resp);
  }

  std::string newEmail = (*jsonPtr)["new_email"].asString();

  std::string errMsg;
  if (!isValidEmail(newEmail, errMsg)) {
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value(errMsg));
    resp->setStatusCode(k400BadRequest);
    return callback(resp);
  }

  auto dbClient = app().getDbClient();
  try {
    drogon::orm::Mapper<AppUser> userMapper(dbClient);

    auto criteria = drogon::orm::Criteria(
        AppUser::Cols::_email, drogon::orm::CompareOperator::EQ, newEmail);
    if (userMapper.count(criteria) > 0) {
      auto resp = HttpResponse::newHttpJsonResponse(
          Json::Value("Email is already taken"));
      resp->setStatusCode(k409Conflict);
      return callback(resp);
    }

    const char* envSecret = std::getenv("JWT_SECRET");
    const std::string secret = envSecret ? envSecret : "secret_key";

    auto now = std::chrono::system_clock::now();
    auto token = jwt::create()
                     .set_issued_at(now)
                     .set_expires_at(now + std::chrono::hours(1))
                     .set_type("JWT")
                     .set_payload_claim("sub", jwt::claim(userId))
                     .set_payload_claim("new_email", jwt::claim(newEmail))
                     .sign(jwt::algorithm::hs256{secret});

    sendEmailChangeLog(newEmail, token);

    auto resp = HttpResponse::newHttpJsonResponse(
        Json::Value("Confirmation link sent to new email"));
    resp->setStatusCode(k200OK);
    callback(resp);

  } catch (const std::exception& e) {
    LOG_ERROR << "requestEmailChange failed: " << e.what();
    auto resp =
        HttpResponse::newHttpJsonResponse(Json::Value("Internal server error"));
    resp->setStatusCode(k500InternalServerError);
    callback(resp);
  }
}

void UsersController::confirmEmailChange(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback) {
  std::string token = req->getParameter("token");
  if (token.empty()) {
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Missing token"));
    resp->setStatusCode(k400BadRequest);
    return callback(resp);
  }

  const char* envSecret = std::getenv("JWT_SECRET");
  const std::string secret = envSecret ? envSecret : "secret_key";

  try {
    auto decoded = jwt::decode(token);
    auto verifier =
        jwt::verify().allow_algorithm(jwt::algorithm::hs256{secret});
    verifier.verify(decoded);

    std::string userId = decoded.get_payload_claim("sub").as_string();
    std::string newEmail = decoded.get_payload_claim("new_email").as_string();

    auto dbClient = app().getDbClient();
    drogon::orm::Mapper<AppUser> userMapper(dbClient);
    AppUser user = userMapper.findByPrimaryKey(userId);

    user.setEmail(newEmail);
    user.setUpdatedAt(::trantor::Date::now());
    userMapper.update(user);

    auto resp = HttpResponse::newHttpJsonResponse(
        Json::Value("Email successfully updated!"));
    resp->setStatusCode(k200OK);
    callback(resp);

  } catch (const std::exception& e) {
    LOG_ERROR << "confirmEmailChange failed: " << e.what();
    auto resp = HttpResponse::newHttpJsonResponse(
        Json::Value("Invalid or expired token"));
    resp->setStatusCode(k400BadRequest);
    callback(resp);
  }
}