#include "control_session.h"
#include "gui_session.h"
#include <thread>
#include <iostream>

namespace runner {
::control::ReplyHandlers management_handlers() {
    const auto started = std::chrono::steady_clock::now();
    ::control::ReplyHandlers handlers;
    for (const auto type : {"HEARTBEAT","STATE_REQUEST","TERMINATE","OBSERVE","ACTION_REQUEST","ARTIFACT_REQUEST"}) {
        handlers[type] = [started](const scrp::Envelope& e) {
            ::control::Reply reply;
            bool finish = false;
            if (e.type == "HEARTBEAT" || e.type == "STATE_REQUEST") {
                reply.type = e.type == "HEARTBEAT" ? "ALIVE" : "STATE_RESULT";
                reply.payload["runtime_state"] = "DEGRADED";
                reply.payload["worker_alive"] = false;
                reply.payload["queue_depth"] = 0;
                if (e.type == "HEARTBEAT") reply.payload["uptime_ms"] = Json::UInt64(
                    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-started).count());
                else {
                    reply.payload["action_state"] = Json::Value();
                    if (!e.payload["action_id"].isNull()) {
                        reply.payload["action_state"]["action_id"] = e.payload["action_id"];
                        reply.payload["action_state"]["status"] = "UNKNOWN";
                    }
                }
            } else if (e.type == "TERMINATE") {
                reply.type = "TERMINATE_RESULT";
                reply.payload["worker_stopped"] = true; // no worker was started
                reply.payload["pending_actions_dropped"] = 0;
                finish = true;
            } else {
                reply.type = "ERROR"; reply.status = "ERROR";
                reply.error["code"] = "UNSUPPORTED_TYPE";
                reply.error["message"] = "GUI execution and uploads are unavailable in Control management mode";
                reply.error["retryable"] = false;
                reply.error["recommended_next_step"] = "IMPLEMENT_WORKER";
            }
            return ::control::Replies{{std::move(reply)}, finish};
        };
    }
    return handlers;
}

DWORD run_control_session(::control::Context context, HANDLE stop_event, GuiSession* gui) {
    if (gui) context.schema = gui->schema();
    ::control::Receiver receiver(std::move(context), gui ? gui->handlers() : management_handlers(), {},
        telemetry::make_winhttp_websocket, gui ? gui->hooks() : ::control::Hooks{});
    std::atomic<bool> stop{false}, done{false};
    std::thread worker([&] { receiver.run(stop); done = true; });
    DWORD result = ERROR_SUCCESS;
    while (!done) {
        const auto wait = WaitForSingleObject(stop_event, 20);
        if (wait == WAIT_OBJECT_0) { if (gui) gui->block(); stop = true; break; }
        if (wait == WAIT_FAILED) { result = GetLastError(); if (gui) gui->block(); stop = true; break; }
    }
    worker.join();
    if (gui) gui->stop();
    const auto snapshot = receiver.snapshot();
    std::cout << "Control stopped dispatched=" << snapshot.dispatched << '\n';
    if (snapshot.state == ::control::State::failed) {
        std::cerr << snapshot.diagnostic << '\n';
        return ERROR_GEN_FAILURE;
    }
    return result;
}
}
