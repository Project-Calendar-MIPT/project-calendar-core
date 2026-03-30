#include "API/FeedController.h"
#include <drogon/HttpResponse.h>
#include <drogon/drogon.h>
#include <json/json.h>
#include <trantor/utils/Logger.h>
#include <algorithm>
#include <string>
using namespace drogon;

void FeedController::getRecommendedProjects(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback) {
  const std::string userId = req->attributes()->get<std::string>("user_id");
  if (userId.empty()) {
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Unauthorized"));
    resp->setStatusCode(k401Unauthorized);
    return callback(resp);
  }

  auto dbClient = app().getDbClient();
  try {
    auto rows = dbClient->execSqlSync(
        R"sql(
        SELECT t.id::text AS id,
               t.title,
               t.description,
               t.status::text AS status,
               t.priority::text AS priority,
               pv.visibility::text AS visibility,
               t.start_date::text AS start_date,
               t.due_date::text AS due_date,
               t.wanted_skills,
               COUNT(us.skill_key) AS match_score
        FROM task t
        JOIN project_visibility pv ON pv.project_id = t.id
        LEFT JOIN user_skill us
          ON us.user_id = $1::uuid
         AND us.skill_key = ANY(t.wanted_skills)
        WHERE t.parent_task_id IS NULL
          AND pv.visibility = 'public'
          AND NOT EXISTS (
            SELECT 1 FROM task_assignment ta
            WHERE ta.task_id = t.id AND ta.user_id = $1::uuid
          )
        GROUP BY t.id, t.title, t.description, t.status, t.priority,
                 pv.visibility, t.start_date, t.due_date, t.wanted_skills
        ORDER BY match_score DESC, t.created_at DESC
        LIMIT 20
        )sql",
        userId);

    Json::Value out(Json::arrayValue);
    for (const auto& row : rows) {
      Json::Value item(Json::objectValue);
      item["id"] = row["id"].isNull() ? Json::Value() : Json::Value(row["id"].as<std::string>());
      item["title"] = row["title"].isNull() ? Json::Value() : Json::Value(row["title"].as<std::string>());
      item["description"] = row["description"].isNull() ? Json::Value() : Json::Value(row["description"].as<std::string>());
      item["status"] = row["status"].isNull() ? Json::Value() : Json::Value(row["status"].as<std::string>());
      item["priority"] = row["priority"].isNull() ? Json::Value() : Json::Value(row["priority"].as<std::string>());
      item["visibility"] = row["visibility"].isNull() ? Json::Value() : Json::Value(row["visibility"].as<std::string>());
      item["start_date"] = row["start_date"].isNull() ? Json::Value() : Json::Value(row["start_date"].as<std::string>());
      item["due_date"] = row["due_date"].isNull() ? Json::Value() : Json::Value(row["due_date"].as<std::string>());
      item["match_score"] = row["match_score"].isNull() ? 0 : row["match_score"].as<int>();
      out.append(item);
    }
    auto resp = HttpResponse::newHttpJsonResponse(out);
    resp->setStatusCode(k200OK);
    return callback(resp);
  } catch (const std::exception& e) {
    LOG_ERROR << "getRecommendedProjects failed: " << e.what();
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Internal server error"));
    resp->setStatusCode(k500InternalServerError);
    return callback(resp);
  }
}

void FeedController::getFeed(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback) {
  const std::string userId = req->attributes()->get<std::string>("user_id");
  if (userId.empty()) {
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Unauthorized"));
    resp->setStatusCode(k401Unauthorized);
    return callback(resp);
  }

  long long limit = 20, offset = 0;
  const std::string limitParam = req->getParameter("limit");
  const std::string offsetParam = req->getParameter("offset");
  if (!limitParam.empty()) {
    try { long long v = std::stoll(limitParam); limit = (v < 1 ? 1 : (v > 100 ? 100 : v)); } catch (...) {}
  }
  if (!offsetParam.empty()) {
    try { long long v = std::stoll(offsetParam); offset = v > 0 ? v : 0LL; } catch (...) {}
  }

  auto dbClient = app().getDbClient();
  try {
    // Feed: recent task updates and new member joins in projects the user participates in
    const std::string sql = R"sql(
      SELECT event_type, object_id, object_title, detail, event_time, project_id, project_title
      FROM (
        SELECT 'task_updated' AS event_type,
               t.id::text AS object_id,
               t.title AS object_title,
               t.status::text AS detail,
               t.updated_at AS event_time,
               p.id::text AS project_id,
               p.title AS project_title
        FROM task t
        JOIN task p ON p.id = t.project_root_id
        JOIN task_assignment ta ON ta.task_id = p.id AND ta.user_id = $1::uuid
        WHERE t.updated_at > NOW() - INTERVAL '30 days'
          AND t.parent_task_id IS NOT NULL

        UNION ALL

        SELECT 'member_joined' AS event_type,
               u.id::text AS object_id,
               u.display_name AS object_title,
               tra.role::text AS detail,
               tra.assigned_at AS event_time,
               p.id::text AS project_id,
               p.title AS project_title
        FROM task_role_assignment tra
        JOIN app_user u ON u.id = tra.user_id
        JOIN task p ON p.id = tra.task_id AND p.parent_task_id IS NULL
        JOIN task_assignment my_ta ON my_ta.task_id = p.id AND my_ta.user_id = $1::uuid
        WHERE tra.assigned_at > NOW() - INTERVAL '30 days'
      ) feed_events
      ORDER BY event_time DESC
    )sql" + std::string(" LIMIT ") + std::to_string(limit) + " OFFSET " + std::to_string(offset);

    auto rows = dbClient->execSqlSync(sql, userId);

    Json::Value out(Json::arrayValue);
    for (const auto& row : rows) {
      Json::Value item(Json::objectValue);
      item["event_type"] = row["event_type"].isNull() ? Json::Value() : Json::Value(row["event_type"].as<std::string>());
      item["object_id"] = row["object_id"].isNull() ? Json::Value() : Json::Value(row["object_id"].as<std::string>());
      item["object_title"] = row["object_title"].isNull() ? Json::Value() : Json::Value(row["object_title"].as<std::string>());
      item["detail"] = row["detail"].isNull() ? Json::Value() : Json::Value(row["detail"].as<std::string>());
      item["event_time"] = row["event_time"].isNull() ? Json::Value() : Json::Value(row["event_time"].as<std::string>());
      item["project_id"] = row["project_id"].isNull() ? Json::Value() : Json::Value(row["project_id"].as<std::string>());
      item["project_title"] = row["project_title"].isNull() ? Json::Value() : Json::Value(row["project_title"].as<std::string>());
      out.append(item);
    }
    auto resp = HttpResponse::newHttpJsonResponse(out);
    resp->setStatusCode(k200OK);
    return callback(resp);
  } catch (const std::exception& e) {
    LOG_ERROR << "getFeed failed: " << e.what();
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Internal server error"));
    resp->setStatusCode(k500InternalServerError);
    return callback(resp);
  }
}

void FeedController::getDeadlines(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback) {
  const std::string userId = req->attributes()->get<std::string>("user_id");
  if (userId.empty()) {
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Unauthorized"));
    resp->setStatusCode(k401Unauthorized);
    return callback(resp);
  }

  auto dbClient = app().getDbClient();
  try {
    auto rows = dbClient->execSqlSync(
        R"sql(
        SELECT t.id::text AS id,
               t.title,
               t.description,
               t.status::text AS status,
               t.priority::text AS priority,
               t.due_date::text AS due_date,
               t.start_date::text AS start_date,
               t.project_root_id::text AS project_root_id,
               p.title AS project_title
        FROM task t
        JOIN task_assignment ta ON ta.task_id = t.id AND ta.user_id = $1::uuid
        LEFT JOIN task p ON p.id = t.project_root_id
        WHERE t.due_date IS NOT NULL
          AND t.due_date >= CURRENT_DATE
          AND t.due_date <= CURRENT_DATE + INTERVAL '7 days'
          AND t.status NOT IN ('completed', 'cancelled')
          AND t.parent_task_id IS NOT NULL
        ORDER BY t.due_date ASC
        )sql",
        userId);

    Json::Value out(Json::arrayValue);
    for (const auto& row : rows) {
      Json::Value item(Json::objectValue);
      item["id"] = row["id"].isNull() ? Json::Value() : Json::Value(row["id"].as<std::string>());
      item["title"] = row["title"].isNull() ? Json::Value() : Json::Value(row["title"].as<std::string>());
      item["description"] = row["description"].isNull() ? Json::Value() : Json::Value(row["description"].as<std::string>());
      item["status"] = row["status"].isNull() ? Json::Value() : Json::Value(row["status"].as<std::string>());
      item["priority"] = row["priority"].isNull() ? Json::Value() : Json::Value(row["priority"].as<std::string>());
      item["due_date"] = row["due_date"].isNull() ? Json::Value() : Json::Value(row["due_date"].as<std::string>());
      item["start_date"] = row["start_date"].isNull() ? Json::Value() : Json::Value(row["start_date"].as<std::string>());
      item["project_root_id"] = row["project_root_id"].isNull() ? Json::Value() : Json::Value(row["project_root_id"].as<std::string>());
      item["project_title"] = row["project_title"].isNull() ? Json::Value() : Json::Value(row["project_title"].as<std::string>());
      out.append(item);
    }
    auto resp = HttpResponse::newHttpJsonResponse(out);
    resp->setStatusCode(k200OK);
    return callback(resp);
  } catch (const std::exception& e) {
    LOG_ERROR << "getDeadlines failed: " << e.what();
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Internal server error"));
    resp->setStatusCode(k500InternalServerError);
    return callback(resp);
  }
}
