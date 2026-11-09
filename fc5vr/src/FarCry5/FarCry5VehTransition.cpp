// Getting in and out of vehicles, comfort (author 23 Oct, after the 12:04 headset run). His log: each entry turned the view
// ~40 deg/s for 2 s, each exit added a seat-change settle and recentre 0.6 s before the real one, and bailing out of the
// plane turned the view ~180 deg in half a second. With vehentryfollow the view's heading was held on the game camera's
// during the animation (m_body_yaw_pending = camera - (body + head)): turning the head did not turn the view for 1-3 s.
//  - vehfollowhead on|off (default on, saved; F11 Comfort > Cinematics and vehicles): the body frame turns by the animation
//    camera's own turn since the settle started; the head still turns the view on top. Getting out, the exit ends with the
//    centre moved under the head (no turn of the view: on foot the aim loop brings the game camera to the head).
//  - vehfollowsnap on|off (default on, saved): that turn in snaps of the snap-turn angle (15-45 deg, at most one per 0.25 s),
//    like a snap turn, instead of a smooth artificial turn; the eyes stay within that angle of the camera the game culls with.
//  - vehseatguard on|off (default on, saved): a seat-change settle is not finished while no vehicle is under the camera
//    (update_vehicle); getting out has one settle, not two.
//  - vehseathold on|off (default on, saved; F11 Comfort > Cinematics and vehicles; author 28 Oct 22:17). His 22:15 log: in the boat the mod went from "in a vehicle" to "on foot again" every ~4 s for a minute. The
//    boat's model is 3 m from the driver's camera, outside vehicle_probe's window (0.8 m aside, 0.3-2.2 m below), so only the hands'
//    pass told in from out; the game drew the sidearm (9282, 24417) ~10 ms after each settle and put it away 3 s later (4110), and
//    each time the mod went on foot: the right trigger held for the aim and cut by the sling, so no throttle. Now, when a settle
//    ends, the seat is learnt on the vehicle's model (the Vehicle_ draw within 8 m, the camera in its axes); while the camera stays
//    within vehseatholdm (default 0.6 m, saved) of that seat the player is in that vehicle, and one who settled driving stays driving
//    with a weapon out (the game's vehicle fire is LB; the trigger stays the throttle). A seat change or getting out moves the camera
//    off the seat and goes as before. Off: as before 28 Oct.
//  - vehtriggers left|right (default left, saved; F11 Layout; author 29 Oct 10:19). The game's driver fire is LB (the sidearm drawn and fired, or the vehicle's
//    guns), which the mod put on the left trigger, while the right trigger, the gun hand's, stayed the throttle and the right grip the
//    brake: in his 10:12 drive he pressed B (the game drew the pistol 24417 for 3 s), never the left trigger, and the right trigger kept
//    accelerating. left: driving (any vehicle, planes and helicopters too: RT throttle up / ascend, LT throttle down / descend, LB guns),
//    the left hand drives and the right hand shoots: right trigger LB, left trigger RT, left grip LT, right grip nothing. The left grip
//    stays the modifier for the right stick (D-pad: radio, companions, throwables), right B (RB: throw, rockets) and the menu button
//    (map); a grip press used for one of those stops braking until let go, and the left stick keeps steering while the grip brakes.
//    Passengers and on foot are unchanged. A trigger or left grip held when the layout changes (getting in or out, a seat change) does
//    nothing until it is let go. right: as before 29 Oct.
//  - vehdrivegun on|off (default on, saved; F11 Comfort > Cinematics and vehicles; author 30 Oct 09:11). His right trigger during the quad's entry went to the game as LB, the game drew the driver's sidearm, and the weapon view
//    coming back was taken for a passenger seat (the entry settled "riding", vehseathold learnt it so, the left trigger was LB for 6 s).
//    A weapon view that shows while driving within 1.5 s of that LB (or while held, or after one sent during this vehicle's entry) is the
//    driver's sidearm: latched until the vehicle is left (update_vehicle). Before that rule, the game's own answer (vehscript, the author's 09:27
//    log read by the engine natives thread: GetPawnVehicleSeat counts from 1 and reads 0 during the entry animation, the driver's seat lookup
//    counts from 0): its driver drives with a weapon out, its passenger rides; seat 0, vehscript off or silent leave it to the LB latch.
//    The game's seat, driver's seat and entity are logged on each vehtriggers change, "now driving / riding" and each change of the rule
//    that decides (game_seat_text). Off: as before.
//  - vehaimfront on|off (default on, saved; F11 Comfort > Cinematics and vehicles; 5 Nov gesture driving run, research\2026-11-05_gesturedrive
//    RESULTS_builder.md, testruns\2026-11-05_stickspeed): the settle ends with one cut of the view to the vehicle's front, but the game's own
//    aim (its camera less the head look's offset) stays where the get-in left it. After a normal get-in the game's camera already faces the
//    front (the cut logs 0 deg, the head look's offset 0), but getting in right after getting out of another vehicle (spawnvehiclein) it
//    did not: the quad settled with the game's camera at 117 deg against the front's 38 (cut +96, head look offset 79 deg for the next
//    30 s), the last car at -86 against 42 (cut +141, offset -128). The head look kept the drawn camera on the head (error 0.0) and every
//    yaw the mod logs agreed, yet 5_bars.png shows the quad's bars and dashboard about 20 deg right of straight ahead. The game camera's
//    place hangs on its aim (on foot it sits on the neck, ahead of the body root along the aim: viewpivot), and in a vehicle the eyes stand
//    on it: an aim 79 deg round most likely put the eyes ~20 cm aside of the seat, which shows as ~20 deg on the bars 60 cm away but
//    little on the far ground (its checkerboard seems to sit where a 38 deg view puts it; not measured). Now a settle that ends with its cut also turns
//    the game's aim onto the vehicle's front with the mouse, in the head look's update: measured sends of at most 60 deg (the vehicle
//    camera's degrees per count learnt from each), done within 2 deg, stopped after 3 s or 10 sends, or if the aim does not move. Only
//    while the head look runs (driving) and for the game's ground types (wheeled, boat, motorcycle: an aircraft's mouse steers it).
//    One "[Vehicle] vehaimfront" line per settle: how far the aim was, the sends, and how far the game camera moved in the vehicle's
//    axes (that number confirms or rules out the eyes-aside reading). vehaimfrontmin N (test, 5): an aim within N deg is left as it is;
//    vehaimfrontnow (test): arms it now (turn the game's camera with the right stick while parked, then this). Off: as before 5 Nov.
//  - vehheadingkeep on|off (default on, saved; F11 Comfort > Cinematics and vehicles; author 6 Nov 20:59). His 21:38-21:56 log (build de834e1), three
//    cars, all type 0 wheeled, all settled on the model's forward (vehnose):
//      * car 3, 21:49:24: in a steep turn vehfrontfix took a slide for a front learnt a quarter turn wrong ("front corrected by +90 deg:
//        travelling 16.6 m/s at 81 deg from the learned front"): the view turned onto the right window; 12 s later "-90 deg" turned it back.
//      * car 2, 21:44:47-21:47:58: the model was read under the camera until 21:44:47, then never again for 3 minutes (every heading line
//        "(travel)"): the camera sat 0.58 m aside of that model's origin at the settle, near the probe's 0.8 m window, and the game camera
//        moved about 45 cm in the car (vehseathold: 0.43-0.49 m off the learnt seat from 21:44:51 on; [ViewJump] 40-54 cm steps across).
//        The heading then came from the travel direction, whose reversing rule (over 90 deg off = reversing) keeps a heading that has come
//        round in a slide the wrong way for good: the head look's offset sat at its 150 deg limit from 21:45:00 to 21:47:37 (error up to
//        144 deg), the game's camera turned to the side or behind the car, and the eyes stood on it.
//      * and a read over 20 deg off the heading was left out as another part of the model, every frame, so a heading that had come off
//        the model in a gap never went back onto it.
//    Now: (1) the model read for the heading is also followed outside the window while the camera moves in the car (its origin within
//    0.35 m of last frame's place from the camera, under 3 m away; heading only, in / out keep the window); (2) a model read steady for 12
//    updates but over 20 deg off the heading puts the heading back on it in one cut ("[Vehicle] vehheadingkeep: the heading back on the
//    vehicle model"); (3) over 12 m/s a ground vehicle (the game's type) is going forward: no reversing flip on the travel direction;
//    (4) vehfrontfix leaves out ground vehicles while vehnose is on ("a front correction ... left out"); planes and helicopters as before.
//    (5) Night survey 6-7 Nov (26 vehicles, simulator, C3F95CAF, research note 2026-11-06_vehheadingkeep.md): a full-lock slide drove the
//    head look's offset to its 150 deg limit on every land vehicle with the mod's front right (0 front corrections, "(model)" lines):
//    the game's own aim stayed put in the world while the car turned under it (174 deg in the first car), so the culled camera sat
//    147-166 deg off the eyes, flipping sides, until the next straight run. While driving a ground vehicle, an aim over 30 deg off the
//    front for 8 head look updates now arms vehaimfront again (needs vehaimfront on; at most once per 2 s; "[Vehicle] vehheadingkeep:
//    the game's aim ... turned back").
//    One "[Vehicle] vehheadingkeep" line beside each heading line counts each rule this drive, and "[Vehicle] heading: no vehicle model
//    read" / "read again" mark the travel direction's spells (5 a drive). Off: as before 6 Nov.
// Getting in still ends with one cut to the vehicle's front and a recentre (as before); the log line "[Vehicle] settle cut"
// gives its size.
#include "FarCry5Stereo.h"
#include "FarCry5Lua.h"
#include "FarCry5CullHead.h"

#include <glm/gtc/constants.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>

bool FarCry5Stereo::vehtrans_command(const std::string& cmd, const std::string& arg, bool& persist)
{
    const bool on = arg == "on" || arg == "1" || arg == "true";
    if (cmd == "vehfollowhead") { m_veh_follow_head = on; persist = true; return true; }   // vehfollowhead on|off (saved)
    if (cmd == "vehfollowsnap") { m_veh_follow_snap = on; persist = true; return true; }   // vehfollowsnap on|off (saved)
    if (cmd == "vehseatguard") { m_veh_seat_guard = on; persist = true; return true; }     // vehseatguard on|off (saved)
    if (cmd == "vehaimfront") {                                                             // vehaimfront on|off (saved)
        m_veh_aim_front = on;
        if (!on) m_vaf_state = 0;
        spdlog::info("[Vehicle] vehaimfront {}", on ? "on (a settle also turns the game's aim onto the vehicle's front)" : "off (as before 5 Nov)");
        persist = true;
        return true;
    }
    if (cmd == "vehaimfrontmin") {                                                          // vehaimfrontmin N (test, deg)
        try {
            m_veh_aim_front_min = std::clamp(std::stof(arg), 1.0f, 45.0f);
        } catch (...) {
        }
        spdlog::info("[Vehicle] vehaimfrontmin {:.1f} deg", m_veh_aim_front_min.load());
        return true;
    }
    if (cmd == "vehaimfrontnow") { veh_aim_front_arm(std::chrono::steady_clock::now(), "vehaimfrontnow"); return true; }   // test
    if (cmd == "exityaw") {                                                                 // exityaw on|off (saved)
        m_exit_yaw_ev = on;
        spdlog::info("[Vehicle] exityaw {}", on ? "on (getting out, the culled camera follows the head's yaw through Extended View)" : "off (as before 7 Nov)");
        persist = true;
        return true;
    }
    if (cmd == "vehseathold") { m_veh_seat_hold = on; persist = true; return true; }       // vehseathold on|off (saved)
    if (cmd == "vehheadingkeep") {                                                          // vehheadingkeep on|off (saved)
        m_veh_heading_keep = on;
        spdlog::info("[Vehicle] vehheadingkeep {}", on ? "on (the view keeps the car's front in slides and jumps)" : "off (as before 6 Nov)");
        persist = true;
        return true;
    }
    if (cmd == "vehdrivegun") { m_veh_drive_gun = on; persist = true; return true; }       // vehdrivegun on|off (saved)
    if (cmd == "vehtriggers") {                                                            // vehtriggers left|right (saved)
        if (arg != "left" && arg != "right") return false;
        m_veh_triggers = arg == "left";
        spdlog::info("[Vehicle] vehtriggers {}: driving, {}", arg, arg == "left" ? "the right trigger shoots, the left trigger accelerates, the left grip brakes" : "the right trigger accelerates, the right grip brakes, the left trigger shoots");
        persist = true;
        return true;
    }
    if (cmd == "vehseatholdm") {                                                           // vehseatholdm N (metres, saved)
        try {
            m_veh_seat_hold_m = std::clamp(std::stof(arg), 0.2f, 2.0f);
        } catch (...) {
            return false;
        }
        spdlog::info("[Vehicle] vehseatholdm {:.2f} m", m_veh_seat_hold_m.load());
        persist = true;
        return true;
    }
    return false;
}

// vehtriggers: from the pad hook (VR::on_xinput_get_state) when the driving layout goes on or off.
void FarCry5Stereo::note_veh_triggers(bool swapped, bool right_held, bool left_held)
{
    const int shots = m_vt_shots.exchange(0);
    if (swapped)
        spdlog::info("[Vehicle] vehtriggers: driving, the right trigger shoots (LB), the left trigger accelerates (RT), the left grip brakes (LT){} ({})",
                     right_held || left_held ? "; a trigger held now waits until let go" : "", game_seat_text());
    else
        spdlog::info("[Vehicle] vehtriggers: the triggers back to on foot / passenger (right fires, left LB); {} shots from the right trigger while driving{} ({})", shots,
                     right_held || left_held ? "; a trigger held now waits until let go" : "", game_seat_text());
}

// vehdrivegun (log only): the game's own seat values beside the mod's driving / riding (GetPawnVehicleSeat and the driver's seat lookup
// are under question: on the quad, 30 Oct 09:11, the seat read 1 from 40.56 to the exit while he drove).
std::string FarCry5Stereo::game_seat_text() const
{
    int      seat = -1, dseat = -1;
    uint64_t id   = 0;
    fc5::lua::vehicle_seat(seat, dseat, id);
    if (id == 0) return "the game: no vehicle answer";
    return fmt::format("the game: seat {}, driver's seat {}, entity {:x}", seat, dseat, id);
}

// A settle starts (getting in, a seat change, getting out): the follow takes its start at its first step.
void FarCry5Stereo::veh_follow_start()
{
    m_vf_init      = false;
    m_vf_turn      = 0.0f;
    m_vf_snaps     = 0;
    m_vf_seat_held = 0;
}

// vehseathold: from vehicle_probe, each Vehicle_ draw of the world pass within 8 m of the camera while in a vehicle. Kept per frame:
// the draw that puts the camera nearest the learnt seat, or before it is learnt the largest (the body, not a door or a wheel).
void FarCry5Stereo::veh_seat_probe(int frame, const float* f, UINT count, float dist)
{
    const glm::vec3 x  = glm::normalize(glm::vec3{ f[0], f[4], f[8] });
    const glm::vec3 y0 = glm::vec3{ f[1], f[5], f[9] };
    const glm::vec3 y  = glm::normalize(y0 - glm::dot(y0, x) * x);
    glm::vec3       z  = glm::cross(x, y);
    if (glm::dot(z, glm::vec3{ f[2], f[6], f[10] }) < 0.0f) z = -z;
    const glm::mat3 rot = glm::transpose(glm::mat3{ x, y, z });   // world -> the model's axes
    const glm::vec3 cam = rot * (m_world_cam_pos - glm::vec3{ f[3], f[7], f[11] });
    if (!std::isfinite(cam.x + cam.y + cam.z)) return;
    auto& r = m_vsh_rec[frame & 3];
    if (r.frame == frame) {
        if (m_vsh_valid ? glm::length(cam - m_vsh_seat) >= glm::length(r.cam - m_vsh_seat) : count <= r.count) return;
    }
    r.frame = frame;
    r.count = count;
    r.cam   = cam;
    r.dist  = dist;
}

namespace
{
    // The latest frame's record (the model of a frame is known once its world pass is drawn).
    template <class R> const R* latest_rec(const R (&recs)[4], int frame)
    {
        for (int k = 0; k <= 3; ++k)
            if (recs[(frame - k) & 3].frame == frame - k) return &recs[(frame - k) & 3];
        return nullptr;
    }
}

// vehseathold: a settle ended (getting in, a seat change): the camera's place on the vehicle's model is the seat.
void FarCry5Stereo::veh_seat_learn(int frame)
{
    if (!m_veh_seat_hold.load()) return;
    const VehSeatRec* r = latest_rec(m_vsh_rec, frame);
    if (!r) {
        m_vsh_valid = false;
        m_vsh_side  = 99.0f;
        m_vsh_count = 0;
        spdlog::info("[Vehicle] vehseathold: no vehicle model within 8 m of the camera: no seat learnt (the hands' pass alone tells in from out)");
        return;
    }
    m_vsh_seat      = r->cam;
    m_vsh_count       = r->count;   // vehwheels: the vehicle model, with the seat, for its learnt wheel
    m_vsh_learn_frame = frame;
    m_vsh_side      = r->cam.x;   // drivequadseat: a quad's driver on the centre line, a car's to the side
    m_vsh_valid     = true;
    m_vsh_near      = true;
    m_vsh_driving   = m_veh_driving.load();
    m_vsh_kept_said = false;
    spdlog::info("[Vehicle] vehseathold: the seat learnt on the vehicle's model ({} indices, {:.1f} m from the camera; the camera at ({:.2f}, {:.2f}, {:.2f}) in its axes), {}",
                 r->count, r->dist, r->cam.x, r->cam.y, r->cam.z, m_vsh_driving ? "driving" : "riding");
}

// vehseathold: true while the camera is within vehseatholdm of the learnt seat (update_vehicle, once per frame).
bool FarCry5Stereo::veh_seat_update(int frame)
{
    if (!m_veh_seat_hold.load() || !m_in_vehicle.load() || !m_vsh_valid) {
        m_vsh_near = false;
        return false;
    }
    const VehSeatRec* r    = latest_rec(m_vsh_rec, frame);
    const float       d    = r ? glm::length(r->cam - m_vsh_seat) : -1.0f;
    const bool        in_seat = r && d < m_veh_seat_hold_m.load();
    if (in_seat) m_vsh_off_max = std::max(m_vsh_off_max, d);
    if (in_seat != m_vsh_near) {
        if (r)
            spdlog::info("[Vehicle] vehseathold: the camera {} the seat ({:.2f} m from it)", in_seat ? "back in" : "off", d);
        else
            spdlog::info("[Vehicle] vehseathold: no vehicle model drawn near the camera");
    }
    m_vsh_near = in_seat;
    return in_seat;
}

// One step of the view following the enter / exit animation (vehentryfollow), applied between pairs as m_body_yaw_pending.
float FarCry5Stereo::veh_anim_cam_yaw() const
{
    // exityaw: the camera carries the head's yaw handed through Extended View; the animation's own camera is the camera less it.
    // On foot with the switch on, every frame (the head look's offset at the hand-over, then exityaw's), so the follow sees no turn there.
    return (m_exit_yaw_ev.load() && !m_in_vehicle.load()) ? std::remainder(m_cine_game_yaw - m_exy_oy.load(), glm::two_pi<float>()) : m_cine_game_yaw;
}

void FarCry5Stereo::veh_follow_step(std::chrono::steady_clock::time_point now)
{
    const float cam = veh_anim_cam_yaw();
    if (!m_veh_follow_head.load()) {
        // As before 23 Oct: the view's heading held on the game camera's.
        m_body_yaw_pending = std::remainder(cam - (m_body_yaw + m_head_rel_yaw), glm::two_pi<float>());
        return;
    }
    if (!m_vf_init) {
        m_vf_init     = true;
        m_vf_body0    = m_body_yaw + m_body_yaw_pending;
        m_vf_cam_last = cam;
        m_vf_turn     = 0.0f;
        m_vf_snap_t   = now - std::chrono::seconds(1);
    }
    m_vf_turn += std::remainder(cam - m_vf_cam_last, glm::two_pi<float>());   // unwrapped: a turn past 180 deg is kept
    m_vf_cam_last = cam;
    const float d = std::remainder(m_vf_body0 + m_vf_turn - m_body_yaw, glm::two_pi<float>());
    if (!m_veh_follow_snap.load()) {
        m_body_yaw_pending = d;
        return;
    }
    const float step = glm::radians(std::clamp(m_snap_deg.load(), 15.0f, 45.0f));
    if (std::abs(d) >= step && now - m_vf_snap_t >= std::chrono::milliseconds(250)) {
        m_body_yaw_pending = d;   // the whole way to the animation's heading, in one cut
        m_vf_snap_t        = now;
        m_vf_snaps++;
        request_jump_reset();     // the view jumped: each eye's DLSS history starts over
    }
}

void FarCry5Stereo::draw_vehtrans_menu(const std::function<void(const char*, std::string)>& keep, const std::function<void(const char*)>& hint)
{
    bool v = m_veh_follow_head.load();
    if (ImGui::Checkbox("Vehicles: getting in or out, the head still turns the view", &v)) {
        m_veh_follow_head = v;
        keep("vehfollowhead", v ? "vehfollowhead on" : "vehfollowhead off");
    }
    hint("On (default): during the enter / exit animation the view turns with the animation's camera and your head still turns it on top; getting out ends without a "
         "turn. Off: as before 23 Oct (the view held on the game's camera: turning the head did not turn the view during the animation, then a recentre).");
    v = m_veh_follow_snap.load();
    if (ImGui::Checkbox("Vehicles: the enter / exit animation's turn in snaps", &v)) {
        m_veh_follow_snap = v;
        keep("vehfollowsnap", v ? "vehfollowsnap on" : "vehfollowsnap off");
    }
    hint("On (default): the animation's turn comes in steps of your snap-turn angle, like a snap turn, instead of the world turning smoothly around you. Off: a smooth turn.");
    v = m_veh_seat_guard.load();
    if (ImGui::Checkbox("Vehicles: getting out, no extra recentre", &v)) {
        m_veh_seat_guard = v;
        keep("vehseatguard", v ? "vehseatguard on" : "vehseatguard off");
    }
    hint("Getting out first looks like a change of seat to the mod. On (default): that is not settled while no vehicle is under you, so getting out has one settle. "
         "Off: as before 23 Oct (a cut and a recentre 0.6 s before the exit's own).");
    v = m_exit_yaw_ev.load();
    if (ImGui::Checkbox("Vehicles: getting out, the game draws where you look left or right", &v)) {
        m_exit_yaw_ev = v;
        keep("exityaw", v ? "exityaw on" : "exityaw off");
    }
    hint("On (default; author 7 Nov): during the exit animation the game's head look also turns the camera the game draws "
         "with to where your head looks, so the edges of the view are not cut while the animation swings round (the view still follows the animation in snaps). "
         "Off: as before 7 Nov.");
    v = m_veh_aim_front.load();
    if (ImGui::Checkbox("Vehicles: getting in, the game's camera turned to the vehicle's front", &v)) {
        m_veh_aim_front = v;
        keep("vehaimfront", v ? "vehaimfront on" : "vehaimfront off");
    }
    hint("On (default; 5 Nov: getting into a vehicle while the game's camera looked far to the side, the quad's bars and the car's wheel were drawn about "
         "20 deg off): when the get-in ends, the game's own camera is turned to the vehicle's front, as after a normal get-in, so the wheel or bars sit "
         "straight ahead. Off: as before 5 Nov (the game's camera left where the get-in left it).");
    v = m_veh_heading_keep.load();
    if (ImGui::Checkbox("Vehicles: the view keeps the car's front in slides and jumps", &v)) {
        m_veh_heading_keep = v;
        keep("vehheadingkeep", v ? "vehheadingkeep on" : "vehheadingkeep off");
    }
    hint("On (default; 6 Nov: after jumps and steep turns the view stopped following the car's front, turned to the right window and "
         "readjusted itself): the mod keeps reading the car's own model for its front when your seat moves, comes back onto it after "
         "losing it, does not take a slide for reversing or for a wrong front, and turns the game's own camera back to the front when a slide "
         "leaves it behind. Off: as before 6 Nov.");
    v = m_veh_seat_hold.load();
    if (ImGui::Checkbox("Vehicles: stay in the vehicle while in its seat (boats)", &v)) {
        m_veh_seat_hold = v;
        keep("vehseathold", v ? "vehseathold on" : "vehseathold off");
    }
    hint("On (default): the seat is learnt on the vehicle's model once you are in; while you sit in it you stay in the vehicle, and the driver stays the driver when "
         "the game shows a weapon (in a boat the game draws the sidearm every few seconds, and the mod took you for on foot: no throttle). "
         "Off: as before 28 Oct.");
}
