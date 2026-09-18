#pragma once

// Synthetic ACE and iRacing adapter fixtures.
//
// This file is included once, mid-file, by rapid-telemetry-daemon.cpp at the
// point where the previous additional_adapter_self_test namespace began, so it
// can use the Frame/Metadata definitions and the adapter classes above it.
//
// The fake shared memory is built from each simulator's *published* layout, not
// from the adapter under test: a value written through the documented struct
// must land where the adapter reads it, so a wrong adapter offset fails the
// test instead of agreeing with it. These run only under --self-test; a running
// simulator is never opened or modified.
//
// Layout sources (retrieved 2026-09-18):
//  * ACE -- "ACE SharedFileOut Documentation v1", the official shared-memory
//    API document (Kunos Simulazioni; assettocorsa.net shared-memory API
//    thread / hosted Google Doc). Changelog 2026-04-28 added car_ids.
//    Structures are naturally aligned (no #pragma pack) and the SMEvo*
//    sub-structures have the fixed sizes stated by the document.
//  * iRacing -- irsdk_defines.h from the official iRacing SDK (IRSDK_VER 2):
//    irsdk_header (112 bytes), irsdk_varBuf (16 bytes), irsdk_varHeader
//    (144 bytes), and the session-info YAML document.
namespace additional_adapter_self_test {
struct Bytes { alignas(16) std::array<std::byte, 16384> bytes{}; };

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(std::string("Adapter self-test: ") + message);
}

template <typename T>
void put(Bytes& mapping, std::size_t offset, T value) {
    require(offset + sizeof(T) <= mapping.bytes.size(), "fixture write out of bounds");
    std::memcpy(mapping.bytes.data() + offset, &value, sizeof(T));
}

void text(Bytes& mapping, std::size_t offset, std::string_view value, std::size_t capacity) {
    require(value.size() < capacity && offset + capacity <= mapping.bytes.size(), "fixture text out of bounds");
    std::memcpy(mapping.bytes.data() + offset, value.data(), value.size());
}

template <typename T> T& view(Bytes& mapping, std::size_t offset) {
    require(offset + sizeof(T) <= mapping.bytes.size(), "fixture view out of bounds");
    return *reinterpret_cast<T*>(mapping.bytes.data() + offset);
}

template <std::size_t N> void set_text(char (&destination)[N], std::string_view value) {
    require(value.size() < N, "fixture struct text out of bounds");
    std::memset(destination, 0, N);
    std::memcpy(destination, value.data(), value.size());
}

// ---- Assetto Corsa EVO: official SPageFile structures -----------------------
// The physics page is all four-byte fields, so its layout is the same packed or
// aligned; the graphics/static pages interleave bool/i8/i16 and must be
// naturally aligned, which the static_asserts below pin.
struct SPageFilePhysicsEvo {
    std::int32_t packet_id;
    float gas, brake, fuel;
    std::int32_t gear, rpms;
    float steer_angle, speed_kmh;
    float velocity[3];
    float acc_g[3];
    float wheel_slip[4], wheel_load[4], wheels_pressure[4], wheel_angular_speed[4];
    float tyre_wear[4], tyre_dirty_level[4], tyre_core_temperature[4];
    float camber_rad[4], suspension_travel[4];
    float drs, tc, heading, pitch, roll, cg_height;
    float car_damage[5];
    std::int32_t number_of_tyres_out, pit_limiter_on;
    float abs_intensity;
    float kers_charge, kers_input;
    std::int32_t auto_shifter_on;
    float ride_height[2];
    float turbo_boost, ballast, air_density, air_temp, road_temp;
    float local_angular_vel[3];
    float final_ff, performance_meter;
    std::int32_t engine_brake, ers_recovery_level, ers_power_level, ers_heat_charging, ers_is_charging;
    float kers_current_kj;
    std::int32_t drs_available, drs_enabled;
    float brake_temp[4], clutch;
    float tyre_temp_i[4], tyre_temp_m[4], tyre_temp_o[4];
    std::int32_t is_ai_controlled;
    float tyre_contact_point[4][3], tyre_contact_normal[4][3], tyre_contact_heading[4][3];
    float brake_bias, local_velocity[3];
    std::int32_t p2p_activations, p2p_status, current_max_rpm;
    float mz[4], fx[4], fy[4], slip_ratio[4], slip_angle[4];
    std::int32_t tcin_action, abs_in_action;
    float suspension_damage[4], tyre_temp[4], water_temp;
    float brake_torque[4];
    std::int32_t front_brake_compound, rear_brake_compound;
    float pad_life[4], disc_life[4];
    std::int32_t ignition_on, starter_engine_on, is_engine_running;
    float kerb_vibration, slip_vibrations, road_vibrations, abs_vibrations;
};
static_assert(sizeof(SPageFilePhysicsEvo) == 800, "SPageFilePhysics is 800 bytes in the ACE doc");
static_assert(offsetof(SPageFilePhysicsEvo, gas) == 4, "ACE physics gas offset");
static_assert(offsetof(SPageFilePhysicsEvo, gear) == 16, "ACE physics gear offset");
static_assert(offsetof(SPageFilePhysicsEvo, steer_angle) == 24, "ACE physics steerAngle offset");
static_assert(offsetof(SPageFilePhysicsEvo, speed_kmh) == 28, "ACE physics speedKmh offset");
static_assert(offsetof(SPageFilePhysicsEvo, acc_g) == 44, "ACE physics accG offset");
static_assert(offsetof(SPageFilePhysicsEvo, wheel_angular_speed) == 104, "ACE physics wheelAngularSpeed offset");
static_assert(offsetof(SPageFilePhysicsEvo, tyre_core_temperature) == 152, "ACE physics tyreCoreTemperature offset");
static_assert(offsetof(SPageFilePhysicsEvo, suspension_travel) == 184, "ACE physics suspensionTravel offset");
static_assert(offsetof(SPageFilePhysicsEvo, tc) == 204, "ACE physics tc offset");
static_assert(offsetof(SPageFilePhysicsEvo, abs_intensity) == 252, "ACE physics abs offset");
static_assert(offsetof(SPageFilePhysicsEvo, tcin_action) == 672, "ACE physics tcinAction offset");
static_assert(offsetof(SPageFilePhysicsEvo, abs_in_action) == 676, "ACE physics absInAction offset");

struct SMEvoTyreState {
    float slip;
    bool lock;
    float tyre_pressure, tyre_temperature_c, brake_temperature_c, brake_pressure;
    float tyre_temperature_left, tyre_temperature_center, tyre_temperature_right;
    char tyre_compound_front[33];
    char tyre_compound_rear[33];
    float tyre_normalized_pressure, tyre_normalized_temperature_left,
          tyre_normalized_temperature_center, tyre_normalized_temperature_right,
          brake_normalized_temperature, tyre_normalized_temperature_core;
    std::uint8_t pad[128];
};
static_assert(sizeof(SMEvoTyreState) == 256, "SMEvoTyreState is 256 bytes in the ACE doc");

struct SMEvoDamageState {
    float damage_front, damage_rear, damage_left, damage_right, damage_center;
    float damage_suspension_lf, damage_suspension_rf, damage_suspension_lr, damage_suspension_rr;
    std::uint8_t pad[92];
};
static_assert(sizeof(SMEvoDamageState) == 128, "SMEvoDamageState is 128 bytes in the ACE doc");

struct SMEvoPitInfo {
    std::int8_t damage, fuel, tyres_lf, tyres_rf, tyres_lr, tyres_rr;
    std::uint8_t pad[58];
};
static_assert(sizeof(SMEvoPitInfo) == 64, "SMEvoPitInfo is 64 bytes in the ACE doc");

struct SMEvoElectronics {
    std::int8_t tc_level, tc_cut_level, abs_level, esc_level, ebb_level;
    float brake_bias;
    std::int8_t engine_map_level;
    float turbo_level;
    std::int8_t ers_deployment_map;
    float ers_recharge_map;
    bool is_ers_heat_charging_on, is_ers_overtake_mode_on, is_drs_open;
    std::int8_t diff_power_level, diff_coast_level, front_bump_damper_level,
                front_rebound_damper_level, rear_bump_damper_level, rear_rebound_damper_level;
    bool is_ignition_on, is_pitlimiter_on;
    std::int8_t active_performance_mode;
    std::uint8_t pad[88];
};
static_assert(sizeof(SMEvoElectronics) == 128, "SMEvoElectronics is 128 bytes in the ACE doc");

struct SMEvoInstrumentation {
    std::int8_t main_light_stage, special_light_stage, cockpit_light_stage, wiper_level;
    bool rain_lights, direction_light_left, direction_light_right, flashing_lights, warning_lights;
    std::int8_t selected_display_index;
    std::int8_t display_current_page_index[16];
    bool are_headlights_visible;
    std::uint8_t pad[101];
};
static_assert(sizeof(SMEvoInstrumentation) == 128, "SMEvoInstrumentation is 128 bytes in the ACE doc");

struct SMEvoSessionState {
    char phase_name[33];
    char time_left[15];
    std::int32_t time_left_ms;
    char wait_time[15];
    std::int32_t total_lap, current_lap, lights_on, lights_mode;
    float lap_length_km;
    std::int32_t end_session_flag;
    char time_to_next_session[15];
    bool disconnected_from_server, restart_season_enabled, ui_enable_drive,
         ui_enable_setup, is_ready_to_next_blinking, show_waiting_for_players;
    std::uint8_t pad[143];
};
static_assert(sizeof(SMEvoSessionState) == 256, "SMEvoSessionState is 256 bytes in the ACE doc");

struct SMEvoTimingState {
    char current_laptime[15], delta_current[15];
    std::int32_t delta_current_p;
    char last_laptime[15], delta_last[15];
    std::int32_t delta_last_p;
    char best_laptime[15], ideal_laptime[15], total_time[15];
    bool is_invalid;
    std::uint8_t pad[138];
};
static_assert(sizeof(SMEvoTimingState) == 256, "SMEvoTimingState is 256 bytes in the ACE doc");

struct SMEvoAssistsState {
    std::uint8_t auto_gear, auto_blip, auto_clutch, auto_clutch_on_start,
                 manual_ignition_e_start, auto_pit_limiter, standing_start_assist;
    float auto_steer, arcade_stability_control;
    std::uint8_t pad[48];
};
static_assert(sizeof(SMEvoAssistsState) == 64, "SMEvoAssistsState is 64 bytes in the ACE doc");

struct SPageFileGraphicEvo {
    std::int32_t packet_id, status;
    std::uint64_t focused_car_id_a, focused_car_id_b, player_car_id_a, player_car_id_b;
    std::uint16_t rpm;
    bool is_rpm_limiter_on, is_change_up_rpm, is_change_down_rpm, tc_active, abs_active,
         esc_active, launch_active, is_ignition_on, is_engine_running, kers_is_charging,
         is_wrong_way, is_drs_available, battery_is_charging, is_max_kj_per_lap_reached,
         is_max_charge_kj_per_lap_reached;
    std::int16_t display_speed_kmh, display_speed_mph, display_speed_ms;
    float pitspeeding_delta;
    std::int16_t gear_int;
    float rpm_percent, gas_percent, brake_percent, handbrake_percent, clutch_percent,
          steering_percent, ffb_strength, car_ffb_multiplier, water_temperature_percent,
          water_pressure_bar, fuel_pressure_bar;
    std::int8_t water_temperature_c, air_temperature_c;
    float oil_temperature_c, oil_pressure_bar, exhaust_temperature_c;
    float g_forces_x, g_forces_y, g_forces_z;
    float turbo_boost, turbo_boost_level, turbo_boost_perc;
    std::int32_t steer_degrees;
    float current_km;
    std::uint32_t total_km, total_driving_time_s;
    std::int32_t time_of_day_hours, time_of_day_minutes, time_of_day_seconds;
    std::int32_t delta_time_ms, current_lap_time_ms, predicted_lap_time_ms;
    float fuel_liter_current_quantity, fuel_liter_current_quantity_percent,
          fuel_liter_per_km, km_per_fuel_liter, current_torque;
    std::int32_t current_bhp;
    SMEvoTyreState tyre_lf, tyre_rf, tyre_lr, tyre_rr;
    float npos, kers_charge_perc, kers_current_perc, control_lock_time;
    SMEvoDamageState car_damage;
    std::int32_t car_location;
    SMEvoPitInfo pit_info;
    float fuel_liter_used, fuel_liter_per_lap, laps_possible_with_fuel,
          battery_temperature, battery_voltage, instantaneous_fuel_liter_per_km,
          instantaneous_km_per_fuel_liter, gear_rpm_window;
    SMEvoInstrumentation instrumentation, instrumentation_min_limit, instrumentation_max_limit;
    SMEvoElectronics electronics, electronics_min_limit, electronics_max_limit, electronics_is_modifiable;
    std::int32_t total_lap_count;
    std::uint32_t current_pos, total_drivers;
    std::int32_t last_laptime_ms, best_laptime_ms, flag, global_flag;
    std::uint32_t max_gears;
    std::int32_t engine_type;
    bool has_kers, is_last_lap;
    char performance_mode_name[33];
    float diff_coast_raw_value, diff_power_raw_value;
    std::int32_t race_cut_gained_time_ms, distance_to_deadline;
    float race_cut_current_delta;
    SMEvoSessionState session_state;
    SMEvoTimingState timing_state;
    std::int32_t player_ping, player_latency, player_cpu_usage, player_cpu_usage_avg,
          player_qos, player_qos_avg, player_fps, player_fps_avg;
    char driver_name[33], driver_surname[33], car_model[33];
    bool is_in_pit_box, is_in_pit_lane, is_valid_lap;
    float car_coordinates[60][3];
    float gap_ahead, gap_behind;
    std::uint8_t active_cars;
    float fuel_per_lap, fuel_estimated_laps;
    SMEvoAssistsState assists_state;
    float max_fuel, max_turbo_boost;
    bool use_single_compound;
    std::uint64_t car_ids[60][2];
};
static_assert(offsetof(SPageFileGraphicEvo, status) == 4, "ACE graphics status offset");
static_assert(offsetof(SPageFileGraphicEvo, delta_time_ms) == 184, "ACE graphics delta offset");
static_assert(offsetof(SPageFileGraphicEvo, current_lap_time_ms) == 188, "ACE graphics lap-time offset");
static_assert(offsetof(SPageFileGraphicEvo, npos) == 1244, "ACE graphics npos offset");
static_assert(offsetof(SPageFileGraphicEvo, car_model) == 3086, "ACE graphics car_model offset");

struct SPageFileStaticEvo {
    char sm_version[15];
    char ac_evo_version[15];
    std::int32_t session;
    char session_name[33];
    std::uint8_t event_id, session_id;
    std::int32_t starting_grip;
    float starting_ambient_temperature_c, starting_ground_temperature_c;
    bool is_static_weather, is_timed_race, is_online;
    std::int32_t number_of_sessions;
    char nation[33];
    float longitude, latitude;
    char track[33];
    char track_configuration[33];
    float track_length_m;
};
static_assert(offsetof(SPageFileStaticEvo, session) == 32, "ACE static session offset");
static_assert(offsetof(SPageFileStaticEvo, session_name) == 36, "ACE static session_name offset");
static_assert(offsetof(SPageFileStaticEvo, track) == 136, "ACE static track offset");
static_assert(offsetof(SPageFileStaticEvo, track_configuration) == 169, "ACE static layout offset");
static_assert(offsetof(SPageFileStaticEvo, track_length_m) == 204, "ACE static track_length offset");
static_assert(sizeof(SPageFileStaticEvo) == 208, "SPageFileStatic is 208 bytes in the ACE doc");

// ACE session enum (ACEVO_SESSION_TYPE): -1 unknown, 0 time attack, 1 race,
// 2 hot stint, 3 cruise. The adapter previously used AC1's enum.
void ace() {
    const auto prefix = L"Local\\raPIdAceSelfTest_" + std::to_wstring(GetCurrentProcessId()) +
                        L"_" + std::to_wstring(GetTickCount64());
    assetto_self_test::TestMapping<SPageFilePhysicsEvo> physics(prefix + L"_physics");
    assetto_self_test::TestMapping<SPageFileGraphicEvo> graphics(prefix + L"_graphics");
    assetto_self_test::TestMapping<SPageFileStaticEvo> info(prefix + L"_static");
    auto& p = physics.value();
    auto& g = graphics.value();
    auto& s = info.value();

    set_text(s.track, "monza");
    set_text(s.track_configuration, "gp");
    s.session = 1;                       // AC_RACE
    set_text(s.session_name, "Race 1");
    set_text(g.car_model, "bmw_m4_evo");
    p.packet_id = 41;
    p.gas = .8f; p.brake = .3f; p.fuel = 20.f;
    p.gear = 4; p.rpms = 7000;
    p.steer_angle = .25f; p.speed_kmh = 201.5f;
    p.velocity[0] = 1.f; p.velocity[1] = 2.f; p.velocity[2] = 3.f;
    p.acc_g[0] = .4f; p.acc_g[1] = 1.1f; p.acc_g[2] = -.2f;
    p.wheel_slip[0] = .1f; p.wheels_pressure[0] = 27.5f; p.wheel_angular_speed[0] = 71.f;
    p.tyre_core_temperature[0] = 85.f; p.suspension_travel[0] = .03f;
    // The intensity fields (float, 204/252) and the boolean in-action fields
    // (int, 672/676) are deliberately given different values here so the test
    // tells them apart.
    p.tc = .42f; p.abs_intensity = .66f;
    p.tcin_action = 1; p.abs_in_action = 0;
    p.car_damage[0] = .5f;
    g.packet_id = 20;
    g.status = 2;                        // AC_LIVE
    g.delta_time_ms = -123;
    g.current_lap_time_ms = 18000;
    g.npos = .25f;

    auto adapter = AceAdapter::open((prefix + L"_physics").c_str(), (prefix + L"_graphics").c_str(),
                                    (prefix + L"_static").c_str());
    require(bool(adapter) && adapter->live() && !adapter->ended(),
            "ACE opens and enters the driving state");
    require(adapter->metadata.session == "Race" && adapter->metadata.venue == "monza / gp" &&
                adapter->metadata.vehicle == "bmw_m4_evo",
            "ACE metadata from the official fields: session enum, track/layout, car model");
    require(adapter->metadata.steering_lock_deg == 0.0,
            "ACE exposes no steering lock: reported unknown, not guessed");

    Frame frame;
    require(adapter->read(frame), "ACE accepts a coherent sample");
    const auto close_enough = [](double actual, double expected) {
        return std::abs(actual - expected) < .0001;
    };
    require(close_enough(frame.value[throttle], .8) && close_enough(frame.value[brake], .3) &&
                close_enough(frame.value[fuel], 20) && frame.value[gear] == 3 && frame.value[rpm] == 7000 &&
                close_enough(frame.value[steering_angle], .25) && close_enough(frame.value[speed_kmh], 201.5) &&
                close_enough(frame.value[g_x], .4) && close_enough(frame.value[g_y], 1.1) &&
                close_enough(frame.value[g_z], -.2),
            "ACE controls, steering, speed and acceleration");
    require(close_enough(frame.value[wheel_slip_fl], .1) && close_enough(frame.value[pressure_fl], 27.5) &&
                close_enough(frame.value[wheel_speed_fl], 71) && close_enough(frame.value[core_temp_fl], 85) &&
                close_enough(frame.value[suspension_fl], .03),
            "ACE per-corner channels");
    // The adapter must read the documented float intensity fields, not the
    // boolean tcinAction/absInAction flags at 672/676 (the fixture sets those
    // to 1 and 0): reading the wrong field yields 1.0/0.0, not 0.42/0.66.
    require(close_enough(frame.value[tc], .42) && close_enough(frame.value[abs_activity], .66),
            "ACE tc/abs use the documented intensity fields");
    require(frame.value[lap_number] == 1 && frame.value[current_lap_ms] == 18000 &&
                frame.completed_lap_ms == 0 && frame.delta_ms == -123 &&
                close_enough(frame.value[lap_position], .25),
            "ACE lap number, current lap time, delta and position");
    require(frame.valid_mask ==
                (all_field_bits() & ~field_bit(pit_limiter) & ~field_bit(damage_front) &
                 ~field_bit(damage_rear) & ~field_bit(damage_left) & ~field_bit(damage_right) &
                 ~field_bit(damage_center)),
            "ACE unavailable-channel mask");

    // A lap reset from ~18 s to ~1 s derives the completed lap and increments
    // the synthetic lap number even though ACE never ticks it itself (#16).
    p.packet_id = 42; g.current_lap_time_ms = 1000;
    require(adapter->read(frame) && frame.value[lap_number] == 2 && frame.completed_lap_ms == 18000,
            "ACE lap rollover derives the completed lap time");

    // ACEVO_STATUS: 0 off, 1 replay, 2 live, 3 pause. Only 0 is the end of the
    // session; replay and pause are gaps inside it (#15).
    g.status = 3;
    require(!adapter->live() && !adapter->ended(), "ACE pause is a gap, not the end (#15)");
    g.status = 1;
    require(!adapter->live() && !adapter->ended(), "ACE replay is a gap, not the end (#15)");
    g.status = 0;
    require(!adapter->live() && adapter->ended(), "ACE status 0 (off/menu) reports ended (#15)");
    g.status = 2; p.packet_id = 43;
    require(adapter->live() && adapter->read(frame) && !adapter->ended(), "ACE resumes after the menu");
    p.packet_id = 0;
    require(adapter->read(frame), "ACE accepts a restarted packet counter");

    // Mid-session car/track/session change while not live: refresh_metadata()
    // must expose it so the run loop can end the recording (#15).
    g.status = 0;
    set_text(g.car_model, "ferrari_296_gtb");
    set_text(s.track, "spa");
    set_text(s.track_configuration, "endurance");
    s.session = 3;                       // AC_CRUISE
    adapter->refresh_metadata();
    require(adapter->metadata.vehicle == "ferrari_296_gtb" && adapter->metadata.venue == "spa / endurance" &&
                adapter->metadata.session == "Cruise",
            "ACE refresh_metadata reflects a mid-session car/track/session change (#15)");

    std::cout << "ACE (Assetto Corsa EVO) adapter self-test passed: controls, metadata, laps, pause, replay, "
                 "restart, ended/menu detection, mid-session car/track/session change, steering "
                 "normalisation (lock unknown)\n";
}

// ---- iRacing: official irsdk structures ------------------------------------
struct IrsdkVarBuf {
    std::int32_t tick_count;
    std::int32_t buf_offset;
    std::int32_t pad[2];
};
struct IrsdkVarHeader {
    std::int32_t type;
    std::int32_t offset;
    std::int32_t count;
    bool count_as_time;
    char pad[3];
    char name[32];
    char desc[64];
    char unit[32];
};
struct IrsdkHeader {
    std::int32_t ver, status, tick_rate;
    std::int32_t session_info_update, session_info_len, session_info_offset;
    std::int32_t num_vars, var_header_offset;
    std::int32_t num_buf, buf_len;
    std::int32_t pad1[2];
    IrsdkVarBuf var_buf[4];
};
static_assert(sizeof(IrsdkVarBuf) == 16, "irsdk_varBuf is 16 bytes");
static_assert(sizeof(IrsdkVarHeader) == 144, "irsdk_varHeader is 144 bytes");
static_assert(sizeof(IrsdkHeader) == 112, "irsdk_header is 112 bytes");
static_assert(offsetof(IrsdkHeader, status) == 4, "irsdk_header status");
static_assert(offsetof(IrsdkHeader, session_info_len) == 16, "irsdk_header sessionInfoLen");
static_assert(offsetof(IrsdkHeader, session_info_offset) == 20, "irsdk_header sessionInfoOffset");
static_assert(offsetof(IrsdkHeader, num_vars) == 24, "irsdk_header numVars");
static_assert(offsetof(IrsdkHeader, var_header_offset) == 28, "irsdk_header varHeaderOffset");
static_assert(offsetof(IrsdkHeader, num_buf) == 32, "irsdk_header numBuf");
static_assert(offsetof(IrsdkHeader, var_buf) == 48, "irsdk_header varBuf");
static_assert(offsetof(IrsdkVarHeader, offset) == 4, "irsdk_varHeader offset");
static_assert(offsetof(IrsdkVarHeader, name) == 16, "irsdk_varHeader name");

constexpr std::size_t kIracingVarTable = 256;
constexpr std::size_t kIracingBuffer = 4096;
constexpr std::size_t kIracingYaml = 6000;

void iracing_variable(Bytes& data, int index, int type, int offset, std::string_view name_text) {
    const std::size_t at = kIracingVarTable + static_cast<std::size_t>(index) * sizeof(IrsdkVarHeader);
    auto& variable = view<IrsdkVarHeader>(data, at);
    variable.type = type; variable.offset = offset; variable.count = 1; variable.count_as_time = false;
    require(name_text.size() < sizeof(variable.name), "iRacing variable name too long");
    std::memcpy(variable.name, name_text.data(), name_text.size());
}

void iracing_header(Bytes& data, int num_vars) {
    auto& header = view<IrsdkHeader>(data, 0);
    header.ver = 2; header.status = 1; header.tick_rate = 60;
    header.session_info_update = 1;
    header.num_vars = num_vars;
    header.var_header_offset = static_cast<std::int32_t>(kIracingVarTable);
    header.num_buf = 1;
    header.buf_len = 256;
    header.var_buf[0].tick_count = 7;
    header.var_buf[0].buf_offset = static_cast<std::int32_t>(kIracingBuffer);
}

void iracing_yaml(Bytes& data, std::string_view yaml) {
    require(kIracingYaml + yaml.size() + 1 <= data.bytes.size(), "iRacing YAML out of bounds");
    std::memcpy(data.bytes.data() + kIracingYaml, yaml.data(), yaml.size());
    data.bytes[kIracingYaml + yaml.size()] = std::byte{0};
    auto& header = view<IrsdkHeader>(data, 0);
    header.session_info_len = static_cast<std::int32_t>(yaml.size());
    header.session_info_offset = static_cast<std::int32_t>(kIracingYaml);
}

void iracing() {
    // Session YAML in iRacing's real nested form (WeekendInfo / SessionInfo /
    // DriverInfo), not a flat list of top-level keys.
    const std::string yaml =
        "WeekendInfo:\n"
        " TrackName: testtrack\n"
        " TrackDisplayName: Test Track 2\n"
        " TrackConfigName: Grand Prix\n"
        "SessionInfo:\n"
        " Sessions:\n"
        " - SessionNum: 0\n"
        "   SessionType: Practice\n"
        "   SessionName: Practice\n"
        " - SessionNum: 1\n"
        "   SessionType: Race\n"
        "DriverInfo:\n"
        " DriverCarIdx: 0\n"
        " DriverCarSteerWheelRange: 900.000\n"
        " Drivers:\n"
        " - CarIdx: 0\n"
        "   UserName: Test Driver 2\n"
        "   CarScreenName: Test Car 2\n";

    const auto name = L"Local\\raPIdIracingSelfTest_" + std::to_wstring(GetCurrentProcessId()) +
                      L"_" + std::to_wstring(GetTickCount64());
    assetto_self_test::TestMapping<Bytes> mapping(name);
    auto& data = mapping.value();
    iracing_header(data, 26);
    int index = 0;
    iracing_variable(data, index++, 1, 0, "IsOnTrack");
    iracing_variable(data, index++, 1, 100, "IsOnTrackCar");
    iracing_variable(data, index++, 4, 4, "Throttle");
    iracing_variable(data, index++, 4, 8, "Brake");
    iracing_variable(data, index++, 4, 12, "FuelLevel");
    iracing_variable(data, index++, 2, 16, "Gear");
    iracing_variable(data, index++, 4, 20, "RPM");
    iracing_variable(data, index++, 4, 24, "SteeringWheelAngle");
    iracing_variable(data, index++, 4, 28, "Speed");
    iracing_variable(data, index++, 4, 32, "VelocityX");
    iracing_variable(data, index++, 4, 36, "VelocityY");
    iracing_variable(data, index++, 4, 40, "VelocityZ");
    iracing_variable(data, index++, 4, 44, "LatAccel");
    iracing_variable(data, index++, 4, 48, "VertAccel");
    iracing_variable(data, index++, 4, 52, "LongAccel");
    iracing_variable(data, index++, 2, 56, "Lap");
    iracing_variable(data, index++, 4, 60, "LapCurrentLapTime");
    iracing_variable(data, index++, 4, 64, "LapLastLapTime");
    iracing_variable(data, index++, 4, 68, "LapDeltaToBestLap");
    iracing_variable(data, index++, 4, 72, "LapDistPct");
    iracing_variable(data, index++, 3, 76, "EngineWarnings");
    iracing_variable(data, index++, 4, 80, "LFshockDefl");
    iracing_variable(data, index++, 4, 84, "RFshockDefl");
    iracing_variable(data, index++, 4, 88, "LRshockDefl");
    iracing_variable(data, index++, 4, 92, "RRshockDefl");
    iracing_variable(data, index++, 4, 96, "SteeringWheelAngleMax");
    require(index == 26, "iRacing fixture variable count");
    iracing_yaml(data, yaml);
    put<unsigned char>(data, kIracingBuffer + 0, 1);
    put<unsigned char>(data, kIracingBuffer + 100, 0);
    put<float>(data, kIracingBuffer + 4, .6f);
    put<float>(data, kIracingBuffer + 8, .2f);
    put<float>(data, kIracingBuffer + 12, 42.f);
    put<std::int32_t>(data, kIracingBuffer + 16, 4);
    put<float>(data, kIracingBuffer + 20, 6500.f);
    put<float>(data, kIracingBuffer + 24, -1.3f);
    put<float>(data, kIracingBuffer + 28, 50.f);
    put<float>(data, kIracingBuffer + 32, 1.f);
    put<float>(data, kIracingBuffer + 36, 2.f);
    put<float>(data, kIracingBuffer + 40, 3.f);
    put<float>(data, kIracingBuffer + 44, 9.80665f);
    put<float>(data, kIracingBuffer + 48, 19.6133f);
    put<float>(data, kIracingBuffer + 52, -9.80665f);
    put<std::int32_t>(data, kIracingBuffer + 56, 7);
    put<float>(data, kIracingBuffer + 60, 12.5f);
    put<float>(data, kIracingBuffer + 64, 91.25f);
    put<float>(data, kIracingBuffer + 68, -.5f);
    put<float>(data, kIracingBuffer + 72, .375f);
    put<std::int32_t>(data, kIracingBuffer + 76, 0x18);
    put<float>(data, kIracingBuffer + 80, 1.f);
    put<float>(data, kIracingBuffer + 84, 2.f);
    put<float>(data, kIracingBuffer + 88, 3.f);
    put<float>(data, kIracingBuffer + 92, 4.f);
    // -1.3 rad / 6.5 rad live half-lock = -0.2 normalised: proves the live
    // SteeringWheelAngleMax, not the 900 deg YAML lock, drives normalisation.
    put<float>(data, kIracingBuffer + 96, 6.5f);

    auto adapter = IracingAdapter::open(name.c_str());
    require(bool(adapter) && adapter->connected() && adapter->live(), "iRacing opens and enters track state");
    require(adapter->metadata.session == "Practice" && adapter->metadata.vehicle == "Test Car 2" &&
                adapter->metadata.venue == "Test Track 2" && adapter->metadata.driver == "Test Driver 2" &&
                adapter->metadata.steering_lock_deg == 900.0,
            "iRacing metadata from the real nested session YAML");
    Frame frame;
    require(adapter->read(frame), "iRacing accepts a connected sample");
    const auto close_enough = [](double actual, double expected) {
        return std::abs(actual - expected) < .0001;
    };
    require(close_enough(frame.value[throttle], .6) && close_enough(frame.value[brake], .2) &&
                close_enough(frame.value[fuel], 42) && frame.value[gear] == 4 &&
                close_enough(frame.value[rpm], 6500) && close_enough(frame.value[steering_angle], -.2) &&
                close_enough(frame.value[speed_kmh], 180) && close_enough(frame.value[velocity_x], 1) &&
                close_enough(frame.value[velocity_y], 2) && close_enough(frame.value[velocity_z], 3) &&
                close_enough(frame.value[g_x], 1) && close_enough(frame.value[g_y], 2) &&
                close_enough(frame.value[g_z], -1),
            "iRacing controls, steering, speed, velocity and acceleration");
    require(frame.value[lap_number] == 7 && frame.value[current_lap_ms] == 12500 &&
                frame.completed_lap_ms == 91250 && frame.delta_ms == -500 &&
                close_enough(frame.value[lap_position], .375) && frame.value[pit_limiter] == 1.0,
            "iRacing lap timing, position and pit-limiter flag");
    require(close_enough(frame.value[suspension_fl], 1) && close_enough(frame.value[suspension_fr], 2) &&
                close_enough(frame.value[suspension_rl], 3) && close_enough(frame.value[suspension_rr], 4),
            "iRacing per-corner suspension");

    // Pit/menu/replay: iRacing has no ended() signal, so losing IsOnTrack is a
    // gap in the recording, and IsOnTrackCar keeps the session live when only
    // IsOnTrack drops (#15).
    put<unsigned char>(data, kIracingBuffer + 0, 0);
    require(!adapter->live() && !adapter->ended(), "iRacing off-track is a gap, not the end (#15)");
    put<unsigned char>(data, kIracingBuffer + 0, 1);
    require(adapter->live(), "iRacing returns to live when back on track");

    // In-place YAML update: session type, car, track and steering lock change
    // without reopening the adapter (#15, #18).
    const std::string refreshed_yaml =
        "WeekendInfo:\n"
        " TrackName: newtrack\n"
        " TrackDisplayName: Test Track 3\n"
        "SessionInfo:\n"
        " Sessions:\n"
        " - SessionNum: 2\n"
        "   SessionType: Race\n"
        "DriverInfo:\n"
        " DriverCarIdx: 0\n"
        " DriverCarSteerWheelRange: 540.000\n"
        " Drivers:\n"
        " - CarIdx: 0\n"
        "   UserName: Test Driver 3\n"
        "   CarScreenName: Test Car 3\n";
    iracing_yaml(data, refreshed_yaml);
    adapter->refresh_metadata();
    require(adapter->metadata.session == "Race" && adapter->metadata.vehicle == "Test Car 3" &&
                adapter->metadata.venue == "Test Track 3" && adapter->metadata.driver == "Test Driver 3" &&
                adapter->metadata.steering_lock_deg == 540.0,
            "iRacing refresh_metadata reflects a mid-session session/car/track change (#15)");

    put<std::int32_t>(data, 4, 0);
    require(!adapter->connected() && !adapter->read(frame), "iRacing disconnect rejects samples");

    // Steering lock step 2: no live SteeringWheelAngleMax variable, so the
    // session YAML DriverCarSteerWheelRange is the half-lock source.
    const auto name_b = name + L"_b";
    assetto_self_test::TestMapping<Bytes> mapping_b(name_b);
    auto& data_b = mapping_b.value();
    iracing_header(data_b, 2);
    iracing_variable(data_b, 0, 1, 0, "IsOnTrack");
    iracing_variable(data_b, 1, 4, 4, "SteeringWheelAngle");
    iracing_yaml(data_b, "DriverInfo:\n DriverCarSteerWheelRange: 700.000\n");
    put<unsigned char>(data_b, kIracingBuffer + 0, 1);
    // 2.1380283 rad / (350 deg half-lock) = 0.35 normalised.
    put<float>(data_b, kIracingBuffer + 4, 2.1380283f);
    auto adapter_b = IracingAdapter::open(name_b.c_str());
    require(bool(adapter_b) && adapter_b->connected() && adapter_b->live() &&
                adapter_b->metadata.steering_lock_deg == 700.0,
            "iRacing reads DriverCarSteerWheelRange when no live half-lock exists");
    Frame frame_b;
    require(adapter_b->read(frame_b) && close_enough(frame_b.value[steering_angle], .35),
            "iRacing steering falls back to the session YAML lock");

    // Steering lock step 3: neither a live variable nor a session lock, so the
    // 450 deg default half-lock (900 deg lock-to-lock) applies.
    const auto name_c = name + L"_c";
    assetto_self_test::TestMapping<Bytes> mapping_c(name_c);
    auto& data_c = mapping_c.value();
    iracing_header(data_c, 2);
    iracing_variable(data_c, 0, 1, 0, "IsOnTrack");
    iracing_variable(data_c, 1, 4, 4, "SteeringWheelAngle");
    iracing_yaml(data_c, "WeekendInfo:\n TrackName: testtrack\n");
    put<unsigned char>(data_c, kIracingBuffer + 0, 1);
    // pi/2 rad / (450 deg half-lock in rad) = 0.2 normalised.
    put<float>(data_c, kIracingBuffer + 4, 1.5707964f);
    auto adapter_c = IracingAdapter::open(name_c.c_str());
    require(bool(adapter_c) && adapter_c->connected() && adapter_c->metadata.steering_lock_deg == 0.0,
            "iRacing reports an unknown steering lock when the sim exposes none");
    Frame frame_c;
    require(adapter_c->read(frame_c) && close_enough(frame_c.value[steering_angle], .2),
            "iRacing steering falls back to the 450 deg default half-lock");

    std::cout << "iRacing adapter self-test passed: controls, metadata, laps, pause/replay (not-ended), "
                 "disconnect, mid-session session/car/track change, steering normalisation and lock "
                 "fallback (live SteeringWheelAngleMax, session DriverCarSteerWheelRange, 450 deg default)\n";
}
} // namespace additional_adapter_self_test




