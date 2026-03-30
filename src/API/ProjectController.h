#pragma once
#include <drogon/HttpController.h>

using namespace drogon;

class ProjectController : public drogon::HttpController<ProjectController> {
 public:
  METHOD_LIST_BEGIN
  ADD_METHOD_TO(ProjectController::createProject, "/api/projects", Post, "AuthFilter");
  ADD_METHOD_TO(ProjectController::getProjects, "/api/projects", Get, "AuthFilter");
  ADD_METHOD_TO(ProjectController::getProject, "/api/projects/{project_id}", Get, "AuthFilter");
  ADD_METHOD_TO(ProjectController::deleteProject, "/api/projects/{project_id}", Delete, "AuthFilter");
  ADD_METHOD_TO(ProjectController::updateProject, "/api/projects/{project_id}", Put, "AuthFilter");
  ADD_METHOD_TO(ProjectController::getProjectProgress, "/api/projects/{project_id}/progress", Get, "AuthFilter");
  ADD_METHOD_TO(ProjectController::getProjectParticipants, "/api/projects/{project_id}/participants", Get, "AuthFilter");
  ADD_METHOD_TO(ProjectController::inviteUser, "/api/projects/{project_id}/invite", Post, "AuthFilter");
  ADD_METHOD_TO(ProjectController::applyToProject, "/api/projects/{project_id}/apply", Post, "AuthFilter");
  ADD_METHOD_TO(ProjectController::listApplications, "/api/projects/{project_id}/applications", Get, "AuthFilter");
  ADD_METHOD_TO(ProjectController::approveApplication, "/api/projects/{project_id}/applications/{application_id}/approve", Post, "AuthFilter");
  ADD_METHOD_TO(ProjectController::rejectApplication, "/api/projects/{project_id}/applications/{application_id}/reject", Post, "AuthFilter");
  METHOD_LIST_END

  void createProject(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback);
  void getProjects(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback);
  void getProject(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback);
  void deleteProject(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback);
  void updateProject(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback);
  void getProjectProgress(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback);
  void getProjectParticipants(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback);
  void inviteUser(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback);
  void applyToProject(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback);
  void listApplications(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback);
  void approveApplication(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback);
  void rejectApplication(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback);
};
