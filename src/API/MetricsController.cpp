#include "MetricsController.h"
#include "Metrics.h"

void MetricsController::getMetrics(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback)
{
    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setStatusCode(drogon::k200OK);
    resp->setContentTypeCodeAndCustomString(drogon::CT_NONE, "text/plain; version=0.0.4; charset=utf-8");
    resp->setBody(Metrics::instance().toPrometheusText());
    callback(resp);
}
