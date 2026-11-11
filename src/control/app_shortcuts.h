#pragma once
#include "control_types.h"
#include <windows.h>
#include <functional>

namespace runner::control {
enum class ShortcutDelivery { Delivered, Unsupported, Rejected, Unknown };
// Call only for the backend's owned target. Revalidate before every message.
ShortcutDelivery deliver_app_shortcut(HWND target, HWND focus, const ActionRequest& request,
                                     const std::function<bool()>& valid);
}
