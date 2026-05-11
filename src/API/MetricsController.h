#pragma once
#include <drogon/HttpController.h>

class MetricsController : public drogon::HttpController<MetricsController, false> {
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(MetricsController::getMetrics, "/metrics", drogon::Get);
    METHOD_LIST_END

    void getMetrics(const drogon::HttpRequestPtr& req,
                    std::function<void(const drogon::HttpResponsePtr&)>&& callback);
};
