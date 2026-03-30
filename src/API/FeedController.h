#pragma once
#include <drogon/HttpController.h>

using namespace drogon;

class FeedController : public drogon::HttpController<FeedController> {
 public:
  METHOD_LIST_BEGIN
  ADD_METHOD_TO(FeedController::getRecommendedProjects, "/api/projects/recommended", Get, "AuthFilter");
  ADD_METHOD_TO(FeedController::getFeed, "/api/feed", Get, "AuthFilter");
  ADD_METHOD_TO(FeedController::getDeadlines, "/api/tasks/deadlines", Get, "AuthFilter");
  METHOD_LIST_END

  void getRecommendedProjects(const HttpRequestPtr& req,
                              std::function<void(const HttpResponsePtr&)>&& callback);
  void getFeed(const HttpRequestPtr& req,
               std::function<void(const HttpResponsePtr&)>&& callback);
  void getDeadlines(const HttpRequestPtr& req,
                    std::function<void(const HttpResponsePtr&)>&& callback);
};
