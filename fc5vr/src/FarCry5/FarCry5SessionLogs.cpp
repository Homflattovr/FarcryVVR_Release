// sessionlogs: see FarCry5SessionLogs.h.
#include "FarCry5SessionLogs.h"

#include <atomic>

#include <spdlog/spdlog.h>

namespace fc5::sessionlogs
{
    namespace
    {
        std::atomic<bool> g_on{ true };
        std::atomic<bool> g_played{ false };
        std::atomic<bool> g_quiet{ false };
        std::atomic<int>  g_prev_level{ -1 };   // the logger's level before it went quiet (the framework sets debug at its start)

        void apply(const char* why)
        {
            const bool want = !g_on.load() && g_played.load();
            if (g_quiet.load() == want) return;
            auto lg = spdlog::default_logger();
            if (!lg) return;
            if (want) {
                spdlog::info("[Log] sessionlogs off ({}): from now on only warnings and errors are logged, and the default probes "
                             "(gputime, memwatch, partwatch, chuteprobe) are paused. F11 Main \"Logs during sessions (slower)\" or "
                             "\"sessionlogs on\" brings every line back", why);
                lg->flush();
                g_prev_level = int(lg->level());
                lg->set_level(spdlog::level::warn);
                g_quiet = true;
            } else {
                const int prev = g_prev_level.exchange(-1);
                lg->set_level(prev >= 0 ? spdlog::level::level_enum(prev) : spdlog::level::info);
                g_quiet = false;
                spdlog::info("[Log] sessionlogs on ({}): every line logged again, the default probes running again", why);
            }
        }
    }

    bool on() { return g_on.load(std::memory_order_relaxed); }

    bool quiet() { return g_quiet.load(std::memory_order_relaxed); }

    void set(bool v)
    {
        const bool was = g_on.exchange(v);
        if (was != v)
            spdlog::info("[Log] sessionlogs {}{}", v ? "on: every line logged, the default probes running" : "off: only warnings and errors from the first play on",
                         g_played.load() || v ? "" : " (not yet: everything up to the first play is still logged)");
        apply(g_played.load() ? "now" : "first play");
    }

    void play_started()
    {
        if (g_played.exchange(true)) return;
        apply("first play");
    }
}
