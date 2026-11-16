// Functions to run when the game quits normally. main() ends the process with _Exit after shutdown (game threads that never saw the
// exit flag are still running guest code, so neither freeing guest memory nor static destructors are safe), which skips atexit handlers:
// diagnostics summaries register here as well.
#pragma once
#include <mutex>
#include <vector>

inline std::mutex& psp_quit_mutex() { static std::mutex m; return m; }
inline std::vector<void (*)()>& psp_quit_hooks() { static std::vector<void (*)()> v; return v; }
inline void psp_on_quit(void (*fn)()) { std::lock_guard<std::mutex> l(psp_quit_mutex()); psp_quit_hooks().push_back(fn); }
inline void psp_run_quit_hooks() {
    std::vector<void (*)()> v;
    { std::lock_guard<std::mutex> l(psp_quit_mutex()); v = psp_quit_hooks(); }
    for (void (*f)() : v) f();
}
