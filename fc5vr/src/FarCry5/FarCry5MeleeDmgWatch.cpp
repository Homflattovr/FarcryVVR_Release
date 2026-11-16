// meleedmgwatch (melee fix 14 probe, 15 Nov; author 15 Nov 22:18Z: one simulator run to find where the game applies its melee damage; research\
// 2026-11-12_melee_hold_react_collide.md, "Fix 14 probe"). Test only: the command is never saved, off at every start.
//
// The health object. GetPawnHealth's native (+0x2FA2900, dumpbin of this build) does:
//   lookup +0xB27D60 (manager [+0x4FAC1E0], handle* out, id); ent = [handle+0x10]; key = [+0xEF87C0() + 0x20] (u32);
//   comp = [ent+0xC8] ? +0xCCCFF0([ent+0xC8], &key, ent+0xA8) : +0xCCD250(ent, &key); H = [comp+0x48]; health = +0xBD8230(H) (float);
//   the handle released (refcount at +8, +0x26E320 at the last; the null handle +0x4CB9E70 never).
// +0xEF87C0, +0xCCCFF0, +0xCCD250 and +0xBD8230 are jmps into protected code, so the field the getter reads is not known statically. The probe
// makes the same calls on the script thread (as the native, which the ray already calls there every cast), checks the getter gives the native's
// answer, and looks for the float equal to the health in H (0x400 bytes), then comp (0x200), then the entity (0x800): at most 4 addresses, one
// debug register each (write, 4 bytes). Whatever is found, the three regions are copied at the arm and compared at the end: the changed dwords
// are logged, so a field stored another way is still found for the next run.
//
// The watch: set and cleared by the probe's own thread (every thread of the game, the script thread included, suspended one at a time while its
// debug registers are set; nothing allocated while one is suspended). The vectored handler (first in the chain) only copies registers and stack
// words into a fixed record, clears that thread's watch and continues; the probe's thread logs it. 3 s at most; one watch at a time; debug
// registers already in use (camwatch) or any fault: the probe logs and turns itself off.
//
// Probe 2 (16 Nov, after the 08:17 run caught the writer FC_m64+0x108e365): each WRITE also logs the damage event the game passed (r8: its
// class, type, handles, position, every non-zero dword) and the unwound caller chain (RtlVirtualUnwind from the trap, marking the frame that
// holds the event: the code that built it). Same command, same arming; log lines only.
#include "FarCry5MeleeDmgWatch.h"

#include <Windows.h>
#include <TlHelp32.h>
#include <spdlog/spdlog.h>
#include <bddisasm.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

namespace fc5::meleedmgwatch
{
    namespace
    {
        constexpr int      kSlots     = 4;
        constexpr int64_t  kWatchMs   = 3000;
        constexpr size_t   kEntBytes  = 0x800, kCompBytes = 0x200, kHBytes = 0x400;
        constexpr int      kRet       = 12;
        // Probe 2 (melee fix 14, 16 Nov): the writer is FC_m64+0x108e340 (H's vtable +0x30: health = min(max, value)), called with r8 = the
        // damage event from the health component's damage routine +0x1119C70 (call at +0x111b124). At the trap r8 still holds the event (the
        // setter does not touch r8 before the store), so the handler copies it; and a real unwind (the module's own unwind data) replaces the
        // stack-word scan, whose 12 words were mostly stale (allocator code, 0x5a31859/0x5abf83d).
        constexpr size_t   kEvBytes   = 0x200;
        constexpr int      kUw        = 24;
        constexpr uint64_t kStimEvVt  = 0x42BEF30;   // the damage event class SendPierceStimWithOrigin (+0x2FB6760) builds

        int64_t mono_ms() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

        std::atomic<bool>    g_on{ false }, g_failed{ false };
        uint8_t*             g_base = nullptr;
        uint64_t             g_lo = 0, g_hi = 0;   // FC_m64.dll's image
        bool                 g_code_ok = false, g_code_checked = false;
        PVOID                g_veh = nullptr;
        std::atomic<DWORD>   g_script_tid{ 0 }, g_worker_tid{ 0 };
        std::atomic<int64_t> g_tick_begin_ms{ 0 }, g_tick_end_ms{ 0 };
        std::atomic<uint64_t> g_ticks{ 0 };
        thread_local bool    t_in_tick = false;

        // The watch (written by the probe's thread before arming; read by the handler).
        uint64_t             g_addr[kSlots]{};
        std::atomic<bool>    g_armed{ false };
        std::atomic<bool>    g_taken{ false };
        std::atomic<int>     g_more{ 0 };   // writes after the first (other threads before they were cleared)
        HANDLE               g_ev_arm = nullptr, g_ev_hit = nullptr, g_ev_landed = nullptr;

        struct Rec
        {
            DWORD    tid;
            int      slot;
            uint64_t rip, rsp;
            uint64_t rax, rbx, rcx, rdx, rsi, rdi, r8, r9;
            uint8_t  code[16];
            bool     code_ok;
            uint32_t value;
            int64_t  t_ms;
            bool     in_tick;
            uint64_t ticks;
            uint64_t ret_call[kRet];
            int      n_call;
            uint64_t ret_raw[kRet];
            int      n_raw;
            uint64_t ev_addr;          // r8 at the trap: the damage event
            uint8_t  ev[kEvBytes];
            bool     ev_ok;
            uint64_t uw_rip[kUw], uw_rsp[kUw];
            int      n_uw;
            int      uw_stop;          // 0 frames ran out, 1 left the stack, 2 rip 0, 3 fault
            uint64_t stk_lo, stk_hi;
            int64_t  tick_begin, tick_end;   // the mod's last script tick, read at the trap (probe 1 read them at log time, 150+ ms later: its "-172 ms")
        };
        Rec g_rec{};

        // The job (script thread -> probe's thread).
        struct Job
        {
            uint64_t entity;
            float    health;
            int64_t  press_ms, contact_ms, arm_ms;
            uint64_t ticks0;
            uint8_t *ent, *comp, *h;
            int      n;
            uint64_t addr[kSlots];
            uint32_t old_bits[kSlots];
            const char* where[kSlots];
            uint32_t off[kSlots];
            uint8_t  s_ent[kEntBytes], s_comp[kCompBytes], s_h[kHBytes];
            bool     ok_ent, ok_comp, ok_h;
        };
        std::mutex        g_job_mx;
        Job               g_job{};
        std::atomic<bool> g_busy{ false };
        int64_t           g_last_press = 0;
        struct Landed { bool any; float a, b; bool changed; int64_t t_ms; };
        Landed            g_landed{};
        std::string       g_last;   // status
        int               g_watches = 0, g_hits = 0;

        bool in_fc(uint64_t v) { return v >= g_lo && v < g_hi; }

        // --- the vectored handler: no lock, no allocation, no API call but the clock. ---
        bool read_qw(const uint64_t* p, uint64_t& v)
        {
            __try { v = *p; return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }
        bool read_bytes(const uint8_t* p, uint8_t* out, size_t n)
        {
            __try { for (size_t i = 0; i < n; ++i) out[i] = p[i]; return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }
        // A return address: a call instruction ends just before it (E8 rel32, or FF /2 of 2 to 7 bytes).
        bool after_call(uint64_t v)
        {
            uint8_t b[8]{};
            if (!read_bytes(reinterpret_cast<const uint8_t*>(v - 8), b, 8)) return false;
            if (b[3] == 0xE8) return true;   // v-5
            for (int len = 2; len <= 7; ++len) {
                const uint8_t op = b[8 - len], modrm = b[8 - len + 1];
                if (op == 0xFF && ((modrm >> 3) & 7) == 2) return true;
            }
            return false;
        }

        // A real unwind from the trap's context with the modules' own unwind data (what the system's exception dispatch does itself on this
        // thread): no allocation, no lock but the loader's function-table read. Stops at the thread's stack limits, a zero rip, or any fault.
        alignas(16) CONTEXT g_uw_ctx;   // one trap at a time (g_taken)
        void unwind(const CONTEXT* c0, Rec& r)
        {
            const NT_TIB* tib = reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
            r.stk_lo = reinterpret_cast<uint64_t>(tib->StackLimit);
            r.stk_hi = reinterpret_cast<uint64_t>(tib->StackBase);
            r.n_uw = 0; r.uw_stop = 0;
            __try {
                std::memcpy(&g_uw_ctx, c0, sizeof(CONTEXT));
                CONTEXT& c = g_uw_ctx;
                for (int i = 0; i < kUw; ++i) {
                    if (c.Rsp < r.stk_lo || c.Rsp >= r.stk_hi) { r.uw_stop = 1; return; }
                    if (c.Rip == 0) { r.uw_stop = 2; return; }
                    r.uw_rip[r.n_uw] = c.Rip; r.uw_rsp[r.n_uw] = c.Rsp; ++r.n_uw;
                    DWORD64 image = 0;
                    PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(c.Rip, &image, nullptr);
                    if (!f) {   // a leaf: the return address is at rsp
                        c.Rip = *reinterpret_cast<const DWORD64*>(c.Rsp);
                        c.Rsp += 8;
                    } else {
                        PVOID   hd = nullptr;
                        DWORD64 ef = 0;
                        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, c.Rip, f, &c, &hd, &ef, nullptr);
                    }
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) { r.uw_stop = 3; }
        }

        LONG CALLBACK on_exception(EXCEPTION_POINTERS* ep)
        {
            if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
            CONTEXT*      c   = ep->ContextRecord;
            const DWORD64 dr6 = c->Dr6;
            const DWORD64 drs[kSlots]{ c->Dr0, c->Dr1, c->Dr2, c->Dr3 };
            int           slot = -1;
            for (int s = 0; s < kSlots; ++s) {
                if ((dr6 & (1ull << s)) && g_addr[s] != 0 && drs[s] == g_addr[s]) slot = s;
            }
            if (slot < 0) return EXCEPTION_CONTINUE_SEARCH;   // not ours (camwatch, a debugger, a single step)
            // This thread stops watching at once (armed or not: a late trap after the clear is still ours and must not reach the game).
            for (int s = 0; s < kSlots; ++s) {
                if (g_addr[s] == 0) continue;
                c->Dr7 &= ~((3ull << (s * 2)) | (0xFull << (16 + s * 4)));
            }
            c->Dr6 = 0;
            if (!g_armed.load()) return EXCEPTION_CONTINUE_EXECUTION;
            if (g_taken.exchange(true)) {
                g_more.fetch_add(1);
                return EXCEPTION_CONTINUE_EXECUTION;
            }
            Rec& r = g_rec;
            r.tid  = GetCurrentThreadId();   // a TEB read, no system call
            r.slot = slot;
            r.rip = c->Rip; r.rsp = c->Rsp;
            r.rax = c->Rax; r.rbx = c->Rbx; r.rcx = c->Rcx; r.rdx = c->Rdx; r.rsi = c->Rsi; r.rdi = c->Rdi; r.r8 = c->R8; r.r9 = c->R9;
            r.code_ok = read_bytes(reinterpret_cast<const uint8_t*>(c->Rip - 16), r.code, 16);
            uint64_t v = 0;
            r.value   = read_qw(reinterpret_cast<const uint64_t*>(g_addr[slot]), v) ? uint32_t(v) : 0xFFFFFFFFu;
            r.t_ms    = mono_ms();
            r.in_tick = t_in_tick;
            r.tick_begin = g_tick_begin_ms.load();
            r.tick_end   = g_tick_end_ms.load();
            r.ticks   = g_ticks.load();
            r.n_call = 0; r.n_raw = 0;
            const uint64_t* sp = reinterpret_cast<const uint64_t*>(c->Rsp);
            for (int k = 0; k < 2048 && (r.n_call < kRet || r.n_raw < kRet); ++k) {
                uint64_t w = 0;
                if (!read_qw(sp + k, w)) break;
                if (!in_fc(w)) continue;
                if (r.n_raw < kRet) r.ret_raw[r.n_raw++] = w;
                if (r.n_call < kRet && after_call(w)) r.ret_call[r.n_call++] = w;
            }
            // probe 2: the damage event (r8) and the real caller chain
            r.ev_addr = c->R8;
            r.ev_ok   = c->R8 > 0x10000 && read_bytes(reinterpret_cast<const uint8_t*>(c->R8), r.ev, kEvBytes);
            unwind(c, r);
            if (g_ev_hit) SetEvent(g_ev_hit);
            return EXCEPTION_CONTINUE_EXECUTION;
        }

        // --- the probe's thread ---
        // n > 0: g_addr[0..n) set on every thread but this one; n == 0: our slots cleared. busy: another user's debug registers met (nothing set).
        int apply(int n, bool& busy)
        {
            busy = false;
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            if (snap == INVALID_HANDLE_VALUE) return -1;
            THREADENTRY32 te{ sizeof(te) };
            const DWORD   pid = GetCurrentProcessId(), self = GetCurrentThreadId();
            int           done = 0;
            for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
                if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
                HANDLE th = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
                if (!th) continue;
                if (SuspendThread(th) != DWORD(-1)) {
                    CONTEXT c{};
                    c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
                    if (GetThreadContext(th, &c)) {
                        if (n > 0 && (c.Dr7 & 0xFF) != 0) {
                            busy = true;
                        } else {
                            DWORD64* dr[kSlots]{ &c.Dr0, &c.Dr1, &c.Dr2, &c.Dr3 };
                            for (int s = 0; s < kSlots; ++s) {
                                const DWORD64 bits = (3ull << (s * 2)) | (0xFull << (16 + s * 4));
                                if (n > 0 && s < n) {
                                    *dr[s] = g_addr[s];
                                    c.Dr7 = (c.Dr7 & ~bits) | (1ull << (s * 2)) | (0b01ull << (16 + s * 4)) | (0b11ull << (18 + s * 4));   // local, write, 4 bytes
                                } else if (n == 0 && g_addr[s] != 0 && *dr[s] == g_addr[s]) {
                                    *dr[s] = 0;   // only our own slots are cleared
                                    c.Dr7 &= ~bits;
                                }
                            }
                            c.Dr6 = 0;
                            if (SetThreadContext(th, &c)) ++done;
                        }
                    }
                    ResumeThread(th);
                }
                CloseHandle(th);
                if (busy) break;
            }
            CloseHandle(snap);
            return done;
        }

        std::string fc(uint64_t v)
        {
            if (in_fc(v)) return fmt::format("FC_m64+0x{:x}", v - reinterpret_cast<uint64_t>(g_base));
            HMODULE m = nullptr;
            char    name[MAX_PATH]{};
            if (v > 0x10000 && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(v), &m) && m &&
                GetModuleFileNameA(m, name, MAX_PATH)) {
                const char* b = std::strrchr(name, '\\');
                return fmt::format("{}+0x{:x}", b ? b + 1 : name, v - reinterpret_cast<uint64_t>(m));
            }
            return fmt::format("0x{:x}", v);
        }

        float as_f(uint32_t b) { float f; std::memcpy(&f, &b, 4); return f; }

        // The instruction that ends at rip (the trap comes after the write): every start 1-15 bytes back that decodes to exactly that length.
        std::string writer_text(const Rec& r)
        {
            if (!r.code_ok) return "(code unreadable)";
            std::string out;
            for (int k = 15; k >= 1; --k) {
                INSTRUX ix{};
                if (!ND_SUCCESS(NdDecodeEx(&ix, r.code + 16 - k, size_t(k), ND_CODE_64, ND_DATA_64)) || ix.Length != k) continue;
                char text[ND_MIN_BUF_SIZE]{};
                NdToText(&ix, r.rip - k, sizeof(text), text);
                if (!out.empty()) out += " | ";
                out += fmt::format("{} {}", fc(r.rip - k), text);
            }
            return out.empty() ? "(no decode ends at the trap)" : out;
        }

        std::string diff(const char* name, const uint8_t* before, bool ok, const uint8_t* now_p, size_t n)
        {
            if (!ok || !now_p) return fmt::format("{}: not copied", name);
            static uint8_t now[kEntBytes];
            if (!read_bytes(now_p, now, n)) return fmt::format("{}: unreadable now", name);
            std::string out;
            int         k = 0;
            for (size_t o = 0; o + 4 <= n; o += 4) {
                uint32_t a, b;
                std::memcpy(&a, before + o, 4);
                std::memcpy(&b, now + o, 4);
                if (a == b) continue;
                if (++k <= 32) out += fmt::format(" +0x{:x} {:08x}->{:08x} ({:g}->{:g})", o, a, b, as_f(a), as_f(b));
            }
            return fmt::format("{} {}: {} dwords changed{}{}", name, static_cast<const void*>(now_p), k, k > 32 ? " (first 32)" : "", out);
        }

        // probe 2's lines: the event's named fields (from the stim builder +0x2FB6760 and the damage routine +0x1119C70), every non-zero dword,
        // and the unwound chain with the frame that holds the event.
        std::string event_text(const Rec& r)
        {
            if (!r.ev_ok) return fmt::format("the damage event (r8 0x{:x}): unreadable", r.ev_addr);
            auto u32 = [&](size_t o) { uint32_t v; std::memcpy(&v, r.ev + o, 4); return v; };
            auto u64 = [&](size_t o) { uint64_t v; std::memcpy(&v, r.ev + o, 8); return v; };
            const uint64_t vt  = u64(0);
            const uint64_t rel = in_fc(vt) ? vt - reinterpret_cast<uint64_t>(g_base) : 0;
            std::string out = fmt::format("the damage event (r8 0x{:x}, {}): class {}{}; type +0x5c {} (the stim sends 3), +0x60 {}, +0x58 {:08x} (the stim's "
                                          "0xab3fb98a), +0xac {:x}, +0x30 ({:g}, {:g}, {:g}), +0x40 handle {:x}, +0x48 handle {:x}, +0x50 handle {:x}; dwords:",
                                          r.ev_addr, r.ev_addr >= r.stk_lo && r.ev_addr < r.stk_hi ? "on the writer's stack" : "not on the stack", fc(vt),
                                          rel == kStimEvVt ? " = the stim's class" : "", u32(0x5c), u32(0x60), u32(0x58), u32(0xac), as_f(u32(0x30)), as_f(u32(0x34)),
                                          as_f(u32(0x38)), u64(0x40), u64(0x48), u64(0x50));
            for (size_t o = 0; o + 4 <= kEvBytes; o += 4) {
                const uint32_t v = u32(o);
                if (v == 0) continue;
                out += fmt::format(" +0x{:x} {:08x}", o, v);
                const float f = as_f(v);
                if (f == f && std::fabs(f) > 1e-4f && std::fabs(f) < 1e7f) out += fmt::format(" ({:g})", f);
                if ((o & 7) == 0 && in_fc(u64(o))) out += fmt::format(" [{}]", fc(u64(o)));
            }
            return out;
        }
        std::string unwind_text(const Rec& r)
        {
            static const char* stop[]{ "frames ran out", "left the stack", "rip 0", "fault" };
            std::string out = fmt::format("unwound chain ({} frames, {}):", r.n_uw, stop[r.uw_stop & 3]);
            for (int i = 0; i < r.n_uw; ++i) {
                const bool holds = r.ev_ok && i + 1 < r.n_uw && r.ev_addr >= r.uw_rsp[i] && r.ev_addr < r.uw_rsp[i + 1];
                out += fmt::format(" {}{}", fc(r.uw_rip[i]), holds ? " [the event is in this frame]" : "");
            }
            return out;
        }

        void worker()
        {
            g_worker_tid = GetCurrentThreadId();
            for (;;) {
                WaitForSingleObject(g_ev_arm, INFINITE);
                if (!g_on.load() || g_failed.load()) { g_busy = false; continue; }
                Job* j = &g_job;   // owned by this thread until g_busy is cleared
                std::memcpy(g_addr, j->addr, sizeof(g_addr));
                g_taken = false;
                g_more  = 0;
                ResetEvent(g_ev_hit);
                ResetEvent(g_ev_landed);
                {
                    std::lock_guard _{ g_job_mx };
                    g_landed = {};
                }
                int64_t t0 = mono_ms();
                bool    busy = false;
                int     threads = 0;
                if (j->n > 0) {
                    g_armed = true;
                    threads = apply(j->n, busy);
                    if (busy || threads <= 0) {
                        g_armed = false;
                        bool b2 = false;
                        apply(0, b2);
                        spdlog::error("[MeleeDmgWatch] {}: probe turned off", busy ? "the debug registers are already in use (camwatch or a debugger)" : "no thread could be watched");
                        g_failed = true;
                        g_on     = false;
                        g_busy   = false;
                        continue;
                    }
                }
                ++g_watches;
                std::string addrs;
                for (int s = 0; s < j->n; ++s) addrs += fmt::format("{}{} {}+0x{:x} = {:08x} ({:g})", s ? ", " : "", fmt::ptr(reinterpret_cast<void*>(j->addr[s])), j->where[s], j->off[s], j->old_bits[s], as_f(j->old_bits[s]));
                spdlog::info("[MeleeDmgWatch] watch {} set: entity {:x} health {:.1f} (ent {}, comp {}, health object {}): {} on {} threads, {} ms to set, {} ms after the press, {} ms after the contact",
                             g_watches, j->entity, j->health, fmt::ptr(j->ent), fmt::ptr(j->comp), fmt::ptr(j->h), j->n ? addrs : std::string{ "no address (the health's float is in none of the three: the copies are compared at the end)" },
                             threads, mono_ms() - t0, t0 - j->press_ms, t0 - j->contact_ms);
                // Until the write, the damage line, or 3 s.
                const int64_t deadline = j->arm_ms + kWatchMs;
                HANDLE        evs[2]{ g_ev_hit, g_ev_landed };
                DWORD         wr = WAIT_TIMEOUT;
                for (;;) {
                    const int64_t left = deadline - mono_ms();
                    if (left <= 0) break;
                    wr = WaitForMultipleObjects(2, evs, FALSE, DWORD(left));
                    if (wr == WAIT_OBJECT_0) break;
                    if (wr == WAIT_OBJECT_0 + 1) {
                        // the damage line: a change with no watched write means the address is not the field; no change: keep watching to 3 s
                        Landed l;
                        { std::lock_guard _{ g_job_mx }; l = g_landed; }
                        if (l.changed) { Sleep(50); if (WaitForSingleObject(g_ev_hit, 0) == WAIT_OBJECT_0) wr = WAIT_OBJECT_0; break; }
                        ResetEvent(g_ev_landed);
                        wr = WAIT_TIMEOUT;
                    }
                }
                const bool hit = g_taken.load() && wr == WAIT_OBJECT_0;
                if (hit) Sleep(150);   // the blow's other writes (for the comparison)
                g_armed = false;
                if (j->n > 0) {
                    bool b2 = false;
                    const int cleared = apply(0, b2);
                    if (cleared <= 0) spdlog::warn("[MeleeDmgWatch] clearing the watch reached no thread");
                }
                Landed l;
                { std::lock_guard _{ g_job_mx }; l = g_landed; }
                std::string landed = l.any ? fmt::format("the damage line: {:.1f} -> {:.1f}{} at +{} ms", l.a, l.b, l.changed ? "" : " (no change)", l.t_ms - j->press_ms) : std::string{ "no damage line yet" };
                if (hit) {
                    ++g_hits;
                    const Rec& r = g_rec;
                    const DWORD st = g_script_tid.load();
                    std::string calls, raw;
                    for (int i = 0; i < r.n_call; ++i) calls += fmt::format("{}{}", i ? " " : "", fc(r.ret_call[i]));
                    for (int i = 0; i < r.n_raw; ++i) raw += fmt::format("{}{}", i ? " " : "", fc(r.ret_raw[i]));
                    const uint32_t oldv = j->old_bits[r.slot];
                    spdlog::info("[MeleeDmgWatch] WRITE watch {}: {} (slot {}, {}+0x{:x}) {:08x} -> {:08x} ({:g} -> {:g}) by the instruction before {} on thread {} ({}{}), {} ms after the press, "
                                 "{} ms after the contact, {} ms after the script tick's start, {} ms after its end ({} the tick: {} ticks since the arm); {}; {} more writes before the clear",
                                 g_watches, fmt::ptr(reinterpret_cast<void*>(j->addr[r.slot])), r.slot, j->where[r.slot], j->off[r.slot], oldv, r.value, as_f(oldv), as_f(r.value), fc(r.rip), r.tid,
                                 r.tid == st ? "the game's script thread" : (r.tid == g_worker_tid.load() ? "the probe's own thread" : "not the script thread"), st ? "" : " (script thread not seen yet)",
                                 r.t_ms - j->press_ms, r.t_ms - j->contact_ms, r.t_ms - r.tick_begin, r.t_ms - r.tick_end, r.in_tick ? "inside" : "outside", r.ticks - j->ticks0, landed, g_more.load());
                    spdlog::info("[MeleeDmgWatch]   the writer: {}", writer_text(r));
                    spdlog::info("[MeleeDmgWatch]   registers: rax {:x} rbx {:x} rcx {:x} rdx {:x} rsi {:x} rdi {:x} r8 {:x} r9 {:x} rsp {:x}", r.rax, r.rbx, r.rcx, r.rdx, r.rsi, r.rdi, r.r8, r.r9, r.rsp);
                    spdlog::info("[MeleeDmgWatch]   return addresses after a call ({}): {}", r.n_call, calls.empty() ? "none" : calls);
                    spdlog::info("[MeleeDmgWatch]   FC_m64 words on the stack ({}): {}", r.n_raw, raw.empty() ? "none" : raw);
                    spdlog::info("[MeleeDmgWatch]   {}", unwind_text(r));
                    spdlog::info("[MeleeDmgWatch]   {}", event_text(r));
                    g_last = fmt::format("watch {}: written by {} on thread {}", g_watches, fc(r.rip), r.tid);
                } else {
                    spdlog::info("[MeleeDmgWatch] watch {}: {} ({}); cleared {} ms after the arm", g_watches,
                                 j->n == 0 ? "nothing watched" : (l.any && l.changed ? "the health changed but no watched address was written: not the field" : "no write in the watch"), landed, mono_ms() - j->arm_ms);
                    g_last = fmt::format("watch {}: no write", g_watches);
                }
                spdlog::info("[MeleeDmgWatch]   {}", diff("health object", j->s_h, j->ok_h, j->h, kHBytes));
                spdlog::info("[MeleeDmgWatch]   {}", diff("component", j->s_comp, j->ok_comp, j->comp, kCompBytes));
                spdlog::info("[MeleeDmgWatch]   {}", diff("entity", j->s_ent, j->ok_ent, j->ent, kEntBytes));
                g_busy = false;
            }
        }

        // --- script thread: the health object as the native finds it ---
        using LookupFn  = void(__fastcall*)(void* mgr, uint8_t** h, uint64_t id);
        using KeyFn     = uint8_t*(__fastcall*)();
        using CompAFn   = uint8_t*(__fastcall*)(uint8_t* c, uint32_t* key, uint8_t* ent_a8);
        using CompBFn   = uint8_t*(__fastcall*)(uint8_t* ent, uint32_t* key);
        using GetFn     = float(__fastcall*)(uint8_t* h);
        using NativeFn  = float(__fastcall*)(uint64_t* id);
        using ReleaseFn = void(__fastcall*)(uint8_t* handle);

        // 1 found, 0 none (no entity or component), -1 a fault.
        int find_raw(uint64_t id, uint8_t*& ent, uint8_t*& comp, uint8_t*& h, float& got, float& native)
        {
            __try {
                ent = comp = h = nullptr;
                uint64_t idc = id;
                native       = reinterpret_cast<NativeFn>(g_base + 0x2FA2900)(&idc);
                uint8_t* hd  = nullptr;
                reinterpret_cast<LookupFn>(g_base + 0xB27D60)(*reinterpret_cast<void**>(g_base + 0x4FAC1E0), &hd, id);
                if (!hd) return 0;
                int r = 0;
                ent   = *reinterpret_cast<uint8_t**>(hd + 0x10);
                if (ent) {
                    uint8_t* c   = *reinterpret_cast<uint8_t**>(ent + 0xC8);
                    uint32_t key = *reinterpret_cast<uint32_t*>(reinterpret_cast<KeyFn>(g_base + 0xEF87C0)() + 0x20);
                    comp         = c ? reinterpret_cast<CompAFn>(g_base + 0xCCCFF0)(c, &key, ent + 0xA8) : reinterpret_cast<CompBFn>(g_base + 0xCCD250)(ent, &key);
                    if (comp) {
                        h = *reinterpret_cast<uint8_t**>(comp + 0x48);
                        if (h) {
                            got = reinterpret_cast<GetFn>(g_base + 0xBD8230)(h);
                            r   = 1;
                        }
                    }
                }
                if (hd != g_base + 0x4CB9E70 && InterlockedDecrement(reinterpret_cast<volatile LONG*>(hd + 8)) == 0) reinterpret_cast<ReleaseFn>(g_base + 0x26E320)(hd);
                return r;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return -1;
            }
        }

        bool check_code()
        {
            if (g_code_checked) return g_code_ok;
            g_code_checked = true;
            g_base         = reinterpret_cast<uint8_t*>(GetModuleHandleA("FC_m64.dll"));
            if (!g_base) return false;
            const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(g_base);
            const auto* nt  = reinterpret_cast<const IMAGE_NT_HEADERS64*>(g_base + dos->e_lfanew);
            g_lo            = reinterpret_cast<uint64_t>(g_base);
            g_hi            = g_lo + nt->OptionalHeader.SizeOfImage;
            // GetPawnHealth's native, +0x2FA2924 to +0x2FA298C, as read with dumpbin (the calls replayed above, in this order).
            static const uint8_t kNative[] = { 0x48, 0x8B, 0x5C, 0x24, 0x60, 0x48, 0x8B, 0x7B, 0x10, 0x48, 0x85, 0xFF, 0x74, 0x5C, 0x48, 0x89, 0x74, 0x24, 0x38, 0x48, 0x8B, 0xB7, 0xC8, 0x00,
                                               0x00, 0x00, 0xE8, 0x7D, 0x5E, 0xF5, 0xFD, 0x8B, 0x50, 0x20, 0x48, 0x85, 0xF6, 0x74, 0x1A, 0x89, 0x54, 0x24, 0x50, 0x4C, 0x8D, 0x87, 0xA8, 0x00,
                                               0x00, 0x00, 0x48, 0x8D, 0x54, 0x24, 0x50, 0x48, 0x8B, 0xCE, 0xE8, 0x8D, 0xA6, 0xD2, 0xFD, 0xEB, 0x11, 0x89, 0x54, 0x24, 0x58, 0x48, 0x8B, 0xCF,
                                               0x48, 0x8D, 0x54, 0x24, 0x58, 0xE8, 0xDA, 0xA8, 0xD2, 0xFD, 0x48, 0x8B, 0x74, 0x24, 0x38, 0x48, 0x85, 0xC0, 0x74, 0x0E, 0x48, 0x8B, 0x48, 0x48,
                                               0xE8, 0xA7, 0x58, 0xC3, 0xFD };
            static const uint8_t kHead[] = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x40, 0x4C, 0x8B, 0x01, 0x48, 0x8D, 0x54, 0x24, 0x60, 0x48, 0x8B, 0x0D, 0xCB, 0x98, 0x00, 0x02 };
            const bool head = std::memcmp(g_base + 0x2FA2900, kHead, sizeof(kHead)) == 0;
            const bool body = std::memcmp(g_base + 0x2FA2924, kNative, sizeof(kNative)) == 0;
            const bool jmps = g_base[0xB27D60] == 0xE9 && g_base[0xEF87C0] == 0xE9 && g_base[0xCCCFF0] == 0xE9 && g_base[0xCCD250] == 0xE9 && g_base[0xBD8230] == 0xE9 && g_base[0x26E320] != 0xCC;
            g_code_ok = head && body && jmps;
            if (!g_code_ok) spdlog::error("[MeleeDmgWatch] unexpected code (native head {}, body {}, helpers {}): not used", head, body, jmps);
            return g_code_ok;
        }

        // The float equal to the health in a region: aligned dwords only (a 4-byte debug watch needs it).
        void scan(Job& j, const uint8_t* copy, bool ok, uint8_t* base, size_t n, const char* name, uint32_t bits)
        {
            if (!ok || !base) return;
            const size_t start = (4 - (reinterpret_cast<uintptr_t>(base) & 3)) & 3;
            for (size_t o = start; o + 4 <= n && j.n < kSlots; o += 4) {
                uint32_t v;
                std::memcpy(&v, copy + o, 4);
                if (v != bits) continue;
                j.addr[j.n] = reinterpret_cast<uint64_t>(base + o);
                j.old_bits[j.n] = v;
                j.where[j.n] = name;
                j.off[j.n] = uint32_t(o);
                ++j.n;
            }
        }
    }

    void set_on(bool on)
    {
        if (on) {
            if (g_failed.load()) { spdlog::info("[MeleeDmgWatch] turned off after a fault this session: not turned on again"); return; }
            if (!check_code()) return;
            if (!g_ev_arm) {
                g_ev_arm    = CreateEventA(nullptr, FALSE, FALSE, nullptr);
                g_ev_hit    = CreateEventA(nullptr, TRUE, FALSE, nullptr);
                g_ev_landed = CreateEventA(nullptr, TRUE, FALSE, nullptr);
                if (!g_ev_arm || !g_ev_hit || !g_ev_landed) { spdlog::error("[MeleeDmgWatch] no events: not used"); g_failed = true; return; }
                std::thread(worker).detach();
            }
            if (!g_veh) g_veh = AddVectoredExceptionHandler(1, &on_exception);
            if (!g_veh) { spdlog::error("[MeleeDmgWatch] no exception handler: not used"); return; }
        }
        g_on = on;
        spdlog::info("[MeleeDmgWatch] meleedmgwatch {} (test, not saved): {}", on ? "on" : "off",
                     on ? "after each press of the game's melee at a contact, a write watch on the hit NPC's health for at most 3 s" : "no new watch");
    }

    bool on() { return g_on.load(); }

    std::string status()
    {
        return fmt::format("meleedmgwatch {}{}: {} watches, {} writes caught; {}", g_on.load() ? "on" : "off", g_failed.load() ? " (turned off after a fault)" : "", g_watches, g_hits,
                           g_last.empty() ? std::string{ "none yet" } : g_last);
    }

    void tick_begin()
    {
        t_in_tick = true;
        g_script_tid = GetCurrentThreadId();
        g_tick_begin_ms = mono_ms();
        g_ticks.fetch_add(1, std::memory_order_relaxed);
    }

    void tick_end()
    {
        t_in_tick     = false;
        g_tick_end_ms = mono_ms();
    }

    void arm(uint64_t entity, float health, int64_t press_ms, int64_t contact_ms)
    {
        if (!g_on.load() || g_failed.load() || entity == 0 || press_ms == g_last_press) return;
        g_last_press = press_ms;
        if (health <= 0.0f) { spdlog::info("[MeleeDmgWatch] entity {:x} health {:.1f}: dead, not watched", entity, health); return; }
        if (g_busy.load()) { spdlog::info("[MeleeDmgWatch] entity {:x}: the previous watch is still on (one at a time): not watched", entity); return; }
        Job&     j = g_job;   // not in use: g_busy is false
        uint8_t *ent = nullptr, *comp = nullptr, *h = nullptr;
        float    got = 0.0f, native = 0.0f;
        const int r = find_raw(entity, ent, comp, h, got, native);
        if (r < 0) {
            spdlog::error("[MeleeDmgWatch] a fault finding entity {:x}'s health object: probe turned off", entity);
            g_failed = true;
            g_on     = false;
            return;
        }
        if (r == 0) { spdlog::info("[MeleeDmgWatch] entity {:x}: no health object (ent {}, comp {}): not watched", entity, fmt::ptr(ent), fmt::ptr(comp)); return; }
        uint32_t bits_n, bits_g;
        std::memcpy(&bits_n, &native, 4);
        std::memcpy(&bits_g, &got, 4);
        if (bits_n != bits_g) {
            spdlog::info("[MeleeDmgWatch] entity {:x}: the health object's getter gives {:g}, the native {:g}: not the same object, not watched", entity, got, native);
            return;
        }
        j        = Job{};
        j.entity = entity;
        j.health = native;
        j.press_ms = press_ms;
        j.contact_ms = contact_ms;
        j.ent = ent; j.comp = comp; j.h = h;
        j.ok_ent  = read_bytes(ent, j.s_ent, kEntBytes);
        j.ok_comp = read_bytes(comp, j.s_comp, kCompBytes);
        j.ok_h    = read_bytes(h, j.s_h, kHBytes);
        scan(j, j.s_h, j.ok_h, h, kHBytes, "health object", bits_n);
        scan(j, j.s_comp, j.ok_comp, comp, kCompBytes, "component", bits_n);
        scan(j, j.s_ent, j.ok_ent, ent, kEntBytes, "entity", bits_n);
        j.arm_ms = mono_ms();
        j.ticks0 = g_ticks.load();
        g_taken  = false;   // melee fix 14b: waiting() true from now (the worker resets it too, later)
        g_busy   = true;
        SetEvent(g_ev_arm);
    }

    bool waiting() { return g_on.load() && g_busy.load() && !g_taken.load(); }

    void landed(uint64_t entity, float a, float b, bool changed)
    {
        if (!g_busy.load() || entity != g_job.entity) return;
        {
            std::lock_guard _{ g_job_mx };
            g_landed = Landed{ true, a, b, changed, mono_ms() };
        }
        if (g_ev_landed) SetEvent(g_ev_landed);
    }
}
