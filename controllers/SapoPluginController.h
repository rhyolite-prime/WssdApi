//
// SapoPluginController.h — HTTP endpoints that run sapo blueprints as
// plugins: "given a DSL blueprint (or the id of one registered from
// sapo/plugins/) plus the execution variables as JSON, run it through the
// embedded Sapo engine and return the outcome".
//
// Endpoints (all JSON):
//   POST /api/v1/sapo/plugins/execute                  — run a plugin
//   POST /api/v1/sapo/plugins/sessions/{sid}/resume    — resume a suspended one
//   GET  /api/v1/sapo/plugins/sessions/{sid}           — session snapshot
//   GET  /api/v1/sapo/plugins/list                     — registered blueprints
//
// Handlers are deliberately callback-style (no C++20 coroutine co_await):
// the blocking engine call is posted to the sapo BlockingRunner pool and the
// drogon response callback is invoked from that worker thread, which drogon
// explicitly supports. A co_await of BlockingRunner::run(...) inside a drogon
// Task-returning handler provoked heap corruption on GCC 12 during frame
// reconstruction on the worker thread; the callback shape sidesteps the
// hazard entirely.
//

#ifndef WSSDAPI_SAPOPLUGINCONTROLLER_H
#define WSSDAPI_SAPOPLUGINCONTROLLER_H

#include <drogon/HttpController.h>

namespace wssd_api::sapo_host {
class SapoEngineService;
}

using namespace drogon;

class SapoPluginController : public drogon::HttpController<SapoPluginController> {
  public:
    /// Engine singleton: resolved once per worker call so the posted jobs
    /// never dereference a drogon plugin pointer off the IO threads.
    static wssd_api::sapo_host::SapoEngineService &engineInstance();

    static constexpr const char *PREFIX = "/api/v1/sapo/plugins/";

    METHOD_LIST_BEGIN
        ADD_METHOD_TO(SapoPluginController::executePlugin, std::string(PREFIX) + "execute", Post);
        ADD_METHOD_TO(SapoPluginController::resumePluginSession,
                      std::string(PREFIX) + "sessions/{sessionId}/resume", Post);
        ADD_METHOD_TO(SapoPluginController::getPluginSession,
                      std::string(PREFIX) + "sessions/{sessionId}", Get);
        ADD_METHOD_TO(SapoPluginController::listPlugins, std::string(PREFIX) + "list", Get);
    METHOD_LIST_END

    void executePlugin(const HttpRequestPtr &req,
                       std::function<void(const HttpResponsePtr &)> &&callback);
    void resumePluginSession(const HttpRequestPtr &req,
                             std::function<void(const HttpResponsePtr &)> &&callback,
                             std::string sessionId);
    void getPluginSession(const HttpRequestPtr &req,
                          std::function<void(const HttpResponsePtr &)> &&callback,
                          std::string sessionId);
    void listPlugins(const HttpRequestPtr &req,
                     std::function<void(const HttpResponsePtr &)> &&callback);
};

#endif  // WSSDAPI_SAPOPLUGINCONTROLLER_H
