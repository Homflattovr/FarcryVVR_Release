// ctrlrec: see FarCry5CtrlRec.h. The rows are kept in memory while recording and written by a thread of their own at the end, so
// the render thread never waits for the disk.
#include "FarCry5CtrlRec.h"
#include "ModPath.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#include <glm/gtc/quaternion.hpp>
#include <spdlog/spdlog.h>

namespace fc5::ctrlrec
{
namespace
{
    using clock = std::chrono::steady_clock;

    struct Row
    {
        float    t_ms;
        int      frame;
        uint32_t sig;
        uint8_t  two, other, gun;
        float    c[7], o[7], h[7], d[7];   // place (m) and turn (w x y z): the controller, the other one, the gun asked for, the gun drawn
    };

    std::atomic<int>  g_ask{ 0 };        // seconds asked for by the command (taken by the render thread); -1: end now
    std::atomic<bool> g_auto{ false };   // ctrlrecauto (saved)

    // Render thread only.
    std::vector<Row>  s_rows;
    bool              s_on{ false }, s_is_auto{ false }, s_auto_done{ false };
    clock::time_point s_t0{}, s_end{}, s_gun_since{};
    bool              s_gun_was{ false };
    glm::mat4         s_last_ctrl{ 0.0f };

    void put(float* f, const glm::mat4& m, float ws)
    {
        const glm::quat q = glm::normalize(glm::quat_cast(glm::mat3{ m }));
        f[0] = m[3][0] / ws; f[1] = m[3][1] / ws; f[2] = m[3][2] / ws;
        f[3] = q.w; f[4] = q.x; f[5] = q.y; f[6] = q.z;
    }

    void write_out(std::vector<Row> rows, bool is_auto)
    {
        const std::string path = fc5::mod_file(is_auto ? "fc5vr_ctrlrec_auto.csv" : "fc5vr_ctrlrec.csv");
        std::thread([rows = std::move(rows), path] {
            FILE* fp = nullptr;
            if (fopen_s(&fp, path.c_str(), "w") != 0 || !fp) {
                spdlog::info("[CtrlRec] could not write {}", path);
                return;
            }
            std::fprintf(fp, "t_ms,frame,weapon,two_hand,other,gun_drawn,cx,cy,cz,cqw,cqx,cqy,cqz,ox,oy,oz,oqw,oqx,oqy,oqz,hx,hy,hz,hqw,hqx,hqy,hqz,dx,dy,dz,dqw,dqx,dqy,dqz\n");
            for (const Row& r : rows) {
                std::fprintf(fp, "%.2f,%d,%u,%d,%d,%d", r.t_ms, r.frame, r.sig, r.two, r.other, r.gun);
                for (const float* f : { r.c, r.o, r.h, r.d })
                    for (int i = 0; i < 7; ++i) std::fprintf(fp, i < 3 ? ",%.5f" : ",%.6f", f[i]);
                std::fputc('\n', fp);
            }
            std::fclose(fp);
            spdlog::info("[CtrlRec] {} controller readings written to {}", rows.size(), path);
        }).detach();
    }

    void start(int seconds, bool is_auto, clock::time_point now)
    {
        s_rows.clear();
        s_rows.reserve(size_t(seconds) * 130);
        s_on      = true;
        s_is_auto = is_auto;
        s_t0      = now;
        s_end     = now + std::chrono::seconds(seconds);
        spdlog::info("[CtrlRec] recording the weapon controller for {} s{}", seconds, is_auto ? " (ctrlrecauto: once per session)" : "");
    }
}

bool command(const std::string& cmd, const std::string& arg, float fv, bool& persist)
{
    if (cmd == "ctrlrec") {
        g_ask = arg == "off" ? -1 : arg.empty() ? 20 : std::clamp(int(fv), 1, 300);
        return true;
    }
    if (cmd == "ctrlrecauto") {
        g_auto  = arg == "on" || arg == "1" || arg == "true";
        persist = true;
        return true;
    }
    return false;
}

void note(int frame, const glm::mat4& ctrl, bool have_other, const glm::vec3& other_pos, const glm::mat3& other_rot, const glm::mat4& held, const glm::mat4& out, uint32_t weapon_sig,
          bool two_hand, bool gun_drawn, float world_scale)
{
    const auto now = clock::now();
    if (const int ask = g_ask.exchange(0); ask > 0) {
        start(ask, false, now);
    } else if (ask < 0 && s_on) {
        s_end = now;
    }
    // ctrlrecauto: once per session, after a gun has been in hand for 5 s on end.
    if (!s_on && !s_auto_done && g_auto.load()) {
        if (gun_drawn && weapon_sig != 0) {
            if (!s_gun_was) s_gun_since = now;
            if (now - s_gun_since >= std::chrono::seconds(5)) {
                s_auto_done = true;
                start(20, true, now);
            }
        }
        s_gun_was = gun_drawn && weapon_sig != 0;
    }
    if (!s_on) return;
    if (now >= s_end || s_rows.size() >= 60000) {
        s_on = false;
        write_out(std::move(s_rows), s_is_auto);
        s_rows = {};
        return;
    }
    if (std::memcmp(&ctrl, &s_last_ctrl, sizeof(ctrl)) == 0) return;   // the pair's second eye: the same reading
    s_last_ctrl = ctrl;
    const float ws = std::max(1e-3f, world_scale);
    Row         r{};
    r.t_ms  = std::chrono::duration<float, std::milli>(now - s_t0).count();
    r.frame = frame;
    r.sig   = weapon_sig;
    r.two   = two_hand ? 1 : 0;
    r.other = have_other ? 1 : 0;
    r.gun   = gun_drawn ? 1 : 0;
    put(r.c, ctrl, ws);
    glm::mat4 o{ other_rot };
    o[3] = glm::vec4{ other_pos, 1.0f };
    put(r.o, o, ws);
    put(r.h, held, ws);
    put(r.d, out, ws);
    s_rows.push_back(r);
}
}
