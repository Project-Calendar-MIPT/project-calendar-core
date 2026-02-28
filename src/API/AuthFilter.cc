#include "AuthFilter.h"

#include <drogon/HttpAppFramework.h>
#include <drogon/HttpResponse.h>
#include <json/json.h>
#include <jwt-cpp/jwt.h>
#include <trantor/utils/Logger.h>

using namespace drogon;

void AuthFilter::doFilter(const HttpRequestPtr& req, FilterCallback&& fcb,
                          FilterChainCallback&& fccb) {
  const std::string auth = req->getHeader("Authorization");
  const std::string prefix = "Bearer ";
  if (auth.size() <= prefix.size() ||
      auth.compare(0, prefix.size(), prefix) != 0) {
    Json::Value j;
    j["error"] = "Missing or invalid Authorization header";
    auto resp = HttpResponse::newHttpJsonResponse(j);
    resp->setStatusCode(k401Unauthorized);
    fcb(resp);
    return;
  }

  const std::string token = auth.substr(prefix.size());

  const char* envSecret = std::getenv("JWT_SECRET");
  const std::string secret = envSecret ? envSecret : "secret_key";

  try {
    auto decoded = jwt::decode(token);

    auto verifier =
        jwt::verify().allow_algorithm(jwt::algorithm::hs256{secret});
    verifier.verify(decoded);

    std::string userId;
    if (decoded.has_payload_claim("user_id")) {
      userId = decoded.get_payload_claim("user_id").as_string();
    } else if (decoded.has_payload_claim("sub")) {
      userId = decoded.get_payload_claim("sub").as_string();
    } else {
      Json::Value j;
      j["error"] = "Token does not contain user id";
      auto resp = HttpResponse::newHttpJsonResponse(j);
      resp->setStatusCode(k401Unauthorized);
      fcb(resp);
      return;
    }

    try {
      req->attributes()->insert("user_id", userId);
    } catch (const std::exception& ex) {
      LOG_ERROR << "AuthFilter: failed to insert attribute user_id: "
                << ex.what();
      Json::Value j;
      j["error"] = "Internal server error";
      auto resp = HttpResponse::newHttpJsonResponse(j);
      resp->setStatusCode(k500InternalServerError);
      fcb(resp);
      return;
    }

    fccb();
    return;

  } catch (const jwt::error::token_verification_exception& e) {
    LOG_WARN << "AuthFilter token verification failed: " << e.what();
    Json::Value j;
    j["error"] = "Invalid or expired token";
    auto resp = HttpResponse::newHttpJsonResponse(j);
    resp->setStatusCode(k401Unauthorized);
    fcb(resp);
    return;
  } catch (const std::exception& e) {
    LOG_WARN << "AuthFilter token error: " << e.what();
    Json::Value j;
    j["error"] = "Invalid token";
    auto resp = HttpResponse::newHttpJsonResponse(j);
    resp->setStatusCode(k401Unauthorized);
    fcb(resp);
    return;
  }
}