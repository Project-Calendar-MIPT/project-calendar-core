#pragma once

#include <drogon/HttpController.h>

using namespace drogon;

class UsersController : public drogon::HttpController<UsersController> {
 public:
  METHOD_LIST_BEGIN
  ADD_METHOD_TO(UsersController::updateSchedule, "/api/users/schedule", Put,
                "AuthFilter");

  ADD_METHOD_TO(UsersController::getSchedule, "/api/users/schedule", Get,
                "AuthFilter");

  ADD_METHOD_TO(UsersController::searchUsers, "/api/users", Get);

  ADD_METHOD_TO(UsersController::getUserProfile, "/api/users/{id}", Get,
                "AuthFilter");

  ADD_METHOD_TO(UsersController::updateUserProfile, "/api/users/profile", Put,
                "AuthFilter");

  ADD_METHOD_TO(UsersController::uploadAvatar, "/api/users/avatar", Post,
                "AuthFilter");
  ADD_METHOD_TO(UsersController::changePassword, "/api/users/password", Put,
                "AuthFilter");

  ADD_METHOD_TO(UsersController::requestEmailChange, "/api/users/email/request",
                Post, "AuthFilter");

  ADD_METHOD_TO(UsersController::confirmEmailChange, "/api/users/email/confirm",
                Get);
  METHOD_LIST_END

  void updateSchedule(const HttpRequestPtr& req,
                      std::function<void(const HttpResponsePtr&)>&& callback);

  void getSchedule(const HttpRequestPtr& req,
                   std::function<void(const HttpResponsePtr&)>&& callback);

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

  void changePassword(const HttpRequestPtr& req,
                      std::function<void(const HttpResponsePtr&)>&& callback);

  void requestEmailChange(
      const HttpRequestPtr& req,
      std::function<void(const HttpResponsePtr&)>&& callback);

  void confirmEmailChange(
      const HttpRequestPtr& req,
      std::function<void(const HttpResponsePtr&)>&& callback);
};