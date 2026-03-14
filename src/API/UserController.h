#pragma once

#include <drogon/HttpController.h>

using namespace drogon;

class UsersController : public drogon::HttpController<UsersController> {
 public:
  METHOD_LIST_BEGIN
  ADD_METHOD_TO(UsersController::setWorkSchedule,
                "/api/users/{id}/work-schedule", Post, "AuthFilter");

  ADD_METHOD_TO(UsersController::getWorkSchedule,
                "/api/users/{id}/work-schedule", Get);

  ADD_METHOD_TO(UsersController::searchUsers, "/api/users", Get);

  ADD_METHOD_TO(UsersController::getUserProfile, "/api/users/{id}", Get,
                "AuthFilter");

  ADD_METHOD_TO(UsersController::updateUserProfile, "/api/users/profile", Put,
                "AuthFilter");

  ADD_METHOD_TO(UsersController::uploadAvatar, "/api/users/avatar", Post,
                "AuthFilter");
  METHOD_LIST_END

  void setWorkSchedule(const HttpRequestPtr& req,
                       std::function<void(const HttpResponsePtr&)>&& callback);

  void getWorkSchedule(const HttpRequestPtr& req,
                       std::function<void(const HttpResponsePtr&)>&& callback);

  void searchUsers(const HttpRequestPtr& req,
                   std::function<void(const HttpResponsePtr&)>&& callback);

  void getUserProfile(const HttpRequestPtr& req,
                      std::function<void(const HttpResponsePtr&)>&& callback);

  void updateUserProfile(
      const HttpRequestPtr& req,
      std::function<void(const HttpResponsePtr&)>&& callback);

  void uploadAvatar(const HttpRequestPtr& req,
                    std::function<void(const HttpResponsePtr&)>&& callback);
};