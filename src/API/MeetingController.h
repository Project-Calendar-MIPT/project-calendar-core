#pragma once
#include <drogon/HttpController.h>
using namespace drogon;

class MeetingController : public drogon::HttpController<MeetingController> {
 public:
  METHOD_LIST_BEGIN
  ADD_METHOD_TO(MeetingController::createMeeting,
                "/api/meetings", Post, "AuthFilter");
  ADD_METHOD_TO(MeetingController::listMeetings,
                "/api/meetings", Get, "AuthFilter");
  ADD_METHOD_TO(MeetingController::getMeeting,
                "/api/meetings/{id}", Get, "AuthFilter");
  ADD_METHOD_TO(MeetingController::deleteMeeting,
                "/api/meetings/{id}", Delete, "AuthFilter");
  METHOD_LIST_END

  void createMeeting(const HttpRequestPtr& req,
                     std::function<void(const HttpResponsePtr&)>&& callback);
  void listMeetings(const HttpRequestPtr& req,
                    std::function<void(const HttpResponsePtr&)>&& callback);
  void getMeeting(const HttpRequestPtr& req,
                  std::function<void(const HttpResponsePtr&)>&& callback);
  void deleteMeeting(const HttpRequestPtr& req,
                     std::function<void(const HttpResponsePtr&)>&& callback);
};
