#pragma once
#include <drogon/HttpController.h>

using namespace drogon;

class TeamController : public drogon::HttpController<TeamController> {
 public:
  METHOD_LIST_BEGIN
  ADD_METHOD_TO(TeamController::createTeam, "/api/teams", Post, "AuthFilter");
  ADD_METHOD_TO(TeamController::getTeam, "/api/teams/{team_id}", Get, "AuthFilter");
  ADD_METHOD_TO(TeamController::updateTeam, "/api/teams/{team_id}", Put, "AuthFilter");
  ADD_METHOD_TO(TeamController::addTeamMember, "/api/teams/{team_id}/members", Post, "AuthFilter");
  ADD_METHOD_TO(TeamController::removeTeamMember, "/api/teams/{team_id}/members/{user_id}", Delete, "AuthFilter");
  ADD_METHOD_TO(TeamController::assignTeamToTask, "/api/tasks/{task_id}/assign-team", Post, "AuthFilter");
  ADD_METHOD_TO(TeamController::assignTeamToProject, "/api/projects/{project_id}/assign-team", Post, "AuthFilter");
  METHOD_LIST_END

  void createTeam(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback);
  void getTeam(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback, std::string team_id);
  void updateTeam(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback, std::string team_id);
  void addTeamMember(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback, std::string team_id);
  void removeTeamMember(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback, std::string team_id, std::string user_id);
  void assignTeamToTask(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback, std::string task_id);
  void assignTeamToProject(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback, std::string project_id);
};
