#pragma once
#include <atomic>
#include <chrono>
#include <string>
#include <sstream>

class Metrics {
public:
    static Metrics& instance() {
        static Metrics inst;
        return inst;
    }

    void incRequest()  { ++requests_total_; }
    void incError4xx() { ++errors_4xx_; }
    void incError5xx() { ++errors_5xx_; }

    std::string toPrometheusText() const {
        using namespace std::chrono;
        auto uptime = duration_cast<seconds>(steady_clock::now() - start_time_).count();
        std::ostringstream out;
        out << "# HELP http_requests_total Total HTTP requests handled\n"
            << "# TYPE http_requests_total counter\n"
            << "http_requests_total " << requests_total_.load() << "\n\n"
            << "# HELP http_errors_4xx_total HTTP 4xx responses\n"
            << "# TYPE http_errors_4xx_total counter\n"
            << "http_errors_4xx_total " << errors_4xx_.load() << "\n\n"
            << "# HELP http_errors_5xx_total HTTP 5xx responses\n"
            << "# TYPE http_errors_5xx_total counter\n"
            << "http_errors_5xx_total " << errors_5xx_.load() << "\n\n"
            << "# HELP process_uptime_seconds Server uptime in seconds\n"
            << "# TYPE process_uptime_seconds gauge\n"
            << "process_uptime_seconds " << uptime << "\n\n"
            << "# HELP app_info Application metadata\n"
            << "# TYPE app_info gauge\n"
            << "app_info{version=\"1.0\"} 1\n";
        return out.str();
    }

private:
    Metrics() : start_time_(std::chrono::steady_clock::now()) {}

    std::atomic<uint64_t> requests_total_{0};
    std::atomic<uint64_t> errors_4xx_{0};
    std::atomic<uint64_t> errors_5xx_{0};
    std::chrono::steady_clock::time_point start_time_;
};
