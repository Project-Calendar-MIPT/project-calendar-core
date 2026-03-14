#pragma once

#include <drogon/HttpController.h>
using namespace drogon;

class AuthController : public drogon::HttpController<AuthController> {
 public:
  METHOD_LIST_BEGIN
  ADD_METHOD_TO(AuthController::registerUser, "/api/auth/register", Post);

  ADD_METHOD_TO(AuthController::login, "/api/auth/login", Post);

  ADD_METHOD_TO(AuthController::me, "/api/auth/me", Get, "AuthFilter");

  ADD_METHOD_TO(AuthController::confirmEmail, "/api/auth/confirm", Get);

  ADD_METHOD_TO(AuthController::forgotPassword, "/api/auth/forgot-password",
                Post);

  ADD_METHOD_TO(AuthController::resetPassword, "/api/auth/reset-password",
                Post);
  METHOD_LIST_END

  void registerUser(
      const drogon::HttpRequestPtr& req,
      std::function<void(const drogon::HttpResponsePtr&)>&& callback);

  void login(const drogon::HttpRequestPtr& req,
             std::function<void(const drogon::HttpResponsePtr&)>&& callback);

  void me(const drogon::HttpRequestPtr& req,
          std::function<void(const drogon::HttpResponsePtr&)>&& callback);

  void confirmEmail(
      const drogon::HttpRequestPtr& req,
      std::function<void(const drogon::HttpResponsePtr&)>&& callback);

  void forgotPassword(const HttpRequestPtr& req,
                      std::function<void(const HttpResponsePtr&)>&& callback);

  void resetPassword(const HttpRequestPtr& req,
                     std::function<void(const HttpResponsePtr&)>&& callback);
};