#pragma once
#include "action_scheduler.h"
namespace runner::control {
// Selected only by trusted launcher argv; never by an SCRP action payload.
std::shared_ptr<WorkerBackend> make_app_window_backend(const std::string& session);
bool is_app_window_backend(const std::shared_ptr<WorkerBackend>& backend);
}
