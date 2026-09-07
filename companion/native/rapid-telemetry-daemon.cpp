#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#include <bcrypt.h>
#include <shellapi.h>
#include <shlobj.h>
#include <tlhelp32.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cctype>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace rapid {

constexpr wchar_t kMutexName[] = L"Local\\raPIdTelemetryDaemon";
constexpr wchar_t kWindowClass[] = L"raPIdTelemetryDaemonWindow";
constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kStatusMessage = WM_APP + 2;
constexpr UINT kMenuStatus = 1001;
constexpr UINT kMenuFolder = 1002;
constexpr UINT kMenuExit = 1003;
constexpr std::size_t kHeaderSize = 1762;
constexpr std::size_t kEventSize = 1154;
constexpr std::size_t kChannelHeaderSize = 124;
constexpr std::size_t kV4HeaderSize = 52;
constexpr std::size_t kV4HmacSize = 32;
constexpr std::uint16_t kV4SchemaVersion = 1;

enum Field : std::size_t {
    elapsed, throttle, brake, fuel, gear, rpm, steering_angle, speed_kmh,
    velocity_x, velocity_y, velocity_z, g_x, g_y, g_z,
    wheel_slip_fl, wheel_slip_fr, wheel_slip_rl, wheel_slip_rr,
    pressure_fl, pressure_fr, pressure_rl, pressure_rr,
    wheel_speed_fl, wheel_speed_fr, wheel_speed_rl, wheel_speed_rr,
    core_temp_fl, core_temp_fr, core_temp_rl, core_temp_rr,
    suspension_fl, suspension_fr, suspension_rl, suspension_rr,
    tc, heading, pitch, roll,
    damage_front, damage_rear, damage_left, damage_right, damage_center,
    pit_limiter, abs_activity, lap_number, current_lap_ms, lap_position,
    field_count
};

struct Channel {
    const char* name;
    const char* short_name;
    const char* unit;
    const char* key;
    double scale;
};

constexpr std::array<Channel, field_count> kChannels{{
    {"Time", "Time", "s", "elapsed", 1.0},
    {"Throttle Position", "Throttle", "%", "throttle", 100.0},
    {"Brake Position", "Brake", "%", "brake", 100.0},
    {"Fuel Level", "Fuel", "l", "fuel", 1.0},
    {"Gear", "Gear", "", "gear", 1.0},
    {"Engine RPM", "RPM", "rpm", "rpm", 1.0},
    {"Steering Position", "Steer", "rad", "steering_angle", 1.0},
    {"Ground Speed", "Speed", "km/h", "speed_kmh", 1.0},
    {"Velocity X", "Vel X", "m/s", "velocity_x", 1.0},
    {"Velocity Y", "Vel Y", "m/s", "velocity_y", 1.0},
    {"Velocity Z", "Vel Z", "m/s", "velocity_z", 1.0},
    {"G Force X", "G X", "g", "g_x", 1.0},
    {"G Force Y", "G Y", "g", "g_y", 1.0},
    {"G Force Z", "G Z", "g", "g_z", 1.0},
    {"Wheel Slip FL", "Slip FL", "", "wheel_slip_fl", 1.0},
    {"Wheel Slip FR", "Slip FR", "", "wheel_slip_fr", 1.0},
    {"Wheel Slip RL", "Slip RL", "", "wheel_slip_rl", 1.0},
    {"Wheel Slip RR", "Slip RR", "", "wheel_slip_rr", 1.0},
    {"Tyre Pressure FL", "Press FL", "psi", "pressure_fl", 1.0},
    {"Tyre Pressure FR", "Press FR", "psi", "pressure_fr", 1.0},
    {"Tyre Pressure RL", "Press RL", "psi", "pressure_rl", 1.0},
    {"Tyre Pressure RR", "Press RR", "psi", "pressure_rr", 1.0},
    {"Wheel Speed FL", "WhlSp FL", "rad/s", "wheel_speed_fl", 1.0},
    {"Wheel Speed FR", "WhlSp FR", "rad/s", "wheel_speed_fr", 1.0},
    {"Wheel Speed RL", "WhlSp RL", "rad/s", "wheel_speed_rl", 1.0},
    {"Wheel Speed RR", "WhlSp RR", "rad/s", "wheel_speed_rr", 1.0},
    {"Tyre Core Temp FL", "Core FL", "C", "core_temp_fl", 1.0},
    {"Tyre Core Temp FR", "Core FR", "C", "core_temp_fr", 1.0},
    {"Tyre Core Temp RL", "Core RL", "C", "core_temp_rl", 1.0},
    {"Tyre Core Temp RR", "Core RR", "C", "core_temp_rr", 1.0},
    {"Suspension Travel FL", "Susp FL", "m", "suspension_fl", 1.0},
    {"Suspension Travel FR", "Susp FR", "m", "suspension_fr", 1.0},
    {"Suspension Travel RL", "Susp RL", "m", "suspension_rl", 1.0},
    {"Suspension Travel RR", "Susp RR", "m", "suspension_rr", 1.0},
    {"TC Activity", "TC", "", "tc", 1.0},
    {"Heading", "Heading", "rad", "heading", 1.0},
    {"Pitch", "Pitch", "rad", "pitch", 1.0},
    {"Roll", "Roll", "rad", "roll", 1.0},
    {"Damage Front", "Dmg F", "", "damage_front", 1.0},
    {"Damage Rear", "Dmg R", "", "damage_rear", 1.0},
    {"Damage Left", "Dmg L", "", "damage_left", 1.0},
    {"Damage Right", "Dmg Rgt", "", "damage_right", 1.0},
    {"Damage Center", "Dmg C", "", "damage_center", 1.0},
    {"Pit Limiter", "Pit Lim", "", "pit_limiter", 1.0},
    {"ABS Activity", "ABS", "", "abs", 1.0},
    {"Lap Number", "Lap", "", "lap_number", 1.0},
    {"Lap Time", "Lap Time", "s", "current_lap_ms", 0.001},
    {"Lap Position", "Lap Pos", "%", "lap_position", 100.0},
}};

constexpr std::uint64_t field_bit(Field field) {
    return std::uint64_t{1} << static_cast<std::size_t>(field);
}

constexpr std::uint64_t all_field_bits() {
    return (std::uint64_t{1} << field_count) - 1;
}

struct Frame {
    std::array<double, field_count> value{};
    std::uint64_t valid_mask = 0;
    int completed_lap_ms = 0;
    int delta_ms = 0;
};

struct Metadata {
    std::chrono::system_clock::time_point started_at = std::chrono::system_clock::now();
    std::string simulator;
    std::string driver;
    std::string vehicle;
    std::string venue;
    std::string session;
};

enum class Protocol { v4, legacy_v3 };

struct Options {
    std::string pi_host = "rapid";
    unsigned short pi_port = 9001;
    int sample_rate = 50;
    fs::path output_directory;
    fs::path config_path;
    std::vector<std::uint8_t> auth_key;
    Protocol protocol = Protocol::v4;
    bool local_recording = false;
    bool no_forward = false;
    bool headless = false;
    bool self_test = false;
};

std::string wide_to_utf8(std::wstring_view input) {
    if (input.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, input.data(), static_cast<int>(input.size()),
                                         nullptr, 0, nullptr, nullptr);
    std::string output(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, input.data(), static_cast<int>(input.size()),
                        output.data(), size, nullptr, nullptr);
    return output;
}

std::wstring utf8_to_wide(std::string_view input) {
    if (input.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, input.data(), static_cast<int>(input.size()), nullptr, 0);
    std::wstring output(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, input.data(), static_cast<int>(input.size()), output.data(), size);
    return output;
}

std::string lower_ascii(std::string input) {
    std::transform(input.begin(), input.end(), input.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return input;
}

fs::path default_output_directory() {
    PWSTR known = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_DEFAULT, nullptr, &known))) {
        fs::path result = fs::path(known) / L"raPId Telemetry";
        CoTaskMemFree(known);
        return result;
    }
    return fs::current_path() / L"raPId Telemetry";
}

fs::path default_config_path() {
    PWSTR known = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &known))) {
        fs::path result = fs::path(known) / L"raPId" / L"daemon.conf";
        CoTaskMemFree(known);
        return result;
    }
    return fs::current_path() / L"daemon.conf";
}

std::string trim_ascii(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::vector<std::uint8_t> decode_auth_key(std::string text, std::string_view source) {
    text = trim_ascii(std::move(text));
    if (text.size() != 64) {
        throw std::runtime_error(std::string(source) + " must contain exactly 64 hexadecimal characters (a 256-bit key)");
    }
    const auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::vector<std::uint8_t> key(32);
    for (std::size_t i = 0; i < key.size(); ++i) {
        const int high = nibble(text[i * 2]);
        const int low = nibble(text[i * 2 + 1]);
        if (high < 0 || low < 0) {
            throw std::runtime_error(std::string(source) + " contains a non-hexadecimal character");
        }
        key[i] = static_cast<std::uint8_t>((high << 4) | low);
    }
    return key;
}

std::string read_small_text_file(const fs::path& path, std::string_view description) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot open " + std::string(description) + ": " + path.string());
    std::string value((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (value.size() > 64 * 1024) throw std::runtime_error(std::string(description) + " is unexpectedly large");
    return value;
}

bool parse_bool(std::string value, std::string_view name) {
    value = lower_ascii(trim_ascii(std::move(value)));
    if (value == "1" || value == "true" || value == "yes" || value == "on") return true;
    if (value == "0" || value == "false" || value == "no" || value == "off") return false;
    throw std::runtime_error(std::string(name) + " must be true or false");
}

Protocol parse_protocol(std::string value, std::string_view source) {
    value = lower_ascii(trim_ascii(std::move(value)));
    if (value == "v4" || value == "4") return Protocol::v4;
    if (value == "v3" || value == "legacy-v3" || value == "legacy_v3") return Protocol::legacy_v3;
    throw std::runtime_error(std::string(source) + " must be v4 or v3");
}

void apply_config_file(Options& options, const fs::path& path, bool required) {
    if (!fs::exists(path)) {
        if (required) throw std::runtime_error("Config file not found: " + path.string());
        return;
    }
    std::istringstream lines(read_small_text_file(path, "daemon config"));
    std::string line;
    std::size_t number = 0;
    while (std::getline(lines, line)) {
        ++number;
        line = trim_ascii(std::move(line));
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        const auto equals = line.find('=');
        if (equals == std::string::npos) {
            throw std::runtime_error("Invalid daemon config line " + std::to_string(number));
        }
        const auto name = lower_ascii(trim_ascii(line.substr(0, equals)));
        const auto value = trim_ascii(line.substr(equals + 1));
        if (name == "pi_host") {
            options.pi_host = value;
        } else if (name == "pi_port") {
            const int port = std::stoi(value);
            if (port < 1 || port > 65535) throw std::runtime_error("pi_port must be 1-65535");
            options.pi_port = static_cast<unsigned short>(port);
        } else if (name == "sample_rate") {
            options.sample_rate = std::stoi(value);
            if (options.sample_rate < 1 || options.sample_rate > 100) throw std::runtime_error("sample_rate must be 1-100");
        } else if (name == "protocol") {
            options.protocol = parse_protocol(value, "protocol");
        } else if (name == "auth_key") {
            options.auth_key = decode_auth_key(value, "auth_key");
        } else if (name == "auth_key_file") {
            fs::path key_path = utf8_to_wide(value);
            if (key_path.is_relative()) key_path = path.parent_path() / key_path;
            options.auth_key = decode_auth_key(read_small_text_file(key_path, "auth key file"), "auth key file");
        } else if (name == "local_recording") {
            options.local_recording = parse_bool(value, "local_recording");
        } else if (name == "output_directory") {
            options.output_directory = utf8_to_wide(value);
        } else if (name == "no_forward") {
            options.no_forward = parse_bool(value, "no_forward");
        } else {
            throw std::runtime_error("Unknown daemon config key: " + name);
        }
    }
}

std::wstring option_value(int& index, int argc, wchar_t** argv, const wchar_t* name) {
    if (index + 1 >= argc) throw std::runtime_error(wide_to_utf8(name) + " requires a value");
    return argv[++index];
}

Options parse_options(int argc, wchar_t** argv) {
    Options options;
    options.output_directory = default_output_directory();
    options.config_path = default_config_path();

    bool explicit_config = false;
    for (int i = 1; i < argc; ++i) {
        std::wstring raw = argv[i];
        std::transform(raw.begin(), raw.end(), raw.begin(), ::towlower);
        if (raw == L"--config" || raw == L"-config") {
            options.config_path = option_value(i, argc, argv, L"--config");
            explicit_config = true;
        }
    }
    apply_config_file(options, options.config_path, explicit_config);

    std::array<wchar_t, 256> environment_key{};
    const DWORD environment_length = GetEnvironmentVariableW(
        L"RAPID_TELEMETRY_KEY", environment_key.data(), static_cast<DWORD>(environment_key.size()));
    if (environment_length > 0) {
        if (environment_length >= environment_key.size()) {
            throw std::runtime_error("RAPID_TELEMETRY_KEY is too long");
        }
        options.auth_key = decode_auth_key(wide_to_utf8(environment_key.data()), "RAPID_TELEMETRY_KEY");
    }

    for (int i = 1; i < argc; ++i) {
        std::wstring raw = argv[i];
        std::transform(raw.begin(), raw.end(), raw.begin(), ::towlower);
        if (raw == L"--config" || raw == L"-config") {
            ++i; // Applied before environment and command-line overrides.
        } else if (raw == L"--pi-host" || raw == L"-pihost") {
            options.pi_host = wide_to_utf8(option_value(i, argc, argv, L"--pi-host"));
        } else if (raw == L"--pi-port" || raw == L"-piport") {
            const int value = std::stoi(option_value(i, argc, argv, L"--pi-port"));
            if (value < 1 || value > 65535) throw std::runtime_error("Pi port must be 1-65535");
            options.pi_port = static_cast<unsigned short>(value);
        } else if (raw == L"--sample-rate" || raw == L"-samplerate") {
            options.sample_rate = std::stoi(option_value(i, argc, argv, L"--sample-rate"));
            if (options.sample_rate < 1 || options.sample_rate > 100) {
                throw std::runtime_error("Sample rate must be 1-100 Hz");
            }
        } else if (raw == L"--output-directory" || raw == L"-outputdirectory") {
            options.output_directory = option_value(i, argc, argv, L"--output-directory");
        } else if (raw == L"--protocol") {
            options.protocol = parse_protocol(wide_to_utf8(option_value(i, argc, argv, L"--protocol")), "--protocol");
        } else if (raw == L"--auth-key") {
            options.auth_key = decode_auth_key(wide_to_utf8(option_value(i, argc, argv, L"--auth-key")), "--auth-key");
        } else if (raw == L"--auth-key-file") {
            const fs::path path = option_value(i, argc, argv, L"--auth-key-file");
            options.auth_key = decode_auth_key(read_small_text_file(path, "auth key file"), "auth key file");
        } else if (raw == L"--local-recording") {
            options.local_recording = true;
        } else if (raw == L"--no-local-recording") {
            options.local_recording = false;
        } else if (raw == L"--no-forward" || raw == L"-noforward") {
            options.no_forward = true;
        } else if (raw == L"--headless" || raw == L"-headless") {
            options.headless = true;
        } else if (raw == L"--self-test" || raw == L"-selftest") {
            options.self_test = true;
        } else if (raw == L"--help" || raw == L"-?" || raw == L"/?") {
            std::puts("raPId native telemetry daemon\n"
                      "  --pi-host HOST            Pi hostname/address (default: rapid)\n"
                      "  --pi-port PORT            Pi UDP port (default: 9001)\n"
                      "  --sample-rate HZ          Capture rate 1-100 (default: 50)\n"
                      "  --protocol v4|v3          Authenticated binary v4 (default) or legacy JSON v3\n"
                      "  --auth-key HEX            64-hex-character v4 HMAC key\n"
                      "  --auth-key-file PATH      Read the v4 HMAC key from a file\n"
                      "  --config PATH             key=value config (default: %LOCALAPPDATA%\\raPId\\daemon.conf)\n"
                      "  --local-recording         Opt in to the PC-side emergency .ld fallback\n"
                      "  --output-directory PATH   Local fallback/log directory\n"
                      "  --no-forward              Disable Pi forwarding\n"
                      "  --headless --self-test\n"
                      "Key precedence: command line, RAPID_TELEMETRY_KEY, config file.");
            std::exit(0);
        } else {
            throw std::runtime_error("Unknown option: " + wide_to_utf8(argv[i]));
        }
    }
    if (options.pi_host.empty()) throw std::runtime_error("Pi host cannot be empty");
    if (options.protocol == Protocol::v4 && !options.no_forward && options.auth_key.empty() && !options.self_test) {
        throw std::runtime_error(
            "Protocol v4 requires a 256-bit HMAC key. Set --auth-key, --auth-key-file, "
            "RAPID_TELEMETRY_KEY, or auth_key in the daemon config.");
    }
    return options;
}

class Logger {
public:
    Logger(fs::path path, bool console) : path_(std::move(path)), console_(console) {}

    void write(const std::string& message) {
        std::lock_guard lock(mutex_);
        std::error_code error;
        fs::create_directories(path_.parent_path(), error);
        std::ofstream output(path_, std::ios::app);
        const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        std::tm local{};
        localtime_s(&local, &now);
        std::ostringstream line;
        line << std::put_time(&local, "%Y-%m-%d %H:%M:%S") << " " << message << '\n';
        output << line.str();
        if (console_) std::fputs(line.str().c_str(), stdout);
    }

private:
    fs::path path_;
    bool console_;
    std::mutex mutex_;
};

class Mapping {
public:
    Mapping() = default;
    Mapping(const Mapping&) = delete;
    Mapping& operator=(const Mapping&) = delete;
    Mapping(Mapping&& other) noexcept { *this = std::move(other); }
    Mapping& operator=(Mapping&& other) noexcept {
        if (this != &other) {
            close();
            handle_ = std::exchange(other.handle_, nullptr);
            data_ = std::exchange(other.data_, nullptr);
        }
        return *this;
    }
    ~Mapping() { close(); }

    bool open(const wchar_t* name) {
        close();
        handle_ = OpenFileMappingW(FILE_MAP_READ, FALSE, name);
        if (!handle_) return false;
        data_ = static_cast<const std::byte*>(MapViewOfFile(handle_, FILE_MAP_READ, 0, 0, 0));
        if (!data_) {
            CloseHandle(handle_);
            handle_ = nullptr;
            return false;
        }
        return true;
    }

    void close() {
        if (data_) UnmapViewOfFile(data_);
        if (handle_) CloseHandle(handle_);
        data_ = nullptr;
        handle_ = nullptr;
    }

    explicit operator bool() const { return data_ != nullptr; }

    template <typename T>
    T read(std::size_t offset) const {
        T value{};
        std::memcpy(&value, data_ + offset, sizeof(T));
        return value;
    }

    std::string ascii(std::size_t offset, std::size_t length) const {
        const auto* begin = reinterpret_cast<const char*>(data_ + offset);
        std::size_t used = 0;
        while (used < length && begin[used] != '\0') ++used;
        return std::string(begin, used);
    }

    std::string wide(std::size_t offset, std::size_t characters) const {
        const auto* begin = reinterpret_cast<const wchar_t*>(data_ + offset);
        std::size_t used = 0;
        while (used < characters && begin[used] != L'\0') ++used;
        return wide_to_utf8(std::wstring_view(begin, used));
    }

private:
    HANDLE handle_ = nullptr;
    const std::byte* data_ = nullptr;
};

enum class Game { none, acc, ac, ace, iracing };

const char* game_name(Game game) {
    switch (game) {
        case Game::acc: return "ACC";
        case Game::ac: return "AC";
        case Game::ace: return "ACE";
        case Game::iracing: return "iRacing";
        default: return "";
    }
}

Game game_for_executable(const wchar_t* executable) {
    if (_wcsicmp(executable, L"AC2-Win64-Shipping.exe") == 0 || _wcsicmp(executable, L"acc.exe") == 0) {
        return Game::acc;
    }
    if (_wcsicmp(executable, L"acs.exe") == 0 || _wcsicmp(executable, L"acs_x86.exe") == 0) {
        return Game::ac;
    }
    if (_wcsicmp(executable, L"AssettoCorsaEVO.exe") == 0 ||
        _wcsicmp(executable, L"AssettoCorsaEVO-Win64-Shipping.exe") == 0 ||
        _wcsicmp(executable, L"ACE-Win64-Shipping.exe") == 0) {
        return Game::ace;
    }
    if (_wcsicmp(executable, L"iRacingSim64DX11.exe") == 0 ||
        _wcsicmp(executable, L"iRacingSim64DX11Obsolete.exe") == 0 ||
        _wcsicmp(executable, L"iRacingSim.exe") == 0) {
        return Game::iracing;
    }
    return Game::none;
}

class RunningGame {
public:
    RunningGame() = default;
    RunningGame(Game game, DWORD process_id, HANDLE process)
        : game_(game), process_id_(process_id), process_(process) {}
    RunningGame(const RunningGame&) = delete;
    RunningGame& operator=(const RunningGame&) = delete;
    RunningGame(RunningGame&& other) noexcept { *this = std::move(other); }
    RunningGame& operator=(RunningGame&& other) noexcept {
        if (this != &other) {
            reset();
            game_ = std::exchange(other.game_, Game::none);
            process_id_ = std::exchange(other.process_id_, 0);
            process_ = std::exchange(other.process_, nullptr);
        }
        return *this;
    }
    ~RunningGame() { reset(); }

    explicit operator bool() const { return game_ != Game::none; }
    Game game() const { return game_; }

    bool running() const {
        if (!process_) return process_id_ != 0;
        return WaitForSingleObject(process_, 0) == WAIT_TIMEOUT;
    }

    void reset() {
        if (process_) CloseHandle(process_);
        process_ = nullptr;
        process_id_ = 0;
        game_ = Game::none;
    }

private:
    Game game_ = Game::none;
    DWORD process_id_ = 0;
    HANDLE process_ = nullptr;
};

RunningGame find_running_game() {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return {};
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    RunningGame result;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            const Game game = game_for_executable(entry.szExeFile);
            if (game != Game::none) {
                HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
                                             FALSE, entry.th32ProcessID);
                result = RunningGame(game, entry.th32ProcessID, process);
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return result;
}

class Adapter {
public:
    virtual ~Adapter() = default;
    virtual bool live() = 0;
    virtual bool read(Frame& frame) = 0;
    virtual bool connected() = 0;
    virtual const char* kind() const = 0;
    Metadata metadata;
};

class AssettoAdapter final : public Adapter {
public:
    static std::unique_ptr<AssettoAdapter> open(Game game) {
        auto adapter = std::unique_ptr<AssettoAdapter>(new AssettoAdapter);
        if (!adapter->physics_.open(L"Local\\acpmf_physics") ||
            !adapter->graphics_.open(L"Local\\acpmf_graphics") ||
            !adapter->static_.open(L"Local\\acpmf_static")) return nullptr;
        adapter->metadata.simulator = game == Game::acc ? "ACC" : "AC";
        const int session_type = adapter->graphics_.read<std::int32_t>(8);
        static constexpr const char* sessions[] = {
            "Practice", "Qualifying", "Race", "Hotlap", "Time Attack",
            "Drift", "Drag", "Hotstint", "Superpole"
        };
        adapter->metadata.session = session_type >= 0 && session_type < 9
            ? sessions[session_type] : adapter->metadata.simulator;
        adapter->metadata.vehicle = adapter->static_.wide(68, 33);
        adapter->metadata.venue = adapter->static_.wide(134, 33);
        adapter->metadata.driver = adapter->static_.wide(200, 33);
        const auto surname = adapter->static_.wide(266, 33);
        if (!surname.empty()) adapter->metadata.driver += " " + surname;
        return adapter;
    }

    bool live() override { return graphics_.read<std::int32_t>(4) == 2; }

    bool connected() override { return true; }

    const char* kind() const override { return "Assetto"; }

    bool read(Frame& frame) override {
        const int before = physics_.read<std::int32_t>(0);
        frame.valid_mask = all_field_bits();
        auto& v = frame.value;
        v[throttle] = physics_.read<float>(4); v[brake] = physics_.read<float>(8);
        v[fuel] = physics_.read<float>(12); v[gear] = physics_.read<std::int32_t>(16) - 1;
        v[rpm] = physics_.read<std::int32_t>(20); v[steering_angle] = physics_.read<float>(24);
        v[speed_kmh] = physics_.read<float>(28);
        v[velocity_x] = physics_.read<float>(32); v[velocity_y] = physics_.read<float>(36);
        v[velocity_z] = physics_.read<float>(40); v[g_x] = physics_.read<float>(44);
        v[g_y] = physics_.read<float>(48); v[g_z] = physics_.read<float>(52);
        read_corners(v, wheel_slip_fl, 56); read_corners(v, pressure_fl, 88);
        read_corners(v, wheel_speed_fl, 104); read_corners(v, core_temp_fl, 152);
        read_corners(v, suspension_fl, 184);
        v[tc] = physics_.read<float>(204); v[heading] = physics_.read<float>(208);
        v[pitch] = physics_.read<float>(212); v[roll] = physics_.read<float>(216);
        for (std::size_t i = 0; i < 5; ++i) v[damage_front + i] = physics_.read<float>(224 + i * 4);
        v[pit_limiter] = physics_.read<std::int32_t>(248); v[abs_activity] = physics_.read<float>(252);
        v[lap_number] = graphics_.read<std::int32_t>(132) + 1;
        v[current_lap_ms] = std::max(0, graphics_.read<std::int32_t>(140));
        frame.completed_lap_ms = graphics_.read<std::int32_t>(144);
        if (frame.completed_lap_ms <= 0 || frame.completed_lap_ms == std::numeric_limits<int>::max()) {
            frame.completed_lap_ms = 0;
        }
        v[lap_position] = graphics_.read<float>(248);
        sanitize(frame);
        return before == physics_.read<std::int32_t>(0);
    }

private:
    void read_corners(std::array<double, field_count>& values, std::size_t first, std::size_t offset) {
        for (std::size_t i = 0; i < 4; ++i) values[first + i] = physics_.read<float>(offset + i * 4);
    }

    static void sanitize(Frame& frame) {
        for (auto& value : frame.value) if (!std::isfinite(value)) value = 0.0;
    }

    Mapping physics_, graphics_, static_;
};

class AceAdapter final : public Adapter {
public:
    static std::unique_ptr<AceAdapter> open() {
        auto adapter = std::unique_ptr<AceAdapter>(new AceAdapter);
        if (!adapter->physics_.open(L"Local\\acevo_pmf_physics") ||
            !adapter->graphics_.open(L"Local\\acevo_pmf_graphics") ||
            !adapter->static_.open(L"Local\\acevo_pmf_static")) return nullptr;
        adapter->metadata.simulator = "ACE";
        adapter->metadata.vehicle = "Assetto Corsa EVO car";
        adapter->metadata.venue = "Assetto Corsa EVO";
        const int session_type = adapter->static_.read<std::int32_t>(32);
        static constexpr const char* sessions[] = {
            "Unknown", "Practice", "Qualifying", "Race", "Hotlap", "Time Attack", "Drift", "Drag"
        };
        adapter->metadata.session = session_type >= 0 && session_type < 8
            ? sessions[session_type] : adapter->static_.ascii(36, 33);
        return adapter;
    }

    bool live() override { return graphics_.read<std::int32_t>(4) == 2; }
    bool connected() override { return true; }
    const char* kind() const override { return "ACE"; }

    bool read(Frame& frame) override {
        const int before = physics_.read<std::int32_t>(0);
        frame.valid_mask = all_field_bits();
        for (std::size_t i = damage_front; i <= damage_center; ++i) {
            frame.valid_mask &= ~(std::uint64_t{1} << i);
        }
        frame.valid_mask &= ~field_bit(pit_limiter);
        auto& v = frame.value;
        v[throttle] = physics_.read<float>(4); v[brake] = physics_.read<float>(8);
        v[fuel] = physics_.read<float>(12); v[gear] = physics_.read<std::int32_t>(16) - 1;
        v[rpm] = physics_.read<std::int32_t>(20); v[steering_angle] = physics_.read<float>(24);
        v[speed_kmh] = physics_.read<float>(28);
        v[velocity_x] = physics_.read<float>(32); v[velocity_y] = physics_.read<float>(36);
        v[velocity_z] = physics_.read<float>(40); v[g_x] = physics_.read<float>(44);
        v[g_y] = physics_.read<float>(48); v[g_z] = physics_.read<float>(52);
        read_corners(v, wheel_slip_fl, 56); read_corners(v, pressure_fl, 88);
        read_corners(v, wheel_speed_fl, 104); read_corners(v, core_temp_fl, 152);
        read_corners(v, suspension_fl, 184);
        v[tc] = physics_.read<std::int32_t>(672); v[abs_activity] = physics_.read<std::int32_t>(676);
        v[heading] = physics_.read<float>(208); v[pitch] = physics_.read<float>(212);
        v[roll] = physics_.read<float>(216); v[current_lap_ms] = std::max(0, graphics_.read<std::int32_t>(188));
        frame.delta_ms = graphics_.read<std::int32_t>(184); v[lap_position] = graphics_.read<float>(1244);
        const int current = static_cast<int>(v[current_lap_ms]);
        if (last_current_lap_ms_ > 15000 && current < 2000) {
            last_lap_ms_ = last_current_lap_ms_;
            ++synthetic_lap_;
        }
        last_current_lap_ms_ = current;
        frame.completed_lap_ms = last_lap_ms_;
        v[lap_number] = synthetic_lap_;
        for (auto& value : v) if (!std::isfinite(value)) value = 0.0;
        return before == physics_.read<std::int32_t>(0);
    }

private:
    void read_corners(std::array<double, field_count>& values, std::size_t first, std::size_t offset) {
        for (std::size_t i = 0; i < 4; ++i) values[first + i] = physics_.read<float>(offset + i * 4);
    }

    Mapping physics_, graphics_, static_;
    int last_lap_ms_ = 0;
    int last_current_lap_ms_ = 0;
    int synthetic_lap_ = 1;
};

class IracingAdapter final : public Adapter {
public:
    struct Variable { int type; int offset; int count; };

    static std::unique_ptr<IracingAdapter> open() {
        auto adapter = std::unique_ptr<IracingAdapter>(new IracingAdapter);
        if (!adapter->mapping_.open(L"Local\\IRSDKMemMapFileName")) return nullptr;
        if ((adapter->mapping_.read<std::int32_t>(4) & 1) == 0) return nullptr;
        const int count = adapter->mapping_.read<std::int32_t>(24);
        const int offset = adapter->mapping_.read<std::int32_t>(28);
        if (count < 1 || count > 4096 || offset < 0) return nullptr;
        for (int i = 0; i < count; ++i) {
            const std::size_t at = static_cast<std::size_t>(offset) + static_cast<std::size_t>(i) * 144;
            const auto name = adapter->mapping_.ascii(at + 16, 32);
            if (!name.empty()) {
                adapter->variables_[name] = {
                    adapter->mapping_.read<std::int32_t>(at),
                    adapter->mapping_.read<std::int32_t>(at + 4),
                    adapter->mapping_.read<std::int32_t>(at + 8)
                };
            }
        }
        std::string yaml;
        const int yaml_length = adapter->mapping_.read<std::int32_t>(16);
        const int yaml_offset = adapter->mapping_.read<std::int32_t>(20);
        if (yaml_length > 0 && yaml_length < 10 * 1024 * 1024 && yaml_offset >= 0) {
            yaml = adapter->mapping_.ascii(static_cast<std::size_t>(yaml_offset), static_cast<std::size_t>(yaml_length));
        }
        adapter->metadata.simulator = "iRacing";
        adapter->metadata.driver = yaml_value(yaml, "UserName");
        adapter->metadata.vehicle = yaml_value(yaml, "CarScreenName");
        adapter->metadata.venue = yaml_value(yaml, "TrackDisplayName");
        adapter->metadata.session = yaml_value(yaml, "SessionType");
        if (adapter->metadata.session.empty()) adapter->metadata.session = "iRacing";
        return adapter;
    }

    bool live() override {
        const int offset = buffer_offset();
        return boolean(offset, "IsOnTrack", boolean(offset, "IsOnTrackCar", false));
    }

    bool connected() override { return (mapping_.read<std::int32_t>(4) & 1) != 0; }
    const char* kind() const override { return "iRacing"; }

    bool read(Frame& frame) override {
        if (!connected()) return false;
        const int offset = buffer_offset();
        frame.valid_mask = field_bit(elapsed) | field_bit(throttle) | field_bit(brake) |
            field_bit(fuel) | field_bit(gear) | field_bit(rpm) | field_bit(steering_angle) |
            field_bit(speed_kmh) | field_bit(velocity_x) | field_bit(velocity_y) |
            field_bit(velocity_z) | field_bit(g_x) | field_bit(g_y) | field_bit(g_z) |
            field_bit(suspension_fl) | field_bit(suspension_fr) |
            field_bit(suspension_rl) | field_bit(suspension_rr) |
            field_bit(pit_limiter) | field_bit(lap_number) |
            field_bit(current_lap_ms) | field_bit(lap_position);
        auto& v = frame.value;
        v[throttle] = number(offset, "Throttle"); v[brake] = number(offset, "Brake");
        v[fuel] = number(offset, "FuelLevel"); v[gear] = number(offset, "Gear");
        v[rpm] = number(offset, "RPM"); v[steering_angle] = number(offset, "SteeringWheelAngle");
        v[speed_kmh] = number(offset, "Speed") * 3.6;
        v[velocity_x] = number(offset, "VelocityX"); v[velocity_y] = number(offset, "VelocityY");
        v[velocity_z] = number(offset, "VelocityZ");
        constexpr double gravity = 9.80665;
        v[g_x] = number(offset, "LatAccel") / gravity;
        v[g_y] = number(offset, "VertAccel") / gravity;
        v[g_z] = number(offset, "LongAccel") / gravity;
        v[lap_number] = number(offset, "Lap");
        v[current_lap_ms] = std::max(0.0, number(offset, "LapCurrentLapTime") * 1000.0);
        frame.completed_lap_ms = std::max(0, static_cast<int>(number(offset, "LapLastLapTime") * 1000.0));
        frame.delta_ms = static_cast<int>(number(offset, "LapDeltaToBestLap") * 1000.0);
        v[lap_position] = number(offset, "LapDistPct");
        v[pit_limiter] = (static_cast<int>(number(offset, "EngineWarnings")) & 0x10) != 0;
        static constexpr const char* shocks[] = {"LFshockDefl", "RFshockDefl", "LRshockDefl", "RRshockDefl"};
        for (std::size_t i = 0; i < 4; ++i) v[suspension_fl + i] = number(offset, shocks[i]);
        for (auto& value : v) if (!std::isfinite(value)) value = 0.0;
        return true;
    }

private:
    static std::string trim(std::string value) {
        const auto first = value.find_first_not_of(" \t\r\n\"'");
        const auto last = value.find_last_not_of(" \t\r\n\"'");
        if (first == std::string::npos) return {};
        return value.substr(first, last - first + 1);
    }

    static std::string yaml_value(const std::string& yaml, const std::string& name) {
        std::size_t at = 0;
        while (at < yaml.size()) {
            const auto end = yaml.find('\n', at);
            auto line = yaml.substr(at, end == std::string::npos ? std::string::npos : end - at);
            const auto first = line.find_first_not_of(" \t");
            if (first != std::string::npos && line.compare(first, name.size(), name) == 0) {
                const auto colon = line.find(':', first + name.size());
                if (colon != std::string::npos) return trim(line.substr(colon + 1));
            }
            if (end == std::string::npos) break;
            at = end + 1;
        }
        return {};
    }

    int buffer_offset() const {
        const int count = std::min(mapping_.read<std::int32_t>(32), 4);
        int best_tick = std::numeric_limits<int>::min();
        int best_offset = -1;
        for (int i = 0; i < count; ++i) {
            const std::size_t at = 48 + static_cast<std::size_t>(i) * 16;
            const int tick = mapping_.read<std::int32_t>(at);
            if (tick > best_tick) {
                best_tick = tick;
                best_offset = mapping_.read<std::int32_t>(at + 4);
            }
        }
        return best_offset;
    }

    double number(int buffer, const std::string& name, double fallback = 0.0) const {
        const auto found = variables_.find(name);
        if (found == variables_.end() || buffer < 0) return fallback;
        const auto at = static_cast<std::size_t>(buffer + found->second.offset);
        switch (found->second.type) {
            case 0: return mapping_.read<unsigned char>(at);
            case 1: return mapping_.read<unsigned char>(at) != 0;
            case 2: case 3: return mapping_.read<std::int32_t>(at);
            case 4: return mapping_.read<float>(at);
            case 5: return mapping_.read<double>(at);
            default: return fallback;
        }
    }

    bool boolean(int buffer, const std::string& name, bool fallback) const {
        return number(buffer, name, fallback ? 1.0 : 0.0) != 0.0;
    }

    Mapping mapping_;
    std::unordered_map<std::string, Variable> variables_;
};

std::unique_ptr<Adapter> open_adapter(Game game) {
    switch (game) {
        case Game::ace: return AceAdapter::open();
        case Game::iracing: return IracingAdapter::open();
        case Game::acc:
        case Game::ac: return AssettoAdapter::open(game);
        default: return nullptr;
    }
}

void append_json_string(std::string& output, std::string_view value) {
    output.push_back('"');
    static constexpr char hex[] = "0123456789abcdef";
    for (const unsigned char character : value) {
        switch (character) {
            case '"': output += "\\\""; break;
            case '\\': output += "\\\\"; break;
            case '\b': output += "\\b"; break;
            case '\f': output += "\\f"; break;
            case '\n': output += "\\n"; break;
            case '\r': output += "\\r"; break;
            case '\t': output += "\\t"; break;
            default:
                if (character < 0x20) {
                    output += "\\u00";
                    output.push_back(hex[character >> 4]);
                    output.push_back(hex[character & 0x0f]);
                } else {
                    output.push_back(static_cast<char>(character));
                }
        }
    }
    output.push_back('"');
}

void append_number(std::string& output, double value) {
    char buffer[48];
    if (!std::isfinite(value)) value = 0.0;
    const int length = std::snprintf(buffer, sizeof(buffer), "%.9g", value);
    output.append(buffer, static_cast<std::size_t>(std::max(0, length)));
}

std::string status_json(std::string_view state, const Adapter* adapter, int rate) {
    std::string output = "{\"version\":3,\"type\":\"status\",\"state\":";
    append_json_string(output, state);
    output += ",\"simulator\":";
    if (adapter) append_json_string(output, adapter->metadata.simulator); else output += "null";
    output += ",\"sample_rate_hz\":" + std::to_string(rate) + "}";
    return output;
}

std::string telemetry_json(const Frame& frame, const Adapter& adapter, int rate) {
    std::string output;
    output.reserve(2300);
    output = "{\"version\":3,\"type\":\"telemetry\",\"simulator\":";
    append_json_string(output, adapter.metadata.simulator);
    output += ",\"sample_rate_hz\":" + std::to_string(rate);
    const std::pair<const char*, const std::string*> metadata[] = {
        {"track_name", &adapter.metadata.venue}, {"car_model", &adapter.metadata.vehicle},
        {"driver_name", &adapter.metadata.driver}, {"session_name", &adapter.metadata.session}
    };
    for (const auto& [name, value] : metadata) {
        output += ",\""; output += name; output += "\":";
        append_json_string(output, *value);
    }
    output += ",\"telemetry\":{";
    bool first = true;
    for (std::size_t i = 1; i < field_count; ++i) {
        if (!first) output.push_back(',');
        first = false;
        output.push_back('"'); output += kChannels[i].key; output += "\":";
        append_number(output, frame.value[i]);
    }
    output += ",\"completed_lap_ms\":" + std::to_string(frame.completed_lap_ms);
    output += ",\"delta_ms\":" + std::to_string(frame.delta_ms) + "}}";
    return output;
}

// Protocol v4 wire contract. All integers and IEEE-754 floats are little-endian.
// Datagram = 52-byte header + payload + 32-byte HMAC-SHA256. The HMAC covers
// exactly the header and payload. Header fields, in order:
//   magic "RPD4"; version/type/simulator/flags u8; header/payload/schema/channel
//   count/sample-rate/reserved u16; run-id[16]; sequence u64; monotonic-us u64.
// Packet types are telemetry=1, metadata=2, status=3. A shared sequence stream
// starts at zero for every run. Metadata precedes the first telemetry packet.
// flags bit 0 means an active run; bit 1 marks the final ended status packet.
// Telemetry payload = valid-mask u64, completed/delta lap time i32, 48 float32.
// Metadata payload = four u16-length UTF-8 strings: venue, vehicle, driver,
// session. Status payload = state u8, reserved u8, text length u16, sent-packet
// count u64, then UTF-8 text. State values: waiting=0, ready=1, driving=2,
// ended=3.

template <typename T>
void append_le(std::vector<std::uint8_t>& output, T value) {
    static_assert(std::is_integral_v<T>);
    using Unsigned = std::make_unsigned_t<T>;
    const auto bits = static_cast<Unsigned>(value);
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        output.push_back(static_cast<std::uint8_t>((bits >> (i * 8)) & 0xff));
    }
}

void append_float_le(std::vector<std::uint8_t>& output, float value) {
    static_assert(sizeof(float) == sizeof(std::uint32_t));
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    append_le(output, bits);
}

void append_wire_string(std::vector<std::uint8_t>& output, std::string_view value) {
    if (value.size() > 512) throw std::runtime_error("Protocol-v4 metadata field exceeds 512 bytes");
    const auto length = value.size();
    append_le(output, static_cast<std::uint16_t>(length));
    output.insert(output.end(), value.begin(), value.begin() + static_cast<std::ptrdiff_t>(length));
}

enum class V4PacketType : std::uint8_t { telemetry = 1, metadata = 2, status = 3 };
enum class V4StatusState : std::uint8_t { waiting = 0, ready = 1, driving = 2, ended = 3 };

std::uint8_t wire_simulator(Game game) {
    switch (game) {
        case Game::acc: return 1;
        case Game::ac: return 2;
        case Game::ace: return 3;
        case Game::iracing: return 4;
        default: return 0;
    }
}

class HmacSha256 {
public:
    explicit HmacSha256(std::vector<std::uint8_t> key) : key_(std::move(key)) {
        if (key_.size() != 32) throw std::runtime_error("Protocol v4 requires a 256-bit HMAC key");
        check(BCryptOpenAlgorithmProvider(&algorithm_, BCRYPT_SHA256_ALGORITHM, nullptr,
                                          BCRYPT_ALG_HANDLE_HMAC_FLAG),
              "BCryptOpenAlgorithmProvider");
        DWORD returned = 0;
        check(BCryptGetProperty(algorithm_, BCRYPT_OBJECT_LENGTH,
                                reinterpret_cast<PUCHAR>(&object_length_), sizeof(object_length_),
                                &returned, 0),
              "BCryptGetProperty(BCRYPT_OBJECT_LENGTH)");
    }

    HmacSha256(const HmacSha256&) = delete;
    HmacSha256& operator=(const HmacSha256&) = delete;

    ~HmacSha256() {
        if (algorithm_) BCryptCloseAlgorithmProvider(algorithm_, 0);
    }

    std::array<std::uint8_t, kV4HmacSize> sign(std::span<const std::uint8_t> input) const {
        std::vector<std::uint8_t> object(object_length_);
        BCRYPT_HASH_HANDLE hash = nullptr;
        check(BCryptCreateHash(algorithm_, &hash, object.data(), static_cast<ULONG>(object.size()),
                               const_cast<PUCHAR>(key_.data()), static_cast<ULONG>(key_.size()), 0),
              "BCryptCreateHash");
        try {
            check(BCryptHashData(hash, const_cast<PUCHAR>(input.data()),
                                 static_cast<ULONG>(input.size()), 0),
                  "BCryptHashData");
            std::array<std::uint8_t, kV4HmacSize> digest{};
            check(BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0),
                  "BCryptFinishHash");
            BCryptDestroyHash(hash);
            return digest;
        } catch (...) {
            BCryptDestroyHash(hash);
            throw;
        }
    }

private:
    static void check(NTSTATUS status, const char* operation) {
        if (status < 0) {
            std::ostringstream message;
            message << operation << " failed (NTSTATUS 0x" << std::hex
                    << static_cast<unsigned long>(status) << ')';
            throw std::runtime_error(message.str());
        }
    }

    std::vector<std::uint8_t> key_;
    BCRYPT_ALG_HANDLE algorithm_ = nullptr;
    DWORD object_length_ = 0;
};

class V4Encoder {
public:
    explicit V4Encoder(std::vector<std::uint8_t> key)
        : hmac_(std::move(key)), daemon_started_(std::chrono::steady_clock::now()) { end_run(); }

    void begin_run() {
        if (BCryptGenRandom(nullptr, run_id_.data(), static_cast<ULONG>(run_id_.size()),
                            BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) {
            throw std::runtime_error("BCryptGenRandom failed while creating the protocol-v4 run identifier");
        }
        sequence_ = 0;
        run_started_ = std::chrono::steady_clock::now();
        active_ = true;
    }

    void end_run() {
        active_ = false;
        if (BCryptGenRandom(nullptr, run_id_.data(), static_cast<ULONG>(run_id_.size()),
                            BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
            throw std::runtime_error("Cannot create v4 control-stream identifier");
        control_sequence_ = 0;
        daemon_started_ = std::chrono::steady_clock::now();
    }

    bool active() const { return active_; }

    std::uint64_t monotonic_us(std::chrono::steady_clock::time_point now) const {
        const auto origin = active_ ? run_started_ : daemon_started_;
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - origin).count();
        return elapsed > 0 ? static_cast<std::uint64_t>(elapsed) : 0;
    }

    std::vector<std::uint8_t> metadata_packet(Game game, const Metadata& metadata, int rate) {
        std::vector<std::uint8_t> payload;
        payload.reserve(metadata.venue.size() + metadata.vehicle.size() + metadata.driver.size() +
                        metadata.session.size() + 8);
        append_wire_string(payload, metadata.venue);
        append_wire_string(payload, metadata.vehicle);
        append_wire_string(payload, metadata.driver);
        append_wire_string(payload, metadata.session);
        return packet(V4PacketType::metadata, game, active_ ? 0x01 : 0, rate, std::move(payload));
    }

    std::vector<std::uint8_t> telemetry_packet(Game game, const Frame& frame, int rate,
                                               std::chrono::steady_clock::time_point captured_at) {
        std::vector<std::uint8_t> payload;
        payload.reserve(16 + field_count * sizeof(float));
        append_le(payload, frame.valid_mask);
        append_le(payload, static_cast<std::int32_t>(frame.completed_lap_ms));
        append_le(payload, static_cast<std::int32_t>(frame.delta_ms));
        for (const double value : frame.value) {
            append_float_le(payload, static_cast<float>(std::isfinite(value) ? value : 0.0));
        }
        return packet(V4PacketType::telemetry, game, 0x01, rate, std::move(payload), captured_at);
    }

    std::vector<std::uint8_t> status_packet(Game game, V4StatusState state, std::string_view text,
                                            std::uint64_t sent_packets, int rate) {
        const auto text_length = std::min<std::size_t>(text.size(), 512);
        std::vector<std::uint8_t> payload;
        payload.reserve(12 + text_length);
        payload.push_back(static_cast<std::uint8_t>(state));
        payload.push_back(0);
        append_le(payload, static_cast<std::uint16_t>(text_length));
        append_le(payload, sent_packets);
        payload.insert(payload.end(), text.begin(), text.begin() + static_cast<std::ptrdiff_t>(text_length));
        std::uint8_t flags = active_ ? 0x01 : 0;
        if (state == V4StatusState::ended) flags |= 0x02;
        return packet(V4PacketType::status, game, flags, rate, std::move(payload));
    }

private:
    std::vector<std::uint8_t> packet(V4PacketType type, Game game, std::uint8_t flags, int rate,
                                     std::vector<std::uint8_t> payload,
                                     std::chrono::steady_clock::time_point captured_at =
                                         std::chrono::steady_clock::now()) {
        if (payload.size() > std::numeric_limits<std::uint16_t>::max()) {
            throw std::runtime_error("Protocol-v4 payload exceeds 65535 bytes");
        }
        std::vector<std::uint8_t> result;
        result.reserve(kV4HeaderSize + payload.size() + kV4HmacSize);
        result.insert(result.end(), {'R', 'P', 'D', '4'});
        result.push_back(4);
        result.push_back(static_cast<std::uint8_t>(type));
        result.push_back(wire_simulator(game));
        result.push_back(flags);
        append_le(result, static_cast<std::uint16_t>(kV4HeaderSize));
        append_le(result, static_cast<std::uint16_t>(payload.size()));
        append_le(result, kV4SchemaVersion);
        append_le(result, static_cast<std::uint16_t>(field_count));
        append_le(result, static_cast<std::uint16_t>(rate));
        append_le(result, std::uint16_t{0});
        result.insert(result.end(), run_id_.begin(), run_id_.end());
        append_le(result, active_ ? sequence_++ : control_sequence_++);
        append_le(result, monotonic_us(captured_at));
        if (result.size() != kV4HeaderSize) throw std::runtime_error("Internal protocol-v4 header size mismatch");
        result.insert(result.end(), payload.begin(), payload.end());
        const auto digest = hmac_.sign(result);
        result.insert(result.end(), digest.begin(), digest.end());
        return result;
    }

    HmacSha256 hmac_;
    std::array<std::uint8_t, 16> run_id_{};
    std::uint64_t sequence_ = 0;
    std::uint64_t control_sequence_ = 0;
    std::chrono::steady_clock::time_point daemon_started_;
    std::chrono::steady_clock::time_point run_started_{};
    bool active_ = false;
};

class UdpSender {
public:
    UdpSender(const std::string& host, unsigned short port) {
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return;
        started_ = true;
        socket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socket_ == INVALID_SOCKET) return;
        BOOL broadcast = TRUE;
        setsockopt(socket_, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&broadcast), sizeof(broadcast));
        BOOL reset = FALSE;
        DWORD returned = 0;
        WSAIoctl(socket_, SIO_UDP_CONNRESET, &reset, sizeof(reset), nullptr, 0, &returned, nullptr, nullptr);
        address_.sin_family = AF_INET;
        address_.sin_port = htons(port);
        if (InetPtonA(AF_INET, host.c_str(), &address_.sin_addr) != 1) {
            addrinfo hints{};
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_DGRAM;
            addrinfo* result = nullptr;
            if (getaddrinfo(host.c_str(), nullptr, &hints, &result) == 0 && result) {
                address_.sin_addr = reinterpret_cast<sockaddr_in*>(result->ai_addr)->sin_addr;
                resolved_ = true;
            }
            if (result) freeaddrinfo(result);
        } else {
            resolved_ = true;
        }
    }

    ~UdpSender() {
        if (socket_ != INVALID_SOCKET) closesocket(socket_);
        if (started_) WSACleanup();
    }

    bool send(std::span<const std::uint8_t> payload) {
        if (socket_ == INVALID_SOCKET || !resolved_) {
            last_error_ = WSAHOST_NOT_FOUND;
            return false;
        }
        const int sent = sendto(socket_, reinterpret_cast<const char*>(payload.data()),
                                static_cast<int>(payload.size()), 0,
                                reinterpret_cast<const sockaddr*>(&address_), sizeof(address_));
        if (sent == SOCKET_ERROR) {
            last_error_ = WSAGetLastError();
            return false;
        }
        last_error_ = 0;
        return true;
    }

    bool send(const std::string& payload) {
        return send(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(payload.data()),
                                                  payload.size()));
    }

    int last_error() const { return last_error_; }

private:
    bool started_ = false;
    bool resolved_ = false;
    SOCKET socket_ = INVALID_SOCKET;
    sockaddr_in address_{};
    int last_error_ = 0;
};

void write_zeros(std::ofstream& output, std::size_t count) {
    static constexpr std::array<char, 1024> zeros{};
    while (count > 0) {
        const auto chunk = std::min(count, zeros.size());
        output.write(zeros.data(), static_cast<std::streamsize>(chunk));
        count -= chunk;
    }
}

template <typename T>
void write_value(std::ofstream& output, T value) {
    output.write(reinterpret_cast<const char*>(&value), sizeof(value));
}

void write_fixed(std::ofstream& output, std::string_view value, std::size_t length) {
    const auto used = std::min(value.size(), length);
    output.write(value.data(), static_cast<std::streamsize>(used));
    write_zeros(output, length - used);
}

std::string safe_filename(std::string_view value, std::string_view fallback) {
    std::string result;
    for (const unsigned char c : value) {
        result.push_back(std::isalnum(c) || c == '-' || c == '_' ? static_cast<char>(c) : '_');
    }
    while (!result.empty() && result.front() == '_') result.erase(result.begin());
    while (!result.empty() && result.back() == '_') result.pop_back();
    return result.empty() ? std::string(fallback) : result;
}

class MotecRecorder {
public:
    MotecRecorder(fs::path directory, int sample_rate)
        : directory_(std::move(directory)), sample_rate_(sample_rate) {}

    bool recording() const { return recording_; }
    std::uint64_t sample_count() const { return sample_count_; }
    const fs::path& last_path() const { return last_path_; }

    void start(const Metadata& source) {
        metadata_ = source;
        metadata_.started_at = std::chrono::system_clock::now();
        for (auto& samples : samples_) {
            samples.clear();
            samples.reserve(static_cast<std::size_t>(sample_rate_) * 600);
        }
        sample_count_ = 0;
        recording_ = true;
    }

    void add(const Frame& frame) {
        if (!recording_) return;
        samples_[elapsed].push_back(static_cast<float>(sample_count_) / sample_rate_);
        for (std::size_t i = 1; i < field_count; ++i) {
            double value = frame.value[i] * kChannels[i].scale;
            if (!std::isfinite(value)) value = 0.0;
            samples_[i].push_back(static_cast<float>(value));
        }
        ++sample_count_;
    }

    fs::path finish() {
        if (!recording_) return {};
        recording_ = false;
        if (sample_count_ == 0) return {};
        fs::create_directories(directory_);
        const auto started = std::chrono::system_clock::to_time_t(metadata_.started_at);
        std::tm local{};
        localtime_s(&local, &started);
        char date[32];
        std::strftime(date, sizeof(date), "%Y-%m-%d_%H-%M-%S", &local);
        const auto venue = safe_filename(metadata_.venue, metadata_.simulator);
        std::string stem = std::string(date) + "_" + metadata_.simulator + "_" + venue;
        fs::path destination = directory_ / utf8_to_wide(stem + ".ld");
        int suffix = 1;
        while (fs::exists(destination)) {
            destination = directory_ / utf8_to_wide(stem + "_" + std::to_string(suffix++) + ".ld");
        }
        write(destination);
        last_path_ = destination;
        return destination;
    }

private:
    void write(const fs::path& path) {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output) throw std::runtime_error("Cannot create LD file");
        const std::uint32_t event_pointer = static_cast<std::uint32_t>(kHeaderSize);
        const std::uint32_t metadata_pointer = static_cast<std::uint32_t>(kHeaderSize + kEventSize);
        const std::uint32_t data_pointer = metadata_pointer + static_cast<std::uint32_t>(field_count * kChannelHeaderSize);
        write_value<std::uint32_t>(output, 0x40); write_zeros(output, 4);
        write_value(output, metadata_pointer); write_value(output, data_pointer);
        write_zeros(output, 20); write_value(output, event_pointer); write_zeros(output, 24);
        write_value<std::uint16_t>(output, 1); write_value<std::uint16_t>(output, 0x4240);
        write_value<std::uint16_t>(output, 0x000f); write_value<std::uint32_t>(output, 0x1f44);
        write_fixed(output, "ADL", 8); write_value<std::uint16_t>(output, 420);
        write_value<std::uint16_t>(output, 0xadb0); write_value<std::uint32_t>(output, field_count);
        write_zeros(output, 4);
        const auto started = std::chrono::system_clock::to_time_t(metadata_.started_at);
        std::tm local{};
        localtime_s(&local, &started);
        char date[32], clock[32];
        std::strftime(date, sizeof(date), "%d/%m/%Y", &local);
        std::strftime(clock, sizeof(clock), "%H:%M:%S", &local);
        write_fixed(output, date, 16); write_zeros(output, 16);
        write_fixed(output, clock, 16); write_zeros(output, 16);
        write_fixed(output, metadata_.driver, 64); write_fixed(output, metadata_.vehicle, 64);
        write_zeros(output, 64); write_fixed(output, metadata_.venue, 64);
        write_zeros(output, 64 + 1024); write_value<std::uint32_t>(output, 0x000c81a4);
        write_zeros(output, 66); write_fixed(output, "raPId " + metadata_.simulator + " telemetry", 64);
        write_zeros(output, 126);
        if (static_cast<std::size_t>(output.tellp()) != kHeaderSize) throw std::runtime_error("Invalid LD header size");
        write_fixed(output, metadata_.simulator, 64); write_fixed(output, metadata_.session, 64);
        write_fixed(output, "Recorded by raPId", 1024); write_value<std::uint16_t>(output, 0);
        if (static_cast<std::size_t>(output.tellp()) != kHeaderSize + kEventSize) {
            throw std::runtime_error("Invalid LD event size");
        }
        std::uint32_t next_data = data_pointer;
        for (std::size_t i = 0; i < field_count; ++i) {
            const std::uint32_t previous = i == 0 ? 0 : metadata_pointer + static_cast<std::uint32_t>((i - 1) * kChannelHeaderSize);
            const std::uint32_t next = i + 1 == field_count ? 0 : metadata_pointer + static_cast<std::uint32_t>((i + 1) * kChannelHeaderSize);
            write_value(output, previous); write_value(output, next); write_value(output, next_data);
            write_value<std::uint32_t>(output, static_cast<std::uint32_t>(sample_count_));
            write_value<std::uint16_t>(output, static_cast<std::uint16_t>(0x2ee1 + i));
            write_value<std::uint16_t>(output, 0x07); write_value<std::uint16_t>(output, 4);
            write_value<std::uint16_t>(output, static_cast<std::uint16_t>(sample_rate_));
            write_value<std::int16_t>(output, 0); write_value<std::int16_t>(output, 1);
            write_value<std::int16_t>(output, 1); write_value<std::int16_t>(output, 0);
            write_fixed(output, kChannels[i].name, 32); write_fixed(output, kChannels[i].short_name, 8);
            write_fixed(output, kChannels[i].unit, 12); write_zeros(output, 40);
            next_data += static_cast<std::uint32_t>(sample_count_ * sizeof(float));
        }
        for (const auto& channel : samples_) {
            output.write(reinterpret_cast<const char*>(channel.data()),
                         static_cast<std::streamsize>(channel.size() * sizeof(float)));
        }
        if (!output) throw std::runtime_error("Could not finish LD file");
    }

    fs::path directory_;
    int sample_rate_;
    bool recording_ = false;
    std::uint64_t sample_count_ = 0;
    Metadata metadata_;
    std::array<std::vector<float>, field_count> samples_;
    fs::path last_path_;
};

struct PublicStatus {
    std::string status = "Dormant - waiting for a supported simulator process";
    std::string simulator = "None detected";
    std::string last_error = "None";
    fs::path last_log;
    std::uint64_t samples = 0;
    std::uint64_t packets = 0;
    bool recording = false;
};

class Daemon {
public:
    Daemon(Options options, HWND window)
        : options_(std::move(options)), window_(window),
          logger_(options_.output_directory / L"rapid-daemon-native.log", options_.headless),
          recorder_(options_.output_directory, options_.sample_rate) {}

    ~Daemon() { stop(); }

    void start() {
        logger_.write("Native daemon started; sample rate " + std::to_string(options_.sample_rate) +
                      " Hz; Pi " + options_.pi_host + ":" + std::to_string(options_.pi_port));
        worker_ = std::thread([this] { run(); });
    }

    void stop() {
        if (!worker_.joinable()) return;
        stopping_.store(true, std::memory_order_relaxed);
        wake_.notify_all();
        worker_.join();
    }

    PublicStatus status() const {
        std::lock_guard lock(status_mutex_);
        return public_status_;
    }

private:
    void set_status(const std::string& text) {
        current_status_ = text;
        {
            std::lock_guard lock(status_mutex_);
            public_status_.status = text;
            public_status_.simulator = adapter_ ? adapter_->metadata.simulator : "None detected";
            public_status_.samples = recorder_.sample_count();
            public_status_.packets = packets_;
            public_status_.recording = recorder_.recording();
            public_status_.last_log = recorder_.last_path();
        }
        if (window_) PostMessageW(window_, kStatusMessage, 0, 0);
    }

    void set_error(const std::string& error) {
        std::lock_guard lock(status_mutex_);
        public_status_.last_error = error;
    }

    void network_error() {
        const int code = sender_ ? sender_->last_error() : 0;
        const auto now = std::chrono::steady_clock::now();
        if (code != last_network_error_ || now - last_network_log_ >= 60s) {
            last_network_error_ = code;
            last_network_log_ = now;
            const auto message = "UDP forwarding unavailable (Winsock " + std::to_string(code) + "); local recording continues";
            logger_.write(message);
            set_error(message);
        }
    }

    void send(std::string payload) {
        if (!sender_) return;
        if (sender_->send(payload)) {
            ++packets_;
            if (last_network_error_ != 0) set_error("None");
            last_network_error_ = 0;
        } else {
            network_error();
        }
    }

    void send(const std::vector<std::uint8_t>& payload) {
        send(std::string(reinterpret_cast<const char*>(payload.data()), payload.size()));
    }

    void finish_recording() {
        if (!recorder_.recording()) return;
        if (v4_ && v4_->active()) {
            send(v4_->status_packet(running_game_.game(), V4StatusState::ended,
                                   "Session ended", packets_, options_.sample_rate));
            v4_->end_run();
        }
        try {
            const auto count = recorder_.sample_count();
            const auto path = recorder_.finish();
            if (!path.empty()) logger_.write("Saved " + std::to_string(count) + " samples to " + path.string());
        } catch (const std::exception& error) {
            logger_.write(std::string("Save failed: ") + error.what());
            set_error(error.what());
        }
    }

    void run() {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        const auto sample_period = std::chrono::nanoseconds(1'000'000'000LL / options_.sample_rate);
        auto next_sample = std::chrono::steady_clock::now();
        auto next_heartbeat = next_sample + 1s;
        auto next_probe = next_sample;
        auto next_connection_check = next_sample + 1s;
        auto next_game_scan = next_sample;
        std::optional<std::chrono::steady_clock::time_point> inactive_since;

        while (!stopping_.load(std::memory_order_relaxed)) {
            const auto now = std::chrono::steady_clock::now();

            // Fully dormant unless a supported simulator executable is running.
            // A single process snapshot every five seconds is the only idle work.
            if (!running_game_ && now >= next_game_scan) {
                running_game_ = find_running_game();
                next_game_scan = now + 5s;
                if (running_game_) {
                    if (!options_.no_forward) {
                        sender_ = std::make_unique<UdpSender>(options_.pi_host, options_.pi_port);
                        if (options_.protocol == Protocol::v4)
                            v4_ = std::make_unique<V4Encoder>(options_.auth_key);
                    }
                    logger_.write(std::string(game_name(running_game_.game())) + " process detected; telemetry waking");
                    set_status(std::string(game_name(running_game_.game())) + " started - waiting for telemetry");
                    next_probe = now;
                    next_heartbeat = now;
                }
            }

            if (!running_game_) {
                std::unique_lock lock(wake_mutex_);
                wake_.wait_until(lock, next_game_scan,
                                 [this] { return stopping_.load(std::memory_order_relaxed); });
                continue;
            }

            if (!running_game_.running()) {
                finish_recording();
                adapter_.reset();
                sender_.reset();
                running_game_.reset();
                inactive_since.reset();
                set_status("Dormant - waiting for a supported simulator process");
                next_game_scan = now + 5s;
                continue;
            }

            if (now >= next_heartbeat) {
                const auto state = recorder_.recording() ? "driving" : adapter_ ? "ready" : "waiting";
                if (v4_) {
                    if (v4_->active() && adapter_)
                        send(v4_->metadata_packet(running_game_.game(), adapter_->metadata, options_.sample_rate));
                    send(v4_->status_packet(running_game_.game(),
                         v4_->active() ? V4StatusState::driving : adapter_ ? V4StatusState::ready : V4StatusState::waiting,
                         "", packets_, options_.sample_rate));
                } else send(status_json(state, adapter_.get(), options_.sample_rate));
                next_heartbeat = now + 1s;
                set_status(current_status_);
            }

            if (!adapter_ && now >= next_probe) {
                adapter_ = open_adapter(running_game_.game());
                next_probe = now + 1s;
                if (adapter_) {
                    logger_.write(adapter_->metadata.simulator + " telemetry detected");
                    set_status(adapter_->metadata.simulator + " detected - waiting for driving");
                    next_sample = now;
                } else {
                    set_status("Waiting for ACC, AC, ACE, or iRacing");
                }
            }

            if (adapter_ && now >= next_connection_check) {
                next_connection_check = now + 1s;
                if (!adapter_->connected()) {
                    finish_recording();
                    adapter_.reset();
                    inactive_since.reset();
                    set_status(std::string(game_name(running_game_.game())) + " running - waiting for telemetry");
                    next_probe = now + 1s;
                    continue;
                }
            }

            if (adapter_ && now >= next_sample) {
                next_sample += sample_period;
                if (next_sample < now - sample_period) next_sample = now + sample_period;
                if (adapter_->live()) {
                    inactive_since.reset();
                    if (!recorder_.recording()) {
                        recorder_.start(adapter_->metadata);
                        if (v4_) {
                            v4_->begin_run();
                            send(v4_->metadata_packet(running_game_.game(), adapter_->metadata, options_.sample_rate));
                        }
                        logger_.write("Recording " + adapter_->metadata.simulator + ": " +
                                      adapter_->metadata.vehicle + " at " + adapter_->metadata.venue);
                    }
                    Frame frame;
                    if (adapter_->read(frame)) {
                        recorder_.add(frame);
                        if (v4_) send(v4_->telemetry_packet(running_game_.game(), frame,
                                                          options_.sample_rate, std::chrono::steady_clock::now()));
                        else send(telemetry_json(frame, *adapter_, options_.sample_rate));
                        set_status(adapter_->metadata.simulator + " recording - " +
                                   std::to_string(recorder_.sample_count()) + " samples");
                    }
                } else {
                    set_status(adapter_->metadata.simulator + " connected - waiting for driving");
                    if (recorder_.recording()) {
                        if (!inactive_since) inactive_since = now;
                        else if (now - *inactive_since >= 2s) finish_recording();
                    }
                }
            }

            auto wake_at = next_heartbeat;
            if (!adapter_) wake_at = std::min(wake_at, next_probe);
            else wake_at = std::min({wake_at, next_sample, next_connection_check});
            std::unique_lock lock(wake_mutex_);
            wake_.wait_until(lock, wake_at, [this] { return stopping_.load(std::memory_order_relaxed); });
        }
        finish_recording();
        adapter_.reset();
        sender_.reset();
        running_game_.reset();
    }

    Options options_;
    HWND window_;
    Logger logger_;
    MotecRecorder recorder_;
    std::unique_ptr<UdpSender> sender_;
    std::unique_ptr<V4Encoder> v4_;
    std::unique_ptr<Adapter> adapter_;
    RunningGame running_game_;
    std::thread worker_;
    std::atomic<bool> stopping_{false};
    std::condition_variable wake_;
    std::mutex wake_mutex_;
    mutable std::mutex status_mutex_;
    PublicStatus public_status_;
    std::string current_status_ = "Dormant - waiting for a supported simulator process";
    std::uint64_t packets_ = 0;
    int last_network_error_ = 0;
    std::chrono::steady_clock::time_point last_network_log_{};
};

std::unique_ptr<Daemon> g_daemon;
NOTIFYICONDATAW g_tray{};
fs::path g_output_directory;

std::wstring status_text(const PublicStatus& status) {
    std::wostringstream text;
    text << L"Status: " << utf8_to_wide(status.status)
         << L"\nSimulator: " << utf8_to_wide(status.simulator)
         << L"\nRecording: " << (status.recording ? L"Yes" : L"No")
         << L"\nSamples: " << status.samples
         << L"\nForwarded packets: " << status.packets
         << L"\nLast log: " << (status.last_log.empty() ? L"None yet" : status.last_log.wstring())
         << L"\nLast error: " << utf8_to_wide(status.last_error);
    return text.str();
}

void show_status(HWND window) {
    if (!g_daemon) return;
    const auto status = g_daemon->status();
    MessageBoxW(window, status_text(status).c_str(), L"raPId Telemetry Daemon", MB_OK | MB_ICONINFORMATION);
}

void update_tray() {
    if (!g_daemon) return;
    auto status = utf8_to_wide("raPId - " + g_daemon->status().status);
    if (status.size() >= std::size(g_tray.szTip)) status.resize(std::size(g_tray.szTip) - 1);
    std::wmemset(g_tray.szTip, 0, std::size(g_tray.szTip));
    std::wmemcpy(g_tray.szTip, status.data(), status.size());
    g_tray.uFlags = NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &g_tray);
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case kStatusMessage:
            update_tray();
            return 0;
        case kTrayMessage:
            if (LOWORD(lparam) == WM_LBUTTONUP || LOWORD(lparam) == WM_LBUTTONDBLCLK) {
                show_status(window);
            } else if (LOWORD(lparam) == WM_RBUTTONUP || LOWORD(lparam) == WM_CONTEXTMENU) {
                POINT cursor{};
                GetCursorPos(&cursor);
                HMENU menu = CreatePopupMenu();
                AppendMenuW(menu, MF_STRING, kMenuStatus, L"Show status");
                AppendMenuW(menu, MF_STRING, kMenuFolder, L"Open telemetry folder");
                AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
                AppendMenuW(menu, MF_STRING, kMenuExit, L"Exit");
                SetForegroundWindow(window);
                const UINT selected = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY,
                                                     cursor.x, cursor.y, 0, window, nullptr);
                DestroyMenu(menu);
                if (selected) PostMessageW(window, WM_COMMAND, selected, 0);
            }
            return 0;
        case WM_COMMAND:
            switch (LOWORD(wparam)) {
                case kMenuStatus: show_status(window); break;
                case kMenuFolder:
                    ShellExecuteW(window, L"open", g_output_directory.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                    break;
                case kMenuExit: DestroyWindow(window); break;
            }
            return 0;
        case WM_DESTROY:
            Shell_NotifyIconW(NIM_DELETE, &g_tray);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

HWND create_tray_window(HINSTANCE instance) {
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = instance;
    window_class.lpszClassName = kWindowClass;
    window_class.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    if (!RegisterClassExW(&window_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return nullptr;
    HWND window = CreateWindowExW(0, kWindowClass, L"raPId Telemetry", 0, 0, 0, 0, 0,
                                  HWND_MESSAGE, nullptr, instance, nullptr);
    if (!window) return nullptr;
    g_tray = {};
    g_tray.cbSize = sizeof(g_tray);
    g_tray.hWnd = window;
    g_tray.uID = 1;
    g_tray.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_tray.uCallbackMessage = kTrayMessage;
    g_tray.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    std::wcscpy(g_tray.szTip, L"raPId - starting");
    Shell_NotifyIconW(NIM_ADD, &g_tray);
    g_tray.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &g_tray);
    return window;
}

bool run_self_test(const fs::path& directory, int sample_rate) {
    // Public test-only key; fixtures exercise the actual Windows BCrypt encoder.
    V4Encoder encoder(std::vector<std::uint8_t>(32, 0x11));
    Metadata wire_metadata;
    wire_metadata.venue = "V4 Track"; wire_metadata.vehicle = "V4 Car";
    wire_metadata.driver = "V4 Driver"; wire_metadata.session = "Race";
    Frame wire_frame;
    wire_frame.valid_mask = all_field_bits();
    wire_frame.value[rpm] = 6500; wire_frame.value[gear] = 4;
    wire_frame.value[throttle] = .8; wire_frame.value[brake] = .2;
    wire_frame.value[steering_angle] = -.3;
    wire_frame.value[g_x] = .7; wire_frame.value[g_y] = 1; wire_frame.value[g_z] = -.4;
    wire_frame.value[speed_kmh] = 198; wire_frame.value[lap_number] = 1;
    wire_frame.value[current_lap_ms] = 1234;
    wire_frame.completed_lap_ms = 90000; wire_frame.delta_ms = -125;
    const auto fixtures = directory / "v4-fixtures";
    fs::create_directories(fixtures);
    auto save = [&](const char* name, const std::vector<std::uint8_t>& bytes) {
        std::ofstream out(fixtures / name);
        for (auto byte : bytes) out << std::hex << std::setw(2) << std::setfill('0') << int(byte);
        out << '\n';
        if (!out) throw std::runtime_error("Cannot write v4 test fixture");
    };
    encoder.begin_run();
    save("metadata.hex", encoder.metadata_packet(Game::acc, wire_metadata, sample_rate));
    save("telemetry.hex", encoder.telemetry_packet(Game::acc, wire_frame, sample_rate, std::chrono::steady_clock::now()));
    save("driving.hex", encoder.status_packet(Game::acc, V4StatusState::driving, "", 2, sample_rate));
    save("next.hex", encoder.telemetry_packet(Game::acc, wire_frame, sample_rate, std::chrono::steady_clock::now()));
    save("ended.hex", encoder.status_packet(Game::acc, V4StatusState::ended, "", 4, sample_rate));
    encoder.end_run();
    save("ready.hex", encoder.status_packet(Game::acc, V4StatusState::ready, "", 5, sample_rate));
    Metadata metadata;
    metadata.simulator = "SELFTEST";
    metadata.driver = "Test Driver";
    metadata.vehicle = "Test Car";
    metadata.venue = "Test Track";
    metadata.session = "Test";
    MotecRecorder recorder(directory, sample_rate);
    recorder.start(metadata);
    Frame frame;
    frame.value[rpm] = 6123;
    frame.value[speed_kmh] = 201.5;
    recorder.add(frame);
    recorder.add(frame);
    const auto path = recorder.finish();
    std::ifstream input(path, std::ios::binary);
    input.seekg(0, std::ios::end);
    const auto size = static_cast<std::size_t>(input.tellg());
    input.seekg(0);
    std::vector<unsigned char> bytes(size);
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    const auto u32 = [&bytes](std::size_t at) {
        std::uint32_t value{};
        std::memcpy(&value, bytes.data() + at, sizeof(value));
        return value;
    };
    const auto metadata_pointer = u32(8);
    const auto data_pointer = u32(12);
    const auto count = u32(86);
    const auto expected = data_pointer + field_count * 2 * sizeof(float);
    if (u32(0) != 0x40 || metadata_pointer != kHeaderSize + kEventSize ||
        count != field_count || size != expected || u32(metadata_pointer + 12) != 2) return false;
    std::cout << "raPId native daemon self-test passed: " << path.string() << '\n';
    return true;
}

void apply_low_impact_policy() {
    SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
#if defined(PROCESS_POWER_THROTTLING_EXECUTION_SPEED)
    PROCESS_POWER_THROTTLING_STATE power{};
    power.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    power.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    power.StateMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &power, sizeof(power));
#endif
}

std::atomic<bool> g_console_stop{false};
BOOL WINAPI console_handler(DWORD signal) {
    if (signal == CTRL_C_EVENT || signal == CTRL_BREAK_EVENT || signal == CTRL_CLOSE_EVENT) {
        g_console_stop.store(true, std::memory_order_relaxed);
        return TRUE;
    }
    return FALSE;
}

} // namespace rapid

int wmain(int argc, wchar_t** argv) {
    using namespace rapid;
    try {
        const Options options = parse_options(argc, argv);
        if (options.self_test) return run_self_test(options.output_directory, options.sample_rate) ? 0 : 1;
        if (!options.headless && GetConsoleWindow()) ShowWindow(GetConsoleWindow(), SW_HIDE);
        std::error_code directory_error;
        fs::create_directories(options.output_directory, directory_error);
        if (directory_error) throw std::runtime_error("Cannot create telemetry output directory");
        HANDLE mutex = CreateMutexW(nullptr, TRUE, kMutexName);
        if (!mutex) throw std::runtime_error("Cannot create daemon mutex");
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            // Login startup and manual launches can overlap. The running
            // instance already owns the tray; leave no dialog or extra process.
            CloseHandle(mutex);
            return 0;
        }
        apply_low_impact_policy();
        g_output_directory = options.output_directory;

        if (options.headless) {
            SetConsoleCtrlHandler(console_handler, TRUE);
            g_daemon = std::make_unique<Daemon>(options, nullptr);
            g_daemon->start();
            while (!g_console_stop.load(std::memory_order_relaxed)) Sleep(250);
        } else {
            const HINSTANCE instance = GetModuleHandleW(nullptr);
            HWND window = create_tray_window(instance);
            if (!window) throw std::runtime_error("Cannot create notification-area window");
            g_daemon = std::make_unique<Daemon>(options, window);
            g_daemon->start();
            MSG message{};
            while (GetMessageW(&message, nullptr, 0, 0) > 0) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        g_daemon.reset();
        ReleaseMutex(mutex);
        CloseHandle(mutex);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "raPId native daemon: %s\n", error.what());
        MessageBoxA(nullptr, error.what(), "raPId native daemon", MB_OK | MB_ICONERROR);
        return 1;
    }
}
