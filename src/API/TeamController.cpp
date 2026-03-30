#include "API/TeamController.h"
#include <drogon/HttpResponse.h>
#include <drogon/drogon.h>
#include <json/json.h>
#include <trantor/utils/Logger.h>
#include <string>
using namespace drogon;

void TeamController::createTeam(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback) {
  const std::string userId = req->attributes()->get<std::string>("user_id");
  if (userId.empty()) {
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Unauthorized"));
    resp->setStatusCode(k401Unauthorized);
    return callback(resp);
  }

  auto body = req->getJsonObject();
  if (!body || !(*body)["name"].isString() || (*body)["name"].asString().empty()) {
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("name is required"));
    resp->setStatusCode(k400BadRequest);
    return callback(resp);
  }

  const std::string name = (*body)["name"].asString();
  const std::string description =
      (*body)["description"].isString() ? (*body)["description"].asString() : "";
  const std::string parentTeamId =
      (*body)["parent_team_id"].isString() ? (*body)["parent_team_id"].asString() : "";

  auto dbClient = app().getDbClient();
  try {
    // Use NULLIF to handle optional parent_team_id in a single query
    auto rows = dbClient->execSqlSync(
        "INSERT INTO team (name, description, owner_user_id, parent_team_id) "
        "VALUES ($1, $2, $3::uuid, NULLIF($4, '')::uuid) "
        "RETURNING id::text, name, description, owner_user_id::text, "
        "parent_team_id::text, created_at::text",
        name, description, userId, parentTeamId);
    if (rows.empty()) {
      auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Failed to create team"));
      resp->setStatusCode(k500InternalServerError);
      return callback(resp);
    }
    const auto& row = rows[0];
    // Add creator as a member
    dbClient->execSqlSync(
        "INSERT INTO team_member (team_id, user_id) VALUES ($1::uuid, $2::uuid) "
        "ON CONFLICT DO NOTHING",
        row["id"].as<std::string>(), userId);

    Json::Value out(Json::objectValue);
    out["id"] = row["id"].as<std::string>();
    out["name"] = row["name"].as<std::string>();
    out["description"] = row["description"].isNull() ? Json::Value() : Json::Value(row["description"].as<std::string>());
    out["owner_user_id"] = row["owner_user_id"].isNull() ? Json::Value() : Json::Value(row["owner_user_id"].as<std::string>());
    out["parent_team_id"] = row["parent_team_id"].isNull() ? Json::Value() : Json::Value(row["parent_team_id"].as<std::string>());
    out["created_at"] = row["created_at"].isNull() ? Json::Value() : Json::Value(row["created_at"].as<std::string>());

    auto resp = HttpResponse::newHttpJsonResponse(out);
    resp->setStatusCode(k201Created);
    return callback(resp);
  } catch (const std::exception& e) {
    LOG_ERROR << "createTeam failed: " << e.what();
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Internal server error"));
    resp->setStatusCode(k500InternalServerError);
    return callback(resp);
  }
}

void TeamController::getTeam(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback,
    std::string team_id) {
  const std::string userId = req->attributes()->get<std::string>("user_id");
  if (userId.empty()) {
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Unauthorized"));
    resp->setStatusCode(k401Unauthorized);
    return callback(resp);
  }

  auto dbClient = app().getDbClient();
  try {
    auto rows = dbClient->execSqlSync(
        "SELECT id::text, name, description, owner_user_id::text, "
        "parent_team_id::text, created_at::text "
        "FROM team WHERE id = $1::uuid",
        team_id);
    if (rows.empty()) {
      auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Team not found"));
      resp->setStatusCode(k404NotFound);
      return callback(resp);
    }
    const auto& row = rows[0];

    Json::Value out(Json::objectValue);
    out["id"] = row["id"].as<std::string>();
    out["name"] = row["name"].as<std::string>();
    out["description"] = row["description"].isNull() ? Json::Value() : Json::Value(row["description"].as<std::string>());
    out["owner_user_id"] = row["owner_user_id"].isNull() ? Json::Value() : Json::Value(row["owner_user_id"].as<std::string>());
    out["parent_team_id"] = row["parent_team_id"].isNull() ? Json::Value() : Json::Value(row["parent_team_id"].as<std::string>());
    out["created_at"] = row["created_at"].isNull() ? Json::Value() : Json::Value(row["created_at"].as<std::string>());

    // Include members
    auto memberRows = dbClient->execSqlSync(
        "SELECT u.id::text, u.display_name, u.email "
        "FROM team_member tm "
        "JOIN app_user u ON u.id = tm.user_id "
        "WHERE tm.team_id = $1::uuid "
        "ORDER BY u.display_name",
        team_id);

    Json::Value members(Json::arrayValue);
    for (const auto& mr : memberRows) {
      Json::Value m(Json::objectValue);
      m["id"] = mr["id"].isNull() ? Json::Value() : Json::Value(mr["id"].as<std::string>());
      m["display_name"] = mr["display_name"].isNull() ? Json::Value() : Json::Value(mr["display_name"].as<std::string>());
      m["email"] = mr["email"].isNull() ? Json::Value() : Json::Value(mr["email"].as<std::string>());
      members.append(m);
    }
    out["members"] = members;

    auto resp = HttpResponse::newHttpJsonResponse(out);
    resp->setStatusCode(k200OK);
    return callback(resp);
  } catch (const std::exception& e) {
    LOG_ERROR << "getTeam failed: " << e.what();
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Internal server error"));
    resp->setStatusCode(k500InternalServerError);
    return callback(resp);
  }
}

void TeamController::updateTeam(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback,
    std::string team_id) {
  const std::string userId = req->attributes()->get<std::string>("user_id");
  if (userId.empty()) {
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Unauthorized"));
    resp->setStatusCode(k401Unauthorized);
    return callback(resp);
  }

  auto dbClient = app().getDbClient();
  try {
    // Check ownership
    auto check = dbClient->execSqlSync(
        "SELECT owner_user_id::text FROM team WHERE id = $1::uuid", team_id);
    if (check.empty()) {
      auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Team not found"));
      resp->setStatusCode(k404NotFound);
      return callback(resp);
    }
    if (check[0]["owner_user_id"].as<std::string>() != userId) {
      auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Insufficient permissions"));
      resp->setStatusCode(k403Forbidden);
      return callback(resp);
    }

    auto body = req->getJsonObject();
    if (!body) {
      auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Request body required"));
      resp->setStatusCode(k400BadRequest);
      return callback(resp);
    }

    if ((*body)["name"].isString() && !(*body)["name"].asString().empty()) {
      dbClient->execSqlSync(
          "UPDATE team SET name = $1 WHERE id = $2::uuid",
          (*body)["name"].asString(), team_id);
    }
    if ((*body)["description"].isString()) {
      dbClient->execSqlSync(
          "UPDATE team SET description = $1 WHERE id = $2::uuid",
          (*body)["description"].asString(), team_id);
    }
    if ((*body)["parent_team_id"].isString()) {
      const std::string ptid = (*body)["parent_team_id"].asString();
      if (ptid.empty()) {
        dbClient->execSqlSync(
            "UPDATE team SET parent_team_id = NULL WHERE id = $1::uuid", team_id);
      } else {
        dbClient->execSqlSync(
            "UPDATE team SET parent_team_id = $1::uuid WHERE id = $2::uuid",
            ptid, team_id);
      }
    }

    auto rows = dbClient->execSqlSync(
        "SELECT id::text, name, description, owner_user_id::text, "
        "parent_team_id::text, created_at::text FROM team WHERE id = $1::uuid",
        team_id);
    const auto& row = rows[0];
    Json::Value out(Json::objectValue);
    out["id"] = row["id"].as<std::string>();
    out["name"] = row["name"].as<std::string>();
    out["description"] = row["description"].isNull() ? Json::Value() : Json::Value(row["description"].as<std::string>());
    out["owner_user_id"] = row["owner_user_id"].isNull() ? Json::Value() : Json::Value(row["owner_user_id"].as<std::string>());
    out["parent_team_id"] = row["parent_team_id"].isNull() ? Json::Value() : Json::Value(row["parent_team_id"].as<std::string>());
    out["created_at"] = row["created_at"].isNull() ? Json::Value() : Json::Value(row["created_at"].as<std::string>());

    auto resp = HttpResponse::newHttpJsonResponse(out);
    resp->setStatusCode(k200OK);
    return callback(resp);
  } catch (const std::exception& e) {
    LOG_ERROR << "updateTeam failed: " << e.what();
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Internal server error"));
    resp->setStatusCode(k500InternalServerError);
    return callback(resp);
  }
}

void TeamController::addTeamMember(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback,
    std::string team_id) {
  const std::string userId = req->attributes()->get<std::string>("user_id");
  if (userId.empty()) {
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Unauthorized"));
    resp->setStatusCode(k401Unauthorized);
    return callback(resp);
  }

  auto body = req->getJsonObject();
  if (!body || !(*body)["user_id"].isString() || (*body)["user_id"].asString().empty()) {
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("user_id is required"));
    resp->setStatusCode(k400BadRequest);
    return callback(resp);
  }

  const std::string targetUserId = (*body)["user_id"].asString();
  auto dbClient = app().getDbClient();
  try {
    // Check team ownership
    auto check = dbClient->execSqlSync(
        "SELECT owner_user_id::text FROM team WHERE id = $1::uuid", team_id);
    if (check.empty()) {
      auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Team not found"));
      resp->setStatusCode(k404NotFound);
      return callback(resp);
    }
    if (check[0]["owner_user_id"].as<std::string>() != userId) {
      auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Insufficient permissions"));
      resp->setStatusCode(k403Forbidden);
      return callback(resp);
    }

    // Verify target user exists
    auto userCheck = dbClient->execSqlSync(
        "SELECT id FROM app_user WHERE id = $1::uuid", targetUserId);
    if (userCheck.empty()) {
      auto resp = HttpResponse::newHttpJsonResponse(Json::Value("User not found"));
      resp->setStatusCode(k404NotFound);
      return callback(resp);
    }

    dbClient->execSqlSync(
        "INSERT INTO team_member (team_id, user_id) VALUES ($1::uuid, $2::uuid) "
        "ON CONFLICT DO NOTHING",
        team_id, targetUserId);

    Json::Value out(Json::objectValue);
    out["team_id"] = team_id;
    out["user_id"] = targetUserId;
    auto resp = HttpResponse::newHttpJsonResponse(out);
    resp->setStatusCode(k201Created);
    return callback(resp);
  } catch (const std::exception& e) {
    LOG_ERROR << "addTeamMember failed: " << e.what();
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Internal server error"));
    resp->setStatusCode(k500InternalServerError);
    return callback(resp);
  }
}

void TeamController::removeTeamMember(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback,
    std::string team_id,
    std::string user_id) {
  const std::string callerId = req->attributes()->get<std::string>("user_id");
  if (callerId.empty()) {
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Unauthorized"));
    resp->setStatusCode(k401Unauthorized);
    return callback(resp);
  }

  auto dbClient = app().getDbClient();
  try {
    auto check = dbClient->execSqlSync(
        "SELECT owner_user_id::text FROM team WHERE id = $1::uuid", team_id);
    if (check.empty()) {
      auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Team not found"));
      resp->setStatusCode(k404NotFound);
      return callback(resp);
    }
    const std::string ownerId = check[0]["owner_user_id"].as<std::string>();
    // Owner can remove anyone; members can remove themselves
    if (callerId != ownerId && callerId != user_id) {
      auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Insufficient permissions"));
      resp->setStatusCode(k403Forbidden);
      return callback(resp);
    }
    // Cannot remove owner
    if (user_id == ownerId) {
      auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Cannot remove team owner"));
      resp->setStatusCode(k400BadRequest);
      return callback(resp);
    }

    dbClient->execSqlSync(
        "DELETE FROM team_member WHERE team_id = $1::uuid AND user_id = $2::uuid",
        team_id, user_id);

    auto resp = HttpResponse::newHttpResponse();
    resp->setStatusCode(k204NoContent);
    return callback(resp);
  } catch (const std::exception& e) {
    LOG_ERROR << "removeTeamMember failed: " << e.what();
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Internal server error"));
    resp->setStatusCode(k500InternalServerError);
    return callback(resp);
  }
}

void TeamController::assignTeamToTask(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback,
    std::string task_id) {
  const std::string userId = req->attributes()->get<std::string>("user_id");
  if (userId.empty()) {
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Unauthorized"));
    resp->setStatusCode(k401Unauthorized);
    return callback(resp);
  }

  auto body = req->getJsonObject();
  if (!body || !(*body)["team_id"].isString() || (*body)["team_id"].asString().empty()) {
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("team_id is required"));
    resp->setStatusCode(k400BadRequest);
    return callback(resp);
  }
  const std::string teamId = (*body)["team_id"].asString();

  auto dbClient = app().getDbClient();
  try {
    // Verify task exists
    auto taskCheck = dbClient->execSqlSync(
        "SELECT id FROM task WHERE id = $1::uuid", task_id);
    if (taskCheck.empty()) {
      auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Task not found"));
      resp->setStatusCode(k404NotFound);
      return callback(resp);
    }
    // Verify team exists
    auto teamCheck = dbClient->execSqlSync(
        "SELECT id FROM team WHERE id = $1::uuid", teamId);
    if (teamCheck.empty()) {
      auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Team not found"));
      resp->setStatusCode(k404NotFound);
      return callback(resp);
    }

    dbClient->execSqlSync(
        "INSERT INTO team_task_assignment (team_id, task_id) "
        "VALUES ($1::uuid, $2::uuid) ON CONFLICT DO NOTHING",
        teamId, task_id);

    Json::Value out(Json::objectValue);
    out["team_id"] = teamId;
    out["task_id"] = task_id;
    auto resp = HttpResponse::newHttpJsonResponse(out);
    resp->setStatusCode(k201Created);
    return callback(resp);
  } catch (const std::exception& e) {
    LOG_ERROR << "assignTeamToTask failed: " << e.what();
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Internal server error"));
    resp->setStatusCode(k500InternalServerError);
    return callback(resp);
  }
}

void TeamController::assignTeamToProject(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback,
    std::string project_id) {
  const std::string userId = req->attributes()->get<std::string>("user_id");
  if (userId.empty()) {
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Unauthorized"));
    resp->setStatusCode(k401Unauthorized);
    return callback(resp);
  }

  auto body = req->getJsonObject();
  if (!body || !(*body)["team_id"].isString() || (*body)["team_id"].asString().empty()) {
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("team_id is required"));
    resp->setStatusCode(k400BadRequest);
    return callback(resp);
  }
  const std::string teamId = (*body)["team_id"].asString();

  auto dbClient = app().getDbClient();
  try {
    // Verify project exists and is a top-level task
    auto projCheck = dbClient->execSqlSync(
        "SELECT id FROM task WHERE id = $1::uuid AND parent_task_id IS NULL",
        project_id);
    if (projCheck.empty()) {
      auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Project not found"));
      resp->setStatusCode(k404NotFound);
      return callback(resp);
    }
    // Verify team exists
    auto teamCheck = dbClient->execSqlSync(
        "SELECT id FROM team WHERE id = $1::uuid", teamId);
    if (teamCheck.empty()) {
      auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Team not found"));
      resp->setStatusCode(k404NotFound);
      return callback(resp);
    }

    dbClient->execSqlSync(
        "INSERT INTO team_project_assignment (team_id, project_id) "
        "VALUES ($1::uuid, $2::uuid) ON CONFLICT DO NOTHING",
        teamId, project_id);

    Json::Value out(Json::objectValue);
    out["team_id"] = teamId;
    out["project_id"] = project_id;
    auto resp = HttpResponse::newHttpJsonResponse(out);
    resp->setStatusCode(k201Created);
    return callback(resp);
  } catch (const std::exception& e) {
    LOG_ERROR << "assignTeamToProject failed: " << e.what();
    auto resp = HttpResponse::newHttpJsonResponse(Json::Value("Internal server error"));
    resp->setStatusCode(k500InternalServerError);
    return callback(resp);
  }
}
