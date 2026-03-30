#pragma once

#include <drogon/HttpFilter.h>
#include <json/json.h>

class AuthFilter : public drogon::HttpFilter<AuthFilter> {
 public:
  void doFilter(const drogon::HttpRequestPtr& req, drogon::FilterCallback&& fcb,
                drogon::FilterChainCallback&& fccb) override;
};