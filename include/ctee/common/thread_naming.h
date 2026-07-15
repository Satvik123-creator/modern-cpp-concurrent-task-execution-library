#pragma once

#include <string>
#include <thread>

namespace ctee::common {

// Platform-specific thread naming for debugger/profiler visibility.
//
// DESIGN: Header-only utility. Uses #ifdef for platform detection.
// On Windows, uses SetThreadDescription (Win10 1607+). On Linux,
// uses pthread_setname_np. On unsupported platforms, no-op.
// This costs nothing at runtime on platforms that don't support it.

#ifdef _WIN32
#include <windows.h>
inline void set_thread_name(const std::string& name) {
    // SetThreadDescription requires Win10 1607+
    // Convert to wide string
    int size = MultiByteToWideChar(CP_UTF8, 0, name.c_str(), -1, nullptr, 0);
    std::wstring wide(size, 0);
    MultiByteToWideChar(CP_UTF8, 0, name.c_str(), -1, &wide[0], size);
    
    // Load dynamically to avoid link-time dependency
    static auto pSetThreadDescription = reinterpret_cast<decltype(&SetThreadDescription)>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription"));
    if (pSetThreadDescription) {
        HANDLE thread = GetCurrentThread();
        pSetThreadDescription(thread, wide.c_str());
    }
}
#elif defined(__linux__)
#include <pthread.h>
inline void set_thread_name(const std::string& name) {
    // pthread_setname_np on Linux truncates to 15 chars
    std::string truncated = name.substr(0, 15);
    pthread_setname_np(pthread_self(), truncated.c_str());
}
#else
inline void set_thread_name(const std::string&) {
    // No-op on unsupported platforms
}
#endif

}  // namespace ctee::common
