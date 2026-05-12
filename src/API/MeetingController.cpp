#include "API/MeetingController.h"
#include <drogon/HttpResponse.h>
#include <drogon/drogon.h>
#include <json/json.h>
#include <trantor/utils/Logger.h>
#include <string>

using namespace drogon;

static std::string meetingIdFromPath(const HttpRequestPtr& req) {
  std::string id = req->getParameter("id");
  if (!id.empty()) return id;
  std::string p = req->path();
  auto s = p.find("/meetings/");
  if (s == std::string::npos) return {};
  s += 10;
  auto e = p.find("/", s);
  return e == std::string::npos ? p.substr(s) : p.substr(s, e - s);
}

void MeetingController::createMeeting(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback) {
  auto attrsPtr = req->attributes();
  if (!attrsPtr || !attrsPtr->find("user_id")) {
    auto r = HttpResponse::newHttpJsonResponse(Json::Value("Unauthorized"));
    r->setStatusCode(k401Unauthorized); return callback(r);
  }
  const std::string userId = attrsPtr->get<std::string>("user_id");
  auto body = req->getJsonObject();
  if (!body) {
    auto r = HttpResponse::newHttpJsonResponse(Json::Value("Invalid JSON"));
    r->setStatusCode(k400BadRequest); return callback(r);
  }
  const Json::Value& j = *body;
  if (!j.isMember("title") || j["title"].asString().empty()) {
    auto r = HttpResponse::newHttpJsonResponse(Json::Value("title required"));
    r->setStatusCode(k400BadRequest); return callback(r);
  }
  if (!j.isMember("start_at") || j["start_at"].asString().empty()) {
    auto r = HttpResponse::newHttpJsonResponse(Json::Value("start_at required"));
    r->setStatusCode(k400BadRequest); return callback(r);
  }
  if (!j.isMember("participant_ids") || !j["participant_ids"].isArray()
      || j["participant_ids"].empty()) {
    auto r = HttpResponse::newHttpJsonResponse(Json::Value("participant_ids required"));
    r->setStatusCode(k400BadRequest); return callback(r);
  }

  const std::string title       = j["title"].asString();
  const std::string description = j.isMember("description") ? j["description"].asString() : "";
  const std::string startAt     = j["start_at"].asString();
  const int durationMin         = j.isMember("duration_min") ? j["duration_min"].asInt() : 60;
  const std::string meetingUrl  = (j.isMember("meeting_url") && !j["meeting_url"].isNull())  ? j["meeting_url"].asString()  : "";
  const std::string location    = (j.isMember("location")    && !j["location"].isNull())     ? j["location"].asString()     : "";

  auto db = drogon::app().getDbClient();
  try {
    // Validate all participants are subordinates of organizer
    for (const auto& pid : j["participant_ids"]) {
      const std::string participantId = pid.asString();
      auto chk = db->execSqlSync(
        R"sql(
          SELECT EXISTS (
            SELECT 1 FROM task_role_assignment ra1
            JOIN task_role_assignment ra2 ON ra1.task_id = ra2.task_id
            WHERE ra1.user_id = $1::uuid AND ra1.role IN ('owner','supervisor')
              AND ra2.user_id = $2::uuid AND ra2.role = 'executor'
          ) AS ok
        )sql", userId, participantId);
      if (!chk[0]["ok"].as<bool>()) {
        Json::Value err;
        err["error"]   = "not_subordinate";
        err["user_id"] = participantId;
        auto r = HttpResponse::newHttpJsonResponse(err);
        r->setStatusCode(k403Forbidden); return callback(r);
      }
    }

    // Insert meeting
    auto ins = db->execSqlSync(
      R"sql(
        INSERT INTO meeting (title, description, organizer_id, start_at,
                             duration_min, meeting_url, location)
        VALUES ($1, $2, $3::uuid, $4::timestamptz, $5, NULLIF($6,''), NULLIF($7,''))
        RETURNING id::text, title, description, organizer_id::text,
                  (start_at AT TIME ZONE 'UTC')::text AS start_at,
                  duration_min, meeting_url, location,
                  (created_at AT TIME ZONE 'UTC')::text AS created_at
      )sql",
      title, description, userId, startAt,
      std::to_string(durationMin), meetingUrl, location);

    const std::string meetingId = ins[0]["id"].as<std::string>();

    // Add organizer as participant
    db->execSqlSync(
      "INSERT INTO meeting_participant (meeting_id, user_id) VALUES ($1::uuid, $2::uuid) "
      "ON CONFLICT DO NOTHING",
      meetingId, userId);

    // Add all participants
    for (const auto& pid : j["participant_ids"]) {
      db->execSqlSync(
        "INSERT INTO meeting_participant (meeting_id, user_id) VALUES ($1::uuid, $2::uuid) "
        "ON CONFLICT DO NOTHING",
        meetingId, pid.asString());
    }

    Json::Value resp_j;
    resp_j["id"]           = meetingId;
    resp_j["title"]        = ins[0]["title"].as<std::string>();
    resp_j["description"]  = ins[0]["description"].isNull() ? "" : ins[0]["description"].as<std::string>();
    resp_j["organizer_id"] = ins[0]["organizer_id"].as<std::string>();
    resp_j["start_at"]     = ins[0]["start_at"].as<std::string>();
    resp_j["duration_min"] = ins[0]["duration_min"].as<int>();
    resp_j["meeting_url"]  = ins[0]["meeting_url"].isNull()  ? "" : ins[0]["meeting_url"].as<std::string>();
    resp_j["location"]     = ins[0]["location"].isNull()     ? "" : ins[0]["location"].as<std::string>();
    resp_j["created_at"]   = ins[0]["created_at"].as<std::string>();
    resp_j["is_organizer"] = true;

    Json::Value parts(Json::arrayValue);
    parts.append(userId);
    for (const auto& pid : j["participant_ids"]) parts.append(pid);
    resp_j["participant_ids"] = parts;

    auto r = HttpResponse::newHttpJsonResponse(resp_j);
    r->setStatusCode(k201Created); callback(r);
  } catch (const std::exception& e) {
    LOG_ERROR << "createMeeting: " << e.what();
    auto r = HttpResponse::newHttpJsonResponse(Json::Value("Internal server error"));
    r->setStatusCode(k500InternalServerError); callback(r);
  }
}

void MeetingController::listMeetings(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback) {
  auto attrsPtr = req->attributes();
  if (!attrsPtr || !attrsPtr->find("user_id")) {
    auto r = HttpResponse::newHttpJsonResponse(Json::Value("Unauthorized"));
    r->setStatusCode(k401Unauthorized); return callback(r);
  }
  const std::string userId   = attrsPtr->get<std::string>("user_id");
  const std::string upcoming = req->getParameter("upcoming");
  auto db = drogon::app().getDbClient();
  try {
    std::string timeFilter = (upcoming == "true") ? "AND m.start_at >= NOW() " : "";
    auto res = db->execSqlSync(
      "SELECT m.id::text, m.title, m.description, m.organizer_id::text, "
      "       (m.start_at AT TIME ZONE 'UTC')::text AS start_at, "
      "       m.duration_min, m.meeting_url, m.location, "
      "       (m.created_at AT TIME ZONE 'UTC')::text AS created_at, "
      "       ARRAY_AGG(mp2.user_id::text) AS participant_ids "
      "FROM meeting m "
      "JOIN meeting_participant mp ON mp.meeting_id = m.id AND mp.user_id = $1::uuid "
      "JOIN meeting_participant mp2 ON mp2.meeting_id = m.id "
      + timeFilter +
      "GROUP BY m.id, m.title, m.description, m.organizer_id, m.start_at, "
      "         m.duration_min, m.meeting_url, m.location, m.created_at "
      "ORDER BY m.start_at ASC",
      userId);

    Json::Value arr(Json::arrayValue);
    for (const auto& row : res) {
      Json::Value item;
      item["id"]           = row["id"].as<std::string>();
      item["title"]        = row["title"].as<std::string>();
      item["description"]  = row["description"].isNull() ? "" : row["description"].as<std::string>();
      item["organizer_id"] = row["organizer_id"].as<std::string>();
      item["start_at"]     = row["start_at"].as<std::string>();
      item["duration_min"] = row["duration_min"].as<int>();
      item["meeting_url"]  = row["meeting_url"].isNull()  ? "" : row["meeting_url"].as<std::string>();
      item["location"]     = row["location"].isNull()     ? "" : row["location"].as<std::string>();
      item["created_at"]   = row["created_at"].as<std::string>();
      item["is_organizer"] = (row["organizer_id"].as<std::string>() == userId);
      arr.append(item);
    }
    auto r = HttpResponse::newHttpJsonResponse(arr);
    r->setStatusCode(k200OK); callback(r);
  } catch (const std::exception& e) {
    LOG_ERROR << "listMeetings: " << e.what();
    auto r = HttpResponse::newHttpJsonResponse(Json::Value("Internal server error"));
    r->setStatusCode(k500InternalServerError); callback(r);
  }
}

void MeetingController::getMeeting(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback) {
  auto attrsPtr = req->attributes();
  if (!attrsPtr || !attrsPtr->find("user_id")) {
    auto r = HttpResponse::newHttpJsonResponse(Json::Value("Unauthorized"));
    r->setStatusCode(k401Unauthorized); return callback(r);
  }
  const std::string userId    = attrsPtr->get<std::string>("user_id");
  const std::string meetingId = meetingIdFromPath(req);
  auto db = drogon::app().getDbClient();
  try {
    auto res = db->execSqlSync(
      R"sql(
        SELECT m.id::text, m.title, m.description, m.organizer_id::text,
               (m.start_at AT TIME ZONE 'UTC')::text AS start_at,
               m.duration_min, m.meeting_url, m.location,
               (m.created_at AT TIME ZONE 'UTC')::text AS created_at
        FROM meeting m
        JOIN meeting_participant mp ON mp.meeting_id = m.id AND mp.user_id = $2::uuid
        WHERE m.id = $1::uuid
      )sql", meetingId, userId);
    if (res.empty()) {
      auto r = HttpResponse::newHttpJsonResponse(Json::Value("Not found"));
      r->setStatusCode(k404NotFound); return callback(r);
    }
    Json::Value item;
    item["id"]           = res[0]["id"].as<std::string>();
    item["title"]        = res[0]["title"].as<std::string>();
    item["description"]  = res[0]["description"].isNull() ? "" : res[0]["description"].as<std::string>();
    item["organizer_id"] = res[0]["organizer_id"].as<std::string>();
    item["start_at"]     = res[0]["start_at"].as<std::string>();
    item["duration_min"] = res[0]["duration_min"].as<int>();
    item["meeting_url"]  = res[0]["meeting_url"].isNull()  ? "" : res[0]["meeting_url"].as<std::string>();
    item["location"]     = res[0]["location"].isNull()     ? "" : res[0]["location"].as<std::string>();
    item["is_organizer"] = (res[0]["organizer_id"].as<std::string>() == userId);
    auto r = HttpResponse::newHttpJsonResponse(item);
    r->setStatusCode(k200OK); callback(r);
  } catch (const std::exception& e) {
    LOG_ERROR << "getMeeting: " << e.what();
    auto r = HttpResponse::newHttpJsonResponse(Json::Value("Internal server error"));
    r->setStatusCode(k500InternalServerError); callback(r);
  }
}

void MeetingController::deleteMeeting(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback) {
  auto attrsPtr = req->attributes();
  if (!attrsPtr || !attrsPtr->find("user_id")) {
    auto r = HttpResponse::newHttpJsonResponse(Json::Value("Unauthorized"));
    r->setStatusCode(k401Unauthorized); return callback(r);
  }
  const std::string userId    = attrsPtr->get<std::string>("user_id");
  const std::string meetingId = meetingIdFromPath(req);
  auto db = drogon::app().getDbClient();
  try {
    auto chk = db->execSqlSync(
      "SELECT organizer_id::text FROM meeting WHERE id = $1::uuid", meetingId);
    if (chk.empty()) {
      auto r = HttpResponse::newHttpJsonResponse(Json::Value("Not found"));
      r->setStatusCode(k404NotFound); return callback(r);
    }
    if (chk[0]["organizer_id"].as<std::string>() != userId) {
      auto r = HttpResponse::newHttpJsonResponse(Json::Value("Forbidden"));
      r->setStatusCode(k403Forbidden); return callback(r);
    }
    db->execSqlSync("DELETE FROM meeting WHERE id = $1::uuid", meetingId);
    auto r = HttpResponse::newHttpJsonResponse(Json::Value("Deleted"));
    r->setStatusCode(k200OK); callback(r);
  } catch (const std::exception& e) {
    LOG_ERROR << "deleteMeeting: " << e.what();
    auto r = HttpResponse::newHttpJsonResponse(Json::Value("Internal server error"));
    r->setStatusCode(k500InternalServerError); callback(r);
  }
}
