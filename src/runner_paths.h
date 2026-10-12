#pragma once

namespace runner {
// One Sandbox is one session. This is a guest-local directory, never a writable
// Host mapping. Bootstrap and file-producing components must use this path.
constexpr wchar_t default_output_path[] = L"C:\\RunnerWorkspace\\Output";
}
