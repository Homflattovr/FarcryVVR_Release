#pragma once

// sessionlogs on|off (saved, default on for now; F11 Main and the launcher: "Logs during sessions (slower)"; author 22 Nov 12:36Z). One switch over what the mod writes and measures while you play:
//   on   as before: every line, and each probe as its own switch says.
//   off  from the first play of the session on (the first switch to 3D), the log keeps only warnings and errors: every info line (the
//        per-second [Probe] block, the 5 s and 10 s summaries such as [Perf], [FC5 stats], [GpuTime], [LOD], [Cull], [View], [Bob],
//        [GunStill], [Handling], [PoseTime], [Mem], and each event line) is dropped before it is formatted, and the log file is no longer
//        written several times a second. The default-on probes that cost more than their lines stop too: gputime (GPU timestamp
//        queries every pair), memwatch's minute line (its process walk), partwatch (each weapon-pass draw followed) and chuteprobe (each
//        draw near the camera while falling). Their own switches are kept as they are and act again once sessionlogs is on.
// Always kept, either way: everything up to the first play (the boot steps, the boot header, the settings replayed, the headset, DLSS and
// resolution notes), every warning and error, the crash report and dump (fc5vr_logs, written without spdlog), the boot watchdog and the
// freeze watchdog's stacks (errors), and the launcher's report zip. A tester's log still shows the start and every failure.
namespace fc5::sessionlogs
{
    bool on();                // the switch (default on)
    void set(bool on);        // sessionlogs on|off (the settings at start, fc5vr_cmd.txt, F11)
    void play_started();      // the first play of the session (FarCry5Stereo: the first switch to 3D): off takes effect from here
    bool quiet();             // off and past the first play: only warnings and errors reach the log
}
