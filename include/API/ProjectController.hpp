#pragma once

#include <drogon/HttpController.h>

using namespace drogon;

class ProjectController : public drogon::HttpController<ProjectController> {
 public:
  METHOD_LIST_BEGIN
  ADD_METHOD_TO(ProjectController::createProject, "/api/projects", Post,
                "AuthFilter");

  ADD_METHOD_TO(ProjectController::getProjects, "/api/projects", Get,
                "AuthFilter");

  ADD_METHOD_TO(ProjectController::getProjectProgress,
                "/api/projects/{project_id}/progress", Get, "AuthFilter");

  ADD_METHOD_TO(ProjectController::getProjectParticipants,
                "/api/projects/{project_id}/participants", Get,
                "AuthFilter");
  METHOD_LIST_END

  void createProject(const HttpRequestPtr& req,
                     std::function<void(const HttpResponsePtr&)>&& callback);

  void getProjects(const HttpRequestPtr& req,
                   std::function<void(const HttpResponsePtr&)>&& callback);

  void getProjectProgress(
      const HttpRequestPtr& req,
      std::function<void(const HttpResponsePtr&)>&& callback);

  void getProjectParticipants(
      const HttpRequestPtr& req,
      std::function<void(const HttpResponsePtr&)>&& callback);
};
