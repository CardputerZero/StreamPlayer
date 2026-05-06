#include "media_controller.h"
#include "ui/ui.h"

#include "lvgl/src/libs/tiny_ttf/lv_tiny_ttf.h"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <string>
#include <signal.h>
#include <ctime>
#include <pwd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {

constexpr int kScreenW = 320;
constexpr int kScreenH = 170;
constexpr int kTopY = 22;
constexpr const char *kAppVersion = "0.1.0";
constexpr const char *kDeviceId = "m5cardputerzero";
constexpr const char *kMpvSocket = "/tmp/m5cardputer-streamplayer-mpv.sock";
constexpr const char *kControlPipe = "/tmp/m5streamplayer-control";
constexpr int kItemsPageSize = 24;
constexpr int kLazyLoadThreshold = 5;
constexpr int kTranscodeMaxWidth = 1920;
constexpr int kTranscodeMaxHeight = 1080;
constexpr int kTranscodeMaxFramerate = 30;
constexpr int kTranscodeVideoBitrate = 5000000;
constexpr int kTranscodeAudioBitrate = 128000;
constexpr int kTranscodeMaxBitrate = kTranscodeVideoBitrate + kTranscodeAudioBitrate;
constexpr int kSmallScreenTranscodeMaxWidth = 320;
constexpr int kSmallScreenTranscodeMaxHeight = 170;
constexpr int kSmallScreenTranscodeVideoBitrate = 500000;
constexpr int kSmallScreenTranscodeMaxBitrate = kSmallScreenTranscodeVideoBitrate + kTranscodeAudioBitrate;
constexpr size_t kNoStream = static_cast<size_t>(-1);
constexpr const char *kDefaultServerUrl = "";
constexpr const char *kServerUrlPlaceholder = "server URL";
constexpr const char *kRecentPlayedLibraryId = "__m5_recent_played";
constexpr const char *kRecentPlayedLibraryType = "recentplayed";
constexpr const char *kSettingsLibraryId = "__m5_settings";
constexpr const char *kSettingsLibraryType = "settings";

enum class ServerType { Jellyfin, Emby };
enum class UiLanguage { English, Chinese };
enum class View { Setup, Loading, Browse, Detail, Player, Sort, Settings, Search, Error };
enum class SortMode { LatestUpdated, Name, PremiereDate, Runtime, Unwatched };
enum class AudioOutput { Speaker, Headphone, Bluetooth, Hdmi };

struct ServerConfig {
    ServerType type = ServerType::Jellyfin;
    std::string base_url;
    std::string username;
    std::string password;
    std::string access_token;
    std::string user_id;
    std::string language;
};

struct MediaStream {
    int index = -1;
    std::string type;
    std::string codec;
    std::string title;
    std::string language;
};

struct MediaLibrary {
    std::string id;
    std::string name;
    std::string type;
};

struct MediaItem {
    std::string id;
    std::string name;
    std::string type;
    std::string media_type;
    std::string parent_id;
    std::string overview;
    std::string album;
    std::string album_artist;
    std::string series_name;
    std::string series_id;
    std::string season_id;
    std::string date_created;
    std::string premiere_date;
    std::string container;
    std::string video_codec;
    std::string audio_codec;
    std::string primary_image_tag;
    std::string thumb_image_tag;
    std::string backdrop_image_tag;
    int production_year = 0;
    int index_number = 0;
    int parent_index_number = 0;
    long long runtime_ticks = 0;
    float community_rating = -1.0f;
    float primary_image_aspect_ratio = 0.0f;
    int critic_rating = -1;
    float frame_rate = 0.0f;
    int width = 0;
    int height = 0;
    std::vector<MediaStream> streams;
};

struct LibraryState {
    std::vector<MediaItem> items;
    size_t selected = 0;
    int loaded = 0;
    bool exhausted = false;
    std::string error;
    bool expanded = false;
    std::string expanded_series_id;
    std::string expanded_series_name;
    std::vector<MediaItem> seasons;
    std::vector<std::vector<MediaItem>> season_episodes;
    std::vector<bool> season_loaded;
    size_t season_selected = 0;
    size_t episode_selected = 0;
    std::string expand_error;
};

struct PlaybackChoice {
    std::string url;
    std::string media_source_id;
    std::string play_session_id;
    bool transcoding = false;
    bool direct_playable = false;
    std::string summary;
    std::vector<MediaStream> streams;
};

struct HttpResponse {
    bool ok = false;
    int exit_code = -1;
    std::string body;
    std::string error;
};

static std::string trim(const std::string &s)
{
    size_t b = 0;
    while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) b++;
    size_t e = s.size();
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) e--;
    return s.substr(b, e - b);
}

static std::string to_lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

static UiLanguage language_from_code(std::string value)
{
    value = to_lower(trim(value));
    if (value.find("zh") == 0 || value.find("chinese") == 0 || value == "cn") {
        return UiLanguage::Chinese;
    }
    return UiLanguage::English;
}

static const char *language_code(UiLanguage language)
{
    return language == UiLanguage::Chinese ? "zh-CN" : "en";
}

static std::string system_language_code()
{
    const char *keys[] = {"LC_ALL", "LC_MESSAGES", "LANG"};
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
        const char *value = getenv(keys[i]);
        if (value && *value) return value;
    }
    return "en";
}

static bool contains_ci(const std::string &haystack, const std::string &needle)
{
    return to_lower(haystack).find(to_lower(needle)) != std::string::npos;
}

static std::string ellipsize(const std::string &s, size_t max_len)
{
    if (s.size() <= max_len) return s;
    if (max_len <= 3) return s.substr(0, max_len);
    return s.substr(0, max_len - 3) + "...";
}

static int utf8_visual_width(const std::string &s, int font_px)
{
    int width = 0;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            width += (std::isalnum(c) || std::ispunct(c)) ? font_px * 6 / 10 : font_px / 2;
            i++;
        } else {
            width += font_px;
            if ((c & 0xE0) == 0xC0) i += 2;
            else if ((c & 0xF0) == 0xE0) i += 3;
            else if ((c & 0xF8) == 0xF0) i += 4;
            else i++;
        }
    }
    return width;
}

static bool should_marquee(const std::string &s, int width_px, int font_px, int lines = 1)
{
    return utf8_visual_width(s, font_px) > width_px * std::max(1, lines);
}

static std::string single_line_text(const char *text)
{
    std::string out;
    bool pending_space = false;
    for (const char *p = text ? text : ""; *p; ++p) {
        unsigned char ch = static_cast<unsigned char>(*p);
        if (ch == '\r' || ch == '\n' || ch == '\t') {
            pending_space = !out.empty();
            continue;
        }
        if (ch < 0x20) continue;
        if (pending_space) {
            out.push_back(' ');
            pending_space = false;
        }
        out.push_back(*p);
    }
    return out;
}

static void append_utf8_codepoint(std::string &text, uint32_t cp)
{
    if (cp == 0) return;
    if (cp < 0x80) {
        text.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        text.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        text.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        text.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        text.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        text.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x110000) {
        text.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        text.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        text.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        text.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

static void erase_last_utf8_char(std::string &text)
{
    if (text.empty()) return;
    size_t pos = text.size() - 1;
    while (pos > 0 && (static_cast<unsigned char>(text[pos]) & 0xC0) == 0x80) pos--;
    text.erase(pos);
}

static std::string shell_quote(const std::string &s)
{
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
}

static std::string json_escape(const std::string &s)
{
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default: out += c; break;
        }
    }
    return out;
}

static std::string url_encode(const std::string &s)
{
    static const char *hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 15]);
        }
    }
    return out;
}

static std::string join_url(std::string base, const std::string &path)
{
    while (!base.empty() && base.back() == '/') base.pop_back();
    if (path.empty()) return base;
    if (path[0] == '/') return base + path;
    return base + "/" + path;
}

static std::string append_query(std::string url, const std::string &key, const std::string &value)
{
    if (value.empty()) return url;
    url += (url.find('?') == std::string::npos) ? '?' : '&';
    url += key + "=" + url_encode(value);
    return url;
}

static std::string set_query_param(std::string url, const std::string &key, const std::string &value)
{
    if (value.empty() || url.empty()) return url;
    const std::string encoded = key + "=" + url_encode(value);
    size_t query = url.find('?');
    if (query == std::string::npos) return append_query(url, key, value);

    size_t pos = query + 1;
    while (pos <= url.size()) {
        size_t next = url.find('&', pos);
        size_t end = next == std::string::npos ? url.size() : next;
        size_t eq = url.find('=', pos);
        if (eq != std::string::npos && eq < end && url.compare(pos, eq - pos, key) == 0) {
            url.replace(pos, end - pos, encoded);
            return url;
        }
        if (next == std::string::npos) break;
        pos = next + 1;
    }
    return append_query(url, key, value);
}

static std::string ensure_api_key(std::string url, const std::string &token)
{
    if (token.empty() || url.find("api_key=") != std::string::npos ||
        url.find("api_key%3D") != std::string::npos) {
        return url;
    }
    return append_query(url, "api_key", token);
}

static std::string redact_query_secret(std::string text)
{
    const char *keys[] = {"api_key=", "X-Emby-Token=", "AccessToken="};
    for (const char *key : keys) {
        size_t pos = 0;
        while ((pos = text.find(key, pos)) != std::string::npos) {
            size_t value = pos + strlen(key);
            size_t end = text.find_first_of("& \r\n\t", value);
            if (end == std::string::npos) end = text.size();
            text.replace(value, end - value, "<redacted>");
            pos = value + 10;
        }
    }
    return text;
}

static void app_log(const std::string &message)
{
    FILE *file = fopen("/tmp/m5streamplayer-app.log", "a");
    if (!file) return;
    time_t now = time(nullptr);
    struct tm tm_now {};
    localtime_r(&now, &tm_now);
    char stamp[32];
    strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tm_now);
    fprintf(file, "[%s] %s\n", stamp, redact_query_secret(message).c_str());
    fclose(file);
}

static std::string bool_text(bool value)
{
    return value ? "1" : "0";
}

static bool command_exists(const char *name);

static std::string first_command_line(const std::string &cmd)
{
    FILE *pipe = popen(cmd.c_str(), "r");
    if (!pipe) return "";
    char buf[512];
    std::string line;
    if (fgets(buf, sizeof(buf), pipe)) line = trim(buf);
    pclose(pipe);
    return line;
}

static std::string bluetooth_pulse_sink()
{
    const char *override = getenv("M5_AUDIO_BLUETOOTH_SINK");
    if (override && *override) return override;
    if (!command_exists("pw-dump")) return "";
    return first_command_line(
        "pw-dump 2>/dev/null | awk -F'\"' '/\"node.name\": \"bluez_output\\./ { print $4; exit }'");
}

static const char *audio_output_label(AudioOutput output)
{
    switch (output) {
    case AudioOutput::Speaker: return "本地";
    case AudioOutput::Headphone: return "3.5mm";
    case AudioOutput::Bluetooth: return "蓝牙";
    case AudioOutput::Hdmi:
    default: return "HDMI OUT";
    }
}

static std::string audio_output_pulse_sink(AudioOutput output)
{
    const char *override = nullptr;
    if (output == AudioOutput::Speaker) override = getenv("M5_AUDIO_SPEAKER_SINK");
    else if (output == AudioOutput::Headphone) override = getenv("M5_AUDIO_HEADPHONE_SINK");
    else if (output == AudioOutput::Bluetooth) return bluetooth_pulse_sink();
    else override = getenv("M5_AUDIO_HDMI_SINK");
    if (override && *override) return override;
    return output == AudioOutput::Hdmi
        ? "alsa_output.platform-3f902000.hdmi.hdmi-stereo"
        : "alsa_output.platform-sound.stereo-fallback";
}

static std::string local_alsa_output_device()
{
    const char *override = getenv("M5_SMALL_SCREEN_ALSA_DEVICE");
    if (!override || !*override) override = getenv("M5_AUDIO_ALSA_DEVICE");
    if (override && *override) return override;
    return "default:CARD=ES8388Audio";
}

static const char *audio_output_wpctl_pattern(AudioOutput output)
{
    switch (output) {
    case AudioOutput::Hdmi: return "Built-in Audio Digital Stereo (HDMI)";
    case AudioOutput::Bluetooth: return "";
    case AudioOutput::Speaker:
    case AudioOutput::Headphone:
    default: return "Built-in Audio Stereo";
    }
}

static bool audio_output_available(AudioOutput output)
{
    if (output == AudioOutput::Bluetooth) return !audio_output_pulse_sink(output).empty();
    return true;
}

static void ensure_analog_audio_volume(AudioOutput output)
{
    if (output != AudioOutput::Speaker && output != AudioOutput::Headphone) return;
    if (!command_exists("amixer")) return;
    const char *volume = getenv("M5_AUDIO_ANALOG_VOLUME");
    if (!volume || !*volume) volume = "60%";
    std::string script =
        "amixer -q -c 1 sset Headphone " + shell_quote(volume) + " >/dev/null 2>&1 || true; "
        "amixer -q -c 1 sset DACL 75% >/dev/null 2>&1 || true; "
        "amixer -q -c 1 sset DACR 75% >/dev/null 2>&1 || true";
    int rc = system(("sh -c " + shell_quote(script)).c_str());
    app_log(std::string("Audio analog mixer target=") + audio_output_label(output) +
            " volume=" + volume +
            " rc=" + std::to_string(rc));
}

static AudioOutput next_audio_output(AudioOutput output)
{
    const AudioOutput order[] = {
        AudioOutput::Speaker,
        AudioOutput::Hdmi,
        AudioOutput::Bluetooth,
    };
    constexpr size_t count = sizeof(order) / sizeof(order[0]);
    size_t current = 0;
    for (size_t i = 0; i < count; ++i) {
        if (order[i] == output) {
            current = i;
            break;
        }
    }
    for (size_t step = 1; step <= count; ++step) {
        AudioOutput candidate = order[(current + step) % count];
        if (audio_output_available(candidate)) return candidate;
    }
    return output;
}

static void apply_pipewire_default_output(AudioOutput output)
{
    ensure_analog_audio_volume(output);
    if (!command_exists("wpctl")) return;
    if (!getenv("XDG_RUNTIME_DIR") && access("/run/user/1000", F_OK) == 0) {
        setenv("XDG_RUNTIME_DIR", "/run/user/1000", 1);
    }
    std::string sink = audio_output_pulse_sink(output);
    if (sink.empty()) {
        app_log(std::string("Audio output unavailable target=") + audio_output_label(output));
        return;
    }
    const char *pattern = audio_output_wpctl_pattern(output);
    if (!pattern || !pattern[0]) {
        app_log(std::string("Audio output using explicit sink target=") + audio_output_label(output) +
                " sink=" + sink);
        return;
    }
    std::string script =
        "pattern=" + shell_quote(pattern) + "; "
        "id=$(wpctl status | awk -v pat=\"$pattern\" 'index($0, pat) {"
        "for (i = 1; i <= NF; i++) if ($i ~ /^[0-9]+\\.$/) { gsub(/\\./, \"\", $i); print $i; exit }"
        "}'); "
        "[ -n \"$id\" ] && wpctl set-default \"$id\"";
    int rc = system(("sh -c " + shell_quote(script)).c_str());
    app_log(std::string("Audio output default target=") + audio_output_label(output) +
            " sink=" + sink +
            " rc=" + std::to_string(rc));
}

static std::string read_stream(FILE *pipe)
{
    char buf[512];
    std::string out;
    while (fgets(buf, sizeof(buf), pipe)) out += buf;
    return out;
}

static std::string config_dir()
{
    const char *override = getenv("M5_STREAMPLAYER_CONFIG_DIR");
    if (override && *override) return override;

    struct stat pi_home {};
    if (stat("/home/pi", &pi_home) == 0 && S_ISDIR(pi_home.st_mode)) {
        return "/home/pi/.config/m5streamplayer";
    }

    const char *home = getenv("HOME");
    std::string root = home ? home : "/tmp";
    return root + "/.config/m5streamplayer";
}

static std::string config_path()
{
    return config_dir() + "/config.ini";
}

static void ensure_config_dir()
{
    std::string dir = config_dir();
    size_t slash = dir.find_last_of('/');
    if (slash != std::string::npos) mkdir(dir.substr(0, slash).c_str(), 0755);
    mkdir(dir.c_str(), 0755);

    if (dir.find("/home/pi/") == 0) {
        struct passwd *pi = getpwnam("pi");
        if (pi) {
            if (slash != std::string::npos) chown(dir.substr(0, slash).c_str(), pi->pw_uid, pi->pw_gid);
            chown(dir.c_str(), pi->pw_uid, pi->pw_gid);
        }
    }
}

static void ensure_pi_owner(const std::string &path)
{
    if (path.find("/home/pi/") != 0) return;
    struct passwd *pi = getpwnam("pi");
    if (pi) chown(path.c_str(), pi->pw_uid, pi->pw_gid);
}

static std::string ini_get(const std::string &content, const std::string &key)
{
    std::istringstream in(content);
    std::string line;
    while (std::getline(in, line)) {
        size_t pos = line.find('=');
        if (pos == std::string::npos) continue;
        if (trim(line.substr(0, pos)) == key) return trim(line.substr(pos + 1));
    }
    return "";
}

static bool load_config(ServerConfig &cfg)
{
    std::ifstream in(config_path().c_str());
    if (!in) return false;
    std::stringstream buffer;
    buffer << in.rdbuf();
    std::string body = buffer.str();
    cfg.type = (ini_get(body, "server_type") == "emby") ? ServerType::Emby : ServerType::Jellyfin;
    cfg.base_url = ini_get(body, "base_url");
    cfg.username = ini_get(body, "username");
    cfg.password = ini_get(body, "password");
    cfg.access_token = ini_get(body, "access_token");
    cfg.user_id = ini_get(body, "user_id");
    cfg.language = ini_get(body, "language");
    return !cfg.base_url.empty() && !cfg.access_token.empty() && !cfg.user_id.empty();
}

static void save_config(const ServerConfig &cfg)
{
    ensure_config_dir();
    std::ofstream out(config_path().c_str(), std::ios::trunc);
    out << "server_type=" << (cfg.type == ServerType::Emby ? "emby" : "jellyfin") << "\n";
    out << "base_url=" << cfg.base_url << "\n";
    out << "username=" << cfg.username << "\n";
    out << "access_token=" << cfg.access_token << "\n";
    out << "user_id=" << cfg.user_id << "\n";
    out << "language=" << (cfg.language.empty() ? language_code(language_from_code(system_language_code()))
                                                : cfg.language) << "\n";
    out.close();
    ensure_pi_owner(config_path());
}

static void clear_config(const ServerConfig *remember = nullptr)
{
    if (remember == nullptr) {
        unlink(config_path().c_str());
        return;
    }

    ServerConfig saved = *remember;
    saved.password.clear();
    saved.access_token.clear();
    saved.user_id.clear();
    save_config(saved);
}

static bool json_skip_ws(const std::string &s, size_t &i)
{
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) i++;
    return i < s.size();
}

static size_t json_find_key(const std::string &s, const std::string &key, size_t start = 0)
{
    std::string pat = "\"" + key + "\"";
    size_t pos = start;
    while ((pos = s.find(pat, pos)) != std::string::npos) {
        size_t c = pos + pat.size();
        json_skip_ws(s, c);
        if (c < s.size() && s[c] == ':') return c + 1;
        pos += pat.size();
    }
    return std::string::npos;
}

static std::string json_string_at(const std::string &s, size_t i)
{
    if (!json_skip_ws(s, i) || i >= s.size() || s[i] != '"') return "";
    i++;
    std::string out;
    while (i < s.size()) {
        char c = s[i++];
        if (c == '"') break;
        if (c == '\\' && i < s.size()) {
            char e = s[i++];
            switch (e) {
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            default: out += e; break;
            }
        } else {
            out += c;
        }
    }
    return out;
}

static std::string json_get_string(const std::string &s, const std::string &key)
{
    size_t pos = json_find_key(s, key);
    if (pos == std::string::npos) return "";
    return json_string_at(s, pos);
}

static std::string json_get_object(const std::string &s, const std::string &key)
{
    size_t pos = json_find_key(s, key);
    if (pos == std::string::npos) return "";
    if (!json_skip_ws(s, pos) || s[pos] != '{') return "";
    bool in_string = false;
    bool escape = false;
    int depth = 0;
    size_t start = pos;
    for (size_t i = pos; i < s.size(); ++i) {
        char c = s[i];
        if (in_string) {
            if (escape) escape = false;
            else if (c == '\\') escape = true;
            else if (c == '"') in_string = false;
            continue;
        }
        if (c == '"') {
            in_string = true;
        } else if (c == '{') {
            depth++;
        } else if (c == '}') {
            depth--;
            if (depth == 0) return s.substr(start, i - start + 1);
        }
    }
    return "";
}

static std::string json_get_first_array_string(const std::string &s, const std::string &key)
{
    size_t pos = json_find_key(s, key);
    if (pos == std::string::npos) return "";
    if (!json_skip_ws(s, pos) || s[pos] != '[') return "";
    pos++;
    return json_string_at(s, pos);
}

static long long json_get_int(const std::string &s, const std::string &key, long long fallback = 0)
{
    size_t pos = json_find_key(s, key);
    if (pos == std::string::npos) return fallback;
    json_skip_ws(s, pos);
    char *end = nullptr;
    long long v = strtoll(s.c_str() + pos, &end, 10);
    return end == s.c_str() + pos ? fallback : v;
}

static double json_get_number(const std::string &s, const std::string &key, double fallback = 0.0)
{
    size_t pos = json_find_key(s, key);
    if (pos == std::string::npos) return fallback;
    json_skip_ws(s, pos);
    char *end = nullptr;
    double v = strtod(s.c_str() + pos, &end);
    return end == s.c_str() + pos ? fallback : v;
}

static bool json_get_bool(const std::string &s, const std::string &key, bool fallback = false)
{
    size_t pos = json_find_key(s, key);
    if (pos == std::string::npos) return fallback;
    json_skip_ws(s, pos);
    if (s.compare(pos, 4, "true") == 0) return true;
    if (s.compare(pos, 5, "false") == 0) return false;
    return fallback;
}

static std::vector<std::string> json_objects_in_array(const std::string &s, const std::string &key)
{
    std::vector<std::string> out;
    size_t pos = json_find_key(s, key);
    if (pos == std::string::npos) return out;
    if (!json_skip_ws(s, pos) || s[pos] != '[') return out;
    bool in_string = false;
    bool escape = false;
    int depth = 0;
    size_t obj_start = std::string::npos;
    for (size_t i = pos + 1; i < s.size(); ++i) {
        char c = s[i];
        if (in_string) {
            if (escape) escape = false;
            else if (c == '\\') escape = true;
            else if (c == '"') in_string = false;
            continue;
        }
        if (c == '"') {
            in_string = true;
        } else if (c == '{') {
            if (depth == 0) obj_start = i;
            depth++;
        } else if (c == '}') {
            depth--;
            if (depth == 0 && obj_start != std::string::npos) {
                out.push_back(s.substr(obj_start, i - obj_start + 1));
                obj_start = std::string::npos;
            }
        } else if (c == ']' && depth == 0) {
            break;
        }
    }
    return out;
}

static std::vector<MediaStream> parse_streams(const std::string &object)
{
    std::vector<MediaStream> streams;
    for (const auto &stream_object : json_objects_in_array(object, "MediaStreams")) {
        MediaStream stream;
        stream.index = static_cast<int>(json_get_int(stream_object, "Index", -1));
        stream.type = json_get_string(stream_object, "Type");
        stream.codec = json_get_string(stream_object, "Codec");
        stream.title = json_get_string(stream_object, "DisplayTitle");
        if (stream.title.empty()) stream.title = json_get_string(stream_object, "Title");
        stream.language = json_get_string(stream_object, "Language");
        streams.push_back(stream);
    }
    return streams;
}

static MediaItem parse_item(const std::string &object)
{
    MediaItem item;
    item.id = json_get_string(object, "Id");
    item.name = json_get_string(object, "Name");
    item.type = json_get_string(object, "Type");
    item.media_type = json_get_string(object, "MediaType");
    item.parent_id = json_get_string(object, "ParentId");
    item.overview = json_get_string(object, "Overview");
    item.album = json_get_string(object, "Album");
    item.album_artist = json_get_string(object, "AlbumArtist");
    item.series_name = json_get_string(object, "SeriesName");
    item.series_id = json_get_string(object, "SeriesId");
    item.season_id = json_get_string(object, "SeasonId");
    item.date_created = json_get_string(object, "DateCreated");
    item.premiere_date = json_get_string(object, "PremiereDate");
    item.container = json_get_string(object, "Container");
    item.video_codec = json_get_string(object, "VideoCodec");
    item.audio_codec = json_get_string(object, "AudioCodec");
    std::string image_tags = json_get_object(object, "ImageTags");
    item.primary_image_tag = json_get_string(image_tags, "Primary");
    item.thumb_image_tag = json_get_string(image_tags, "Thumb");
    item.backdrop_image_tag = json_get_string(image_tags, "Backdrop");
    if (item.primary_image_tag.empty()) item.primary_image_tag = json_get_string(object, "PrimaryImageTag");
    if (item.thumb_image_tag.empty()) item.thumb_image_tag = json_get_string(object, "ThumbImageTag");
    if (item.thumb_image_tag.empty()) item.thumb_image_tag = json_get_string(object, "ParentThumbImageTag");
    if (item.backdrop_image_tag.empty()) item.backdrop_image_tag = json_get_string(object, "BackdropImageTag");
    if (item.backdrop_image_tag.empty()) item.backdrop_image_tag = json_get_first_array_string(object, "BackdropImageTags");
    if (item.backdrop_image_tag.empty()) item.backdrop_image_tag = json_get_first_array_string(object, "ParentBackdropImageTags");
    item.production_year = static_cast<int>(json_get_int(object, "ProductionYear", 0));
    item.index_number = static_cast<int>(json_get_int(object, "IndexNumber", 0));
    item.parent_index_number = static_cast<int>(json_get_int(object, "ParentIndexNumber", 0));
    item.runtime_ticks = json_get_int(object, "RunTimeTicks", 0);
    item.community_rating = static_cast<float>(json_get_number(object, "CommunityRating", -1.0));
    item.primary_image_aspect_ratio = static_cast<float>(json_get_number(object, "PrimaryImageAspectRatio", 0.0));
    item.critic_rating = static_cast<int>(json_get_int(object, "CriticRating", -1));
    item.frame_rate = static_cast<float>(json_get_number(object, "AverageFrameRate", 0.0));
    item.width = static_cast<int>(json_get_int(object, "Width", 0));
    item.height = static_cast<int>(json_get_int(object, "Height", 0));
    if (item.width <= 0 || item.height <= 0) {
        for (const auto &source : json_objects_in_array(object, "MediaSources")) {
            int w = static_cast<int>(json_get_int(source, "Width", 0));
            int h = static_cast<int>(json_get_int(source, "Height", 0));
            if (w > 0 && h > 0) {
                item.width = w;
                item.height = h;
                break;
            }
        }
    }
    item.streams = parse_streams(object);
    return item;
}

static MediaLibrary parse_library(const std::string &object)
{
    MediaLibrary lib;
    lib.id = json_get_string(object, "Id");
    lib.name = json_get_string(object, "Name");
    lib.type = json_get_string(object, "CollectionType");
    if (lib.type.empty()) lib.type = json_get_string(object, "Type");
    return lib;
}

static std::string auth_header()
{
    return "MediaBrowser Client=\"M5CardputerZero\", Device=\"M5CardputerZero\", DeviceId=\"" +
           std::string(kDeviceId) + "\", Version=\"" + kAppVersion + "\"";
}

static HttpResponse curl_request(const std::string &method,
                                 const std::string &url,
                                 const std::string &token,
                                 const std::string &body = "")
{
    HttpResponse response;
    std::string body_file;
    if (!body.empty()) {
        body_file = "/tmp/m5streamplayer-body-" + std::to_string(getpid()) + ".json";
        std::ofstream out(body_file.c_str(), std::ios::trunc);
        out << body;
    }

    std::string cmd = "curl -fsSL --connect-timeout 8 --max-time 35";
    cmd += " -X " + shell_quote(method);
    cmd += " -H " + shell_quote("Accept: application/json");
    cmd += " -H " + shell_quote("Content-Type: application/json");
    cmd += " -H " + shell_quote("X-Emby-Authorization: " + auth_header());
    if (!token.empty()) cmd += " -H " + shell_quote("X-Emby-Token: " + token);
    if (!body_file.empty()) cmd += " --data-binary @" + shell_quote(body_file);
    cmd += " " + shell_quote(url) + " 2>&1";

    FILE *pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        response.error = std::string("curl launch failed: ") + strerror(errno);
        if (!body_file.empty()) unlink(body_file.c_str());
        return response;
    }
    response.body = read_stream(pipe);
    int status = pclose(pipe);
    if (!body_file.empty()) unlink(body_file.c_str());
    response.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : status;
    response.ok = (response.exit_code == 0);
    if (!response.ok) {
        response.error = response.body.empty() ? "HTTP request failed" : response.body;
        app_log("HTTP failed method=" + method + " exit=" + std::to_string(response.exit_code) +
                " url=" + url + " error=" + ellipsize(trim(response.error), 140));
    }
    return response;
}

static const char *server_name(ServerType type)
{
    return type == ServerType::Emby ? "Emby" : "Jellyfin";
}

static lv_color_t accent_color(ServerType type)
{
    return lv_color_hex(type == ServerType::Emby ? 0x52B54B : 0x00A4DC);
}

enum class TextKey {
    Server,
    Language,
    Account,
    Password,
    Login,
    NeedCredentials,
    Authenticating,
    LoadingLibraries,
    LoadingVideos,
    PreloadingVideos,
    NoLibraries,
    NoContent,
    Error,
    EnterEscBack,
    HdmiTitle,
    HdmiMessage,
    SmallScreenAlso,
    SmallScreenButton,
    EnterConfirmEscBack,
    SortTitle,
    SortHelp,
    SettingsTitle,
    SettingsService,
    Logout,
    LogoutHelp,
    PreparingMusic,
    PreparingPhoto,
    PreparingSmallScreen,
    RequestTranscode,
    NoPlayable,
    NoOverview,
    DetailHelp,
    Output,
    Audio,
    Subtitle
};

static const char *ui_text(UiLanguage language, TextKey key)
{
    const bool zh = language == UiLanguage::Chinese;
    switch (key) {
    case TextKey::Server: return zh ? "服务" : "Server";
    case TextKey::Language: return zh ? "语言" : "Lang";
    case TextKey::Account: return zh ? "账号" : "User";
    case TextKey::Password: return zh ? "密码" : "Pass";
    case TextKey::Login: return zh ? "登录" : "Login";
    case TextKey::NeedCredentials: return zh ? "需要地址、账号和密码" : "URL, user and password required";
    case TextKey::Authenticating: return zh ? "正在认证..." : "Authenticating...";
    case TextKey::LoadingLibraries: return zh ? "加载媒体库..." : "Loading libraries...";
    case TextKey::LoadingVideos: return zh ? "加载内容..." : "Loading media...";
    case TextKey::PreloadingVideos: return zh ? "预加载内容..." : "Preloading media...";
    case TextKey::NoLibraries: return zh ? "没有可用目录。" : "No libraries available.";
    case TextKey::NoContent: return zh ? "没有内容" : "No content";
    case TextKey::Error: return zh ? "错误" : "Error";
    case TextKey::EnterEscBack: return zh ? "回车/Esc 返回" : "Enter/Esc Back";
    case TextKey::HdmiTitle: return zh ? "HDMI 未连接" : "HDMI disconnected";
    case TextKey::HdmiMessage:
        return zh ? "未连接 HDMI 输出；视频建议接 HDMI，音乐播放不受影响。"
                  : "HDMI output is not connected; video works best on HDMI. Music is unaffected.";
    case TextKey::SmallScreenAlso: return zh ? "也可以临时在小屏幕播放。" : "Small-screen playback is also available.";
    case TextKey::SmallScreenButton: return zh ? "我还是想看看" : "Play on small screen";
    case TextKey::EnterConfirmEscBack: return zh ? "Enter 确认  Esc 返回" : "Enter Confirm  Esc Back";
    case TextKey::SortTitle: return zh ? "视频排序" : "Sort";
    case TextKey::SortHelp: return zh ? "上下选择  回车应用  Esc返回" : "Up/Down Select  Enter Apply  Esc Back";
    case TextKey::SettingsTitle: return zh ? "设置" : "Settings";
    case TextKey::SettingsService: return zh ? "服务: " : "Server: ";
    case TextKey::Logout: return zh ? "退出登录" : "Log out";
    case TextKey::LogoutHelp: return zh ? "回车退出登录  Esc返回" : "Enter Log out  Esc Back";
    case TextKey::PreparingMusic: return zh ? "准备音乐..." : "Preparing music...";
    case TextKey::PreparingPhoto: return zh ? "准备图片..." : "Preparing photo...";
    case TextKey::PreparingSmallScreen: return zh ? "准备小屏播放..." : "Preparing small-screen playback...";
    case TextKey::RequestTranscode: return zh ? "请求服务器 1080p30 转码..." : "Requesting 1080p30 transcode...";
    case TextKey::NoPlayable: return zh ? "这个媒体没有可播放地址。" : "This item has no playable URL.";
    case TextKey::NoOverview: return zh ? "服务器没有提供简介。" : "No overview from server.";
    case TextKey::DetailHelp:
        return zh ? "上下切换  回车播放  空格返回  Esc列表" : "Up/Down Switch  Enter Play  Space Back  Esc List";
    case TextKey::Output: return zh ? "输出" : "Output";
    case TextKey::Audio: return zh ? "音频" : "Audio";
    case TextKey::Subtitle: return zh ? "字幕" : "Sub";
    }
    return "";
}

static const char *sort_label(SortMode mode, UiLanguage language)
{
    const bool zh = language == UiLanguage::Chinese;
    switch (mode) {
    case SortMode::LatestUpdated: return zh ? "最新" : "Latest";
    case SortMode::Name: return zh ? "名称" : "Name";
    case SortMode::PremiereDate: return zh ? "首映" : "Premiere";
    case SortMode::Runtime: return zh ? "时长" : "Runtime";
    case SortMode::Unwatched: return zh ? "未看" : "Unwatched";
    }
    return zh ? "最新" : "Latest";
}

static std::string sort_query(SortMode mode)
{
    switch (mode) {
    case SortMode::Name: return "SortBy=SortName&SortOrder=Ascending";
    case SortMode::PremiereDate: return "SortBy=PremiereDate&SortOrder=Descending";
    case SortMode::Runtime: return "SortBy=Runtime&SortOrder=Descending";
    case SortMode::Unwatched: return "SortBy=DateCreated&SortOrder=Descending&Filters=IsUnplayed";
    case SortMode::LatestUpdated:
    default: return "SortBy=DateCreated&SortOrder=Descending";
    }
}

static std::string recent_played_query()
{
    return "SortBy=DatePlayed&SortOrder=Descending&Filters=IsPlayed";
}

static bool is_series_item(const MediaItem &item)
{
    return item.type == "Series";
}

static bool is_episode_item(const MediaItem &item)
{
    return item.type == "Episode";
}

static bool is_audio_item(const MediaItem &item)
{
    return item.type == "Audio" || item.media_type == "Audio";
}

static bool is_photo_item(const MediaItem &item)
{
    return item.type == "Photo" || item.media_type == "Photo";
}

static bool library_prefers_series(const MediaLibrary &library)
{
    std::string type = to_lower(library.type);
    return type == "tvshows" || type == "series";
}

static bool library_is_recent_played(const MediaLibrary &library)
{
    return library.id == kRecentPlayedLibraryId || to_lower(library.type) == kRecentPlayedLibraryType;
}

static bool library_is_settings(const MediaLibrary &library)
{
    return library.id == kSettingsLibraryId || to_lower(library.type) == kSettingsLibraryType;
}

static bool library_prefers_music(const MediaLibrary &library)
{
    std::string type = to_lower(library.type);
    return type == "music" || type == "audiobooks" || type == "audio";
}

static bool library_prefers_photos(const MediaLibrary &library)
{
    std::string type = to_lower(library.type);
    return type == "photos" || type == "homevideosphotos";
}

static const char *library_media_label(const MediaLibrary &library)
{
    if (library_is_settings(library)) return "设置";
    if (library_is_recent_played(library)) return "最近";
    if (library_prefers_music(library)) return "音乐";
    if (library_prefers_photos(library)) return "图片";
    if (library_prefers_series(library)) return "剧集";
    return "视频";
}

static void art_ratio_for_item(const MediaItem &item, int &ratio_w, int &ratio_h)
{
    ratio_w = 16;
    ratio_h = 9;
    if (!is_episode_item(item)) return;

    if (item.width > 0 && item.height > 0) {
        long long scaled = static_cast<long long>(item.width) * 1000LL / item.height;
        if (scaled >= 650 && scaled <= 2600) {
            ratio_w = item.width;
            ratio_h = item.height;
        }
        return;
    }

    if (item.primary_image_aspect_ratio >= 0.65f && item.primary_image_aspect_ratio <= 2.6f) {
        ratio_w = std::max(1, static_cast<int>(item.primary_image_aspect_ratio * 1000.0f));
        ratio_h = 1000;
    }
}

static bool item_prefers_still_art(const MediaItem &item)
{
    return is_episode_item(item);
}

static bool item_primary_is_landscape(const MediaItem &item)
{
    if (item.primary_image_aspect_ratio > 0.0f) return item.primary_image_aspect_ratio >= 1.2f;
    if (item.width > 0 && item.height > 0) {
        return static_cast<long long>(item.width) * 10LL >= static_cast<long long>(item.height) * 12LL;
    }
    return item_prefers_still_art(item);
}

static bool item_image_kind_is_usable(const MediaItem &item, const std::string &kind)
{
    if (!item_prefers_still_art(item)) return true;
    if (kind != "Primary") return true;
    return item_primary_is_landscape(item);
}

static bool looks_landscape_file(const std::string &path)
{
    if (path.empty()) return false;
    std::ifstream in(path.c_str(), std::ios::binary);
    if (!in) return false;
    unsigned char header[24] = {};
    in.read(reinterpret_cast<char *>(header), sizeof(header));
    if (in.gcount() < static_cast<std::streamsize>(sizeof(header))) return false;
    const unsigned char png_sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    if (memcmp(header, png_sig, sizeof(png_sig)) != 0) return false;
    int w = (static_cast<int>(header[16]) << 24) |
            (static_cast<int>(header[17]) << 16) |
            (static_cast<int>(header[18]) << 8) |
            static_cast<int>(header[19]);
    int h = (static_cast<int>(header[20]) << 24) |
            (static_cast<int>(header[21]) << 16) |
            (static_cast<int>(header[22]) << 8) |
            static_cast<int>(header[23]);
    return w > 0 && h > 0 && static_cast<long long>(w) * 10LL >= static_cast<long long>(h) * 12LL;
}

static bool should_keep_card_image(const MediaItem &item, const std::string &kind, const std::string &path)
{
    if (!item_prefers_still_art(item)) return true;
    if (!item_image_kind_is_usable(item, kind)) return false;
    return looks_landscape_file(path);
}

static bool should_keep_background_image(const MediaItem &item, const std::string &kind, const std::string &path)
{
    if (!item_prefers_still_art(item)) return true;
    if (!item_image_kind_is_usable(item, kind)) return false;
    return looks_landscape_file(path);
}

static void fit_art_box(const MediaItem &item, int max_w, int max_h, int &art_w, int &art_h)
{
    max_w = std::max(1, max_w);
    max_h = std::max(1, max_h);
    int ratio_w = 16;
    int ratio_h = 9;
    art_ratio_for_item(item, ratio_w, ratio_h);

    long long h_by_w = static_cast<long long>(max_w) * ratio_h / ratio_w;
    if (h_by_w <= max_h) {
        art_w = max_w;
        art_h = std::max(1, static_cast<int>(h_by_w));
    } else {
        art_h = max_h;
        art_w = std::max(1, static_cast<int>(static_cast<long long>(max_h) * ratio_w / ratio_h));
    }
}

static int first_visible_index(size_t selected, size_t total, int visible_count)
{
    if (total == 0 || visible_count <= 0) return 0;
    int first = selected > 0 ? static_cast<int>(selected) - 1 : 0;
    if (total > static_cast<size_t>(visible_count) &&
        first + visible_count > static_cast<int>(total)) {
        first = static_cast<int>(total) - visible_count;
    }
    return std::max(0, first);
}

static std::string ticks_to_time(long long ticks)
{
    if (ticks <= 0) return "--:--";
    long long seconds = ticks / 10000000LL;
    long long minutes = seconds / 60;
    long long hours = minutes / 60;
    minutes %= 60;
    char buf[32];
    if (hours > 0) snprintf(buf, sizeof(buf), "%lldh %02lldm", hours, minutes);
    else snprintf(buf, sizeof(buf), "%lldm", minutes);
    return buf;
}

static long long ticks_to_seconds(long long ticks)
{
    if (ticks <= 0) return 0;
    return ticks / 10000000LL;
}

static std::string seconds_to_clock(long long seconds)
{
    if (seconds < 0) seconds = 0;
    long long hours = seconds / 3600;
    long long minutes = (seconds / 60) % 60;
    long long secs = seconds % 60;
    char buf[32];
    if (hours > 0) snprintf(buf, sizeof(buf), "%lld:%02lld:%02lld", hours, minutes, secs);
    else snprintf(buf, sizeof(buf), "%02lld:%02lld", minutes, secs);
    return buf;
}

static std::string item_display_name(const MediaItem &item)
{
    if (item.type == "Episode" && !item.series_name.empty()) {
        return item.series_name + " S" + std::to_string(item.parent_index_number) +
               "E" + std::to_string(item.index_number) + " " + item.name;
    }
    return item.name;
}

static std::string rating_label(const MediaItem &item)
{
    char buf[32];
    if (item.community_rating >= 0.0f) {
        snprintf(buf, sizeof(buf), "%.1f", item.community_rating);
        return buf;
    }
    if (item.critic_rating >= 0) {
        snprintf(buf, sizeof(buf), "%d%%", item.critic_rating);
        return buf;
    }
    return "";
}

static std::string card_meta_label(const MediaItem &item)
{
    std::string score = rating_label(item);
    if (!score.empty()) return score;
    if (is_series_item(item)) return "剧集";
    if (is_audio_item(item)) {
        if (!item.album_artist.empty()) return item.album_artist;
        if (!item.album.empty()) return item.album;
        return ticks_to_time(item.runtime_ticks);
    }
    if (is_photo_item(item)) return "";
    return "";
}

static std::string season_title(int season)
{
    static const char *digits[] = {"零", "一", "二", "三", "四", "五", "六", "七", "八", "九", "十"};
    if (season > 0 && season <= 10) return std::string("第") + digits[season] + "季";
    if (season > 10 && season < 20) return std::string("第十") + digits[season - 10] + "季";
    if (season > 0) return "第" + std::to_string(season) + "季";
    return "未知季";
}

static bool name_looks_episode_labeled(const std::string &name)
{
    return name.find("第") != std::string::npos && name.find("集") != std::string::npos;
}

static std::string item_breadcrumb(const MediaItem &item)
{
    if (is_episode_item(item)) {
        std::string text = item.series_name.empty() ? "剧集" : item.series_name;
        if (item.parent_index_number > 0) text += " / " + season_title(item.parent_index_number);
        if (item.index_number > 0) {
            text += " / 第" + std::to_string(item.index_number) + "集";
            if (!item.name.empty()) text += " " + item.name;
        } else if (!item.name.empty()) {
            text += " / " + item.name;
        }
        return text;
    }
    if (is_audio_item(item)) {
        if (!item.album.empty()) return item.album + " / " + item.name;
        return "音乐 / " + item.name;
    }
    if (is_photo_item(item)) return "图片 / " + item.name;
    return "电影 / " + item.name;
}

static std::string safe_file_id(const std::string &value)
{
    std::string out;
    for (char c : value) {
        unsigned char ch = static_cast<unsigned char>(c);
        if (std::isalnum(ch) || c == '-' || c == '_') out.push_back(c);
        else out.push_back('_');
    }
    return out.empty() ? "poster" : out;
}

static std::string lvgl_file_path(const std::string &path)
{
#if LV_USE_FS_POSIX
    if (path.size() >= 2 && path[0] >= 'A' && path[0] <= 'Z' && path[1] == ':') return path;
    std::string prefixed;
    prefixed.push_back(static_cast<char>(LV_FS_POSIX_LETTER));
    prefixed.push_back(':');
    prefixed += path;
    return prefixed;
#else
    return path;
#endif
}

static bool command_exists(const char *name)
{
    std::string cmd = "command -v " + shell_quote(name) + " >/dev/null 2>&1";
    return system(cmd.c_str()) == 0;
}

static bool path_exists(const char *path)
{
    return path && access(path, F_OK) == 0;
}

static uint64_t monotonic_ms()
{
    struct timespec ts {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000ULL +
           static_cast<uint64_t>(ts.tv_nsec / 1000000ULL);
}

static std::string read_first_line_file(const std::string &path)
{
    std::ifstream in(path);
    std::string line;
    if (std::getline(in, line)) return trim(line);
    return "";
}

static bool hdmi_output_connected()
{
    const char *skip = getenv("M5_STREAMPLAYER_SKIP_HDMI_CHECK");
    if (skip && (strcmp(skip, "1") == 0 || strcmp(skip, "true") == 0)) return true;
    DIR *dir = opendir("/sys/class/drm");
    if (!dir) return true;

    int status_count = 0;
    struct dirent *entry = nullptr;
    while ((entry = readdir(dir)) != nullptr) {
        if (!strstr(entry->d_name, "HDMI")) continue;
        std::string path = std::string("/sys/class/drm/") + entry->d_name + "/status";
        std::string status = to_lower(read_first_line_file(path));
        if (status.empty()) continue;
        status_count++;
        if (status == "connected") {
            closedir(dir);
            return true;
        }
    }
    closedir(dir);
    return status_count == 0;
}

static const char *hdmi_required_message()
{
    return "未连接 HDMI 输出；视频建议接 HDMI，音乐播放不受影响。";
}

static std::string small_lcd_fbdev_path()
{
    const char *override = getenv("M5_SMALL_FBDEV");
    if (override && *override) return override;

    std::ifstream proc("/proc/fb");
    std::string line;
    while (std::getline(proc, line)) {
        if (line.find("fb_st7789v") == std::string::npos) continue;
        std::istringstream iss(line);
        int fb = -1;
        if (iss >> fb && fb >= 0) return "/dev/fb" + std::to_string(fb);
    }
    return "/dev/fb0";
}

static const lv_font_t *builtin_font_14()
{
#if LV_FONT_SOURCE_HAN_SANS_SC_14_CJK
    return &lv_font_source_han_sans_sc_14_cjk;
#else
    return LV_FONT_DEFAULT;
#endif
}

static const lv_font_t *builtin_font_16()
{
#if LV_FONT_SOURCE_HAN_SANS_SC_16_CJK
    return &lv_font_source_han_sans_sc_16_cjk;
#else
    return builtin_font_14();
#endif
}

static lv_font_t *g_runtime_font_14 = nullptr;
static lv_font_t *g_runtime_font_16 = nullptr;
static lv_font_t *g_runtime_font_21 = nullptr;
static lv_font_t *g_runtime_font_28 = nullptr;
static bool g_runtime_fonts_initialized = false;

static const lv_font_t *ui_font_14()
{
    return g_runtime_font_14 ? g_runtime_font_14 : builtin_font_14();
}

static const lv_font_t *ui_font_16()
{
    return g_runtime_font_16 ? g_runtime_font_16 : builtin_font_16();
}

static const lv_font_t *ui_font_menu_near()
{
    return g_runtime_font_21 ? g_runtime_font_21 : ui_font_16();
}

static const lv_font_t *ui_font_menu_focus()
{
    return g_runtime_font_28 ? g_runtime_font_28 : ui_font_menu_near();
}

static std::vector<std::string> runtime_font_path_variants(const std::string &path)
{
    std::vector<std::string> variants;
    std::string trimmed = trim(path);
    if (trimmed.empty()) return variants;

    variants.push_back(trimmed);

#if LV_USE_FS_POSIX
    bool has_driver = trimmed.size() >= 2 && trimmed[0] >= 'A' && trimmed[0] <= 'Z' &&
                      trimmed[1] == ':';
    if (!has_driver) {
        std::string prefixed;
        prefixed.push_back(static_cast<char>(LV_FS_POSIX_LETTER));
        prefixed.push_back(':');
        prefixed += trimmed;
        variants.push_back(prefixed);
    }
#endif

    return variants;
}

static std::vector<std::string> candidate_font_paths()
{
    std::vector<std::string> paths;
    const char *override_path = getenv("M5_STREAMPLAYER_FONT");
    if (override_path && *override_path) paths.push_back(override_path);

    const char *defaults[] = {
        "fonts/NotoSansSC-Regular.ttf",
        "NotoSansSC-Regular.ttf",
        "dist/NotoSansSC-Regular.ttf",
        "fonts/NotoSansCJK-Regular.ttc",
        "fonts/SourceHanSansSC-Regular.otf",
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/opentype/noto/NotoSansCJKsc-Regular.otf",
        "/usr/share/fonts/truetype/noto/NotoSansSC-Regular.ttf",
        "/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc",
        "/System/Library/Fonts/Hiragino Sans GB.ttc",
    };

    for (size_t i = 0; i < sizeof(defaults) / sizeof(defaults[0]); ++i) {
        paths.push_back(defaults[i]);
    }
    return paths;
}

static void write_font_status_file(const std::string &status, const std::string &detail)
{
    FILE *file = fopen("/tmp/m5streamplayer-font-status.txt", "w");
    if (!file) return;
    fprintf(file, "%s\n%s\n", status.c_str(), detail.c_str());
    fclose(file);
}

static void init_runtime_fonts()
{
    if (g_runtime_fonts_initialized) return;
    g_runtime_fonts_initialized = true;

#if LV_USE_TINY_TTF && LV_TINY_TTF_FILE_SUPPORT
    std::vector<std::string> paths = candidate_font_paths();
    for (size_t i = 0; i < paths.size(); ++i) {
        std::vector<std::string> variants = runtime_font_path_variants(paths[i]);
        for (size_t j = 0; j < variants.size(); ++j) {
            const std::string &variant = variants[j];
            lv_font_t *font_16 = lv_tiny_ttf_create_file(variant.c_str(), 16);
            if (!font_16) continue;

            lv_font_t *font_14 = lv_tiny_ttf_create_file(variant.c_str(), 14);
            if (!font_14) {
                lv_tiny_ttf_destroy(font_16);
                continue;
            }
            lv_font_t *font_21 = lv_tiny_ttf_create_file(variant.c_str(), 21);
            lv_font_t *font_28 = lv_tiny_ttf_create_file(variant.c_str(), 28);

            g_runtime_font_16 = font_16;
            g_runtime_font_14 = font_14;
            g_runtime_font_21 = font_21 ? font_21 : font_16;
            g_runtime_font_28 = font_28 ? font_28 : (font_21 ? font_21 : font_16);
            fprintf(stderr, "StreamPlayer font loaded: %s\n", variant.c_str());
            write_font_status_file("loaded", variant);
            return;
        }
    }
#endif

    fprintf(stderr, "StreamPlayer font fallback: built-in\n");
    write_font_status_file("fallback", "built-in");
}

static void anim_set_x(void *obj, int32_t value)
{
    lv_obj_set_x(static_cast<lv_obj_t *>(obj), value);
}

static void anim_set_y(void *obj, int32_t value)
{
    lv_obj_set_y(static_cast<lv_obj_t *>(obj), value);
}

static void anim_set_opa(void *obj, int32_t value)
{
    lv_obj_set_style_opa(static_cast<lv_obj_t *>(obj), static_cast<lv_opa_t>(value), 0);
}

static void start_value_anim(lv_obj_t *obj,
                             lv_anim_exec_xcb_t exec,
                             int32_t from,
                             int32_t to,
                             uint32_t duration)
{
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, obj);
    lv_anim_set_values(&anim, from, to);
    lv_anim_set_duration(&anim, duration);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&anim, exec);
    lv_anim_start(&anim);
}

static void play_ui_sound(bool vertical)
{
    const char *disabled = getenv("M5_STREAMPLAYER_SOUND");
    if (disabled && strcmp(disabled, "0") == 0) return;

    const char *custom = getenv(vertical ? "M5_STREAMPLAYER_SOUND_VERTICAL"
                                        : "M5_STREAMPLAYER_SOUND_HORIZONTAL");
    if (custom && *custom) {
        pid_t pid = fork();
        if (pid == 0) {
            execlp("sh", "sh", "-c", custom, static_cast<char *>(nullptr));
            _exit(0);
        }
        return;
    }

    fputc('\a', stderr);
    fflush(stderr);
}

class MediaServerClient {
public:
    explicit MediaServerClient(ServerConfig config) : cfg_(std::move(config)) {}

    const ServerConfig &config() const { return cfg_; }
    ServerConfig &config() { return cfg_; }

    bool authenticate(const std::string &password, std::string &error)
    {
        std::string body = "{\"Username\":\"" + json_escape(cfg_.username) +
                           "\",\"Pw\":\"" + json_escape(password) + "\"}";
        HttpResponse res = curl_request("POST", join_url(cfg_.base_url, "/Users/AuthenticateByName"), "", body);
        if (!res.ok) {
            error = "Login failed: " + ellipsize(trim(res.error), 80);
            return false;
        }
        cfg_.access_token = json_get_string(res.body, "AccessToken");
        std::string user_obj = json_get_object(res.body, "User");
        if (user_obj.empty()) user_obj = res.body;
        cfg_.user_id = json_get_string(user_obj, "Id");
        if (cfg_.access_token.empty() || cfg_.user_id.empty()) {
            error = "Login response missing token or user id.";
            return false;
        }
        save_config(cfg_);
        return true;
    }

    bool load_libraries(std::vector<MediaLibrary> &libraries, std::string &error)
    {
        std::string url = join_url(cfg_.base_url, "/Users/" + url_encode(cfg_.user_id) + "/Views");
        HttpResponse res = curl_request("GET", url, cfg_.access_token);
        if (!res.ok) {
            url = join_url(cfg_.base_url, "/Users/" + url_encode(cfg_.user_id) +
                                           "/Items?IncludeItemTypes=CollectionFolder&Recursive=false");
            res = curl_request("GET", url, cfg_.access_token);
        }
        if (!res.ok) {
            error = "Library load failed: " + ellipsize(trim(res.error), 80);
            return false;
        }
        libraries.clear();
        for (const auto &obj : json_objects_in_array(res.body, "Items")) {
            MediaLibrary lib = parse_library(obj);
            if (lib.id.empty() || lib.name.empty()) continue;
            std::string type = to_lower(lib.type);
            if (type.empty() || type == "movies" || type == "tvshows" || type == "homevideos" ||
                type == "boxsets" || type == "collectionfolder" || type == "music" ||
                type == "audiobooks" || type == "photos" || type == "homevideosphotos" ||
                type == "mixedcontent") {
                libraries.push_back(lib);
            }
        }
        if (libraries.empty()) {
            MediaLibrary all;
            all.name = "All Videos";
            all.type = "all";
            libraries.push_back(all);
        }

        MediaLibrary recent;
        recent.id = kRecentPlayedLibraryId;
        recent.name = language_from_code(cfg_.language.empty() ? system_language_code() : cfg_.language) == UiLanguage::Chinese
            ? "最近播放"
            : "Recent";
        recent.type = kRecentPlayedLibraryType;
        libraries.push_back(recent);

        MediaLibrary settings;
        settings.id = kSettingsLibraryId;
        settings.name = language_from_code(cfg_.language.empty() ? system_language_code() : cfg_.language) == UiLanguage::Chinese
            ? "设置"
            : "Settings";
        settings.type = kSettingsLibraryType;
        libraries.push_back(settings);
        return true;
    }

    bool load_items(const MediaLibrary &library,
                    SortMode sort,
                    int start_index,
                    int limit,
                    std::vector<MediaItem> &items,
                    std::string &error)
    {
        if (library_is_settings(library)) {
            items.clear();
            error.clear();
            return true;
        }

        std::string path = "/Users/" + url_encode(cfg_.user_id) + "/Items?";
        if (library_is_recent_played(library)) {
            path += "Recursive=true&IncludeItemTypes=Movie,Episode,Video,Audio&";
        } else {
            if (!library.id.empty()) path += "ParentId=" + url_encode(library.id) + "&";
            if (library_prefers_series(library)) {
                path += "Recursive=false&IncludeItemTypes=Series&";
            } else if (library_prefers_music(library)) {
                path += "Recursive=true&IncludeItemTypes=Audio&MediaTypes=Audio&";
            } else if (library_prefers_photos(library)) {
                path += "Recursive=true&IncludeItemTypes=Photo&MediaTypes=Photo&";
            } else {
                path += "Recursive=true&IncludeItemTypes=Movie,Episode,Video&MediaTypes=Video&";
            }
        }
        path += "StartIndex=" + std::to_string(start_index) + "&Limit=" + std::to_string(limit) + "&";
        path += library_is_recent_played(library) ? recent_played_query() : sort_query(sort);
        path += "&Fields=Overview,DateCreated,PremiereDate,RunTimeTicks,MediaSources,MediaStreams,ParentId,SeriesName,SeriesId,SeasonId,Path,Genres,ProductionYear,ImageTags,BackdropImageTags,PrimaryImageAspectRatio,CommunityRating,CriticRating,Album,AlbumArtist,ParentThumbImageTag,ParentBackdropImageTags";
        HttpResponse res = curl_request("GET", join_url(cfg_.base_url, path), cfg_.access_token);
        if (!res.ok) {
            error = "媒体加载失败: " + ellipsize(trim(res.error), 80);
            return false;
        }
        items.clear();
        for (const auto &obj : json_objects_in_array(res.body, "Items")) {
            MediaItem item = parse_item(obj);
            if (!item.id.empty() && !item.name.empty()) items.push_back(item);
        }
        return true;
    }

    bool search_items(const std::string &query,
                      SortMode sort,
                      int start_index,
                      int limit,
                      std::vector<MediaItem> &items,
                      std::string &error)
    {
        std::string path = "/Users/" + url_encode(cfg_.user_id) + "/Items?";
        path += "Recursive=true&";
        path += "SearchTerm=" + url_encode(query) + "&";
        path += "IncludeItemTypes=Movie,Episode,Video,Audio,Photo&";
        path += "StartIndex=" + std::to_string(start_index) + "&Limit=" + std::to_string(limit) + "&";
        path += sort_query(sort);
        path += "&Fields=Overview,DateCreated,PremiereDate,RunTimeTicks,MediaSources,MediaStreams,ParentId,SeriesName,SeriesId,SeasonId,Path,Genres,ProductionYear,ImageTags,BackdropImageTags,PrimaryImageAspectRatio,CommunityRating,CriticRating,Album,AlbumArtist,ParentThumbImageTag,ParentBackdropImageTags";
        HttpResponse res = curl_request("GET", join_url(cfg_.base_url, path), cfg_.access_token);
        if (!res.ok) {
            error = "搜索失败: " + ellipsize(trim(res.error), 80);
            return false;
        }
        items.clear();
        for (const auto &obj : json_objects_in_array(res.body, "Items")) {
            MediaItem item = parse_item(obj);
            if (!item.id.empty() && !item.name.empty()) items.push_back(item);
        }
        return true;
    }

    std::string poster_url(const MediaItem &item, int width, int height) const
    {
        return image_url(item, "Primary", width, height);
    }

    std::string image_url(const MediaItem &item, const std::string &kind, int width, int height) const
    {
        if (item.id.empty() || cfg_.base_url.empty()) return "";
        std::string path = "/Items/" + url_encode(item.id) + "/Images/" + kind;
        if (kind == "Backdrop") path += "/0";
        std::string url = join_url(cfg_.base_url, path);
        url = append_query(url, "width", std::to_string(width));
        url = append_query(url, "height", std::to_string(height));
        url = append_query(url, "quality", "75");
        url = append_query(url, "format", "png");
        if (kind == "Primary" && !item.primary_image_tag.empty()) url = append_query(url, "tag", item.primary_image_tag);
        if (kind == "Thumb" && !item.thumb_image_tag.empty()) url = append_query(url, "tag", item.thumb_image_tag);
        if (kind == "Backdrop" && !item.backdrop_image_tag.empty()) url = append_query(url, "tag", item.backdrop_image_tag);
        return ensure_api_key(url, cfg_.access_token);
    }

    std::string image_fit_url(const MediaItem &item, const std::string &kind, int max_width, int max_height) const
    {
        if (item.id.empty() || cfg_.base_url.empty()) return "";
        std::string path = "/Items/" + url_encode(item.id) + "/Images/" + kind;
        if (kind == "Backdrop") path += "/0";
        std::string url = join_url(cfg_.base_url, path);
        url = append_query(url, "maxWidth", std::to_string(max_width));
        url = append_query(url, "maxHeight", std::to_string(max_height));
        url = append_query(url, "quality", "82");
        url = append_query(url, "format", "png");
        if (kind == "Primary" && !item.primary_image_tag.empty()) url = append_query(url, "tag", item.primary_image_tag);
        if (kind == "Thumb" && !item.thumb_image_tag.empty()) url = append_query(url, "tag", item.thumb_image_tag);
        if (kind == "Backdrop" && !item.backdrop_image_tag.empty()) url = append_query(url, "tag", item.backdrop_image_tag);
        return ensure_api_key(url, cfg_.access_token);
    }

    std::string photo_view_url(const MediaItem &item) const
    {
        if (item.id.empty() || cfg_.base_url.empty()) return "";
        std::string url = join_url(cfg_.base_url, "/Items/" + url_encode(item.id) + "/Images/Primary");
        url = append_query(url, "maxWidth", "1920");
        url = append_query(url, "maxHeight", "1080");
        url = append_query(url, "quality", "90");
        url = append_query(url, "format", "jpg");
        if (!item.primary_image_tag.empty()) url = append_query(url, "tag", item.primary_image_tag);
        return ensure_api_key(url, cfg_.access_token);
    }

    std::string audio_stream_url(const MediaItem &item) const
    {
        if (item.id.empty() || cfg_.base_url.empty()) return "";
        std::string url = join_url(cfg_.base_url, "/Audio/" + url_encode(item.id) + "/stream");
        url = append_query(url, "static", "true");
        if (!item.container.empty()) url = append_query(url, "container", item.container);
        return ensure_api_key(url, cfg_.access_token);
    }

    bool load_seasons(const MediaItem &series, std::vector<MediaItem> &seasons, std::string &error)
    {
        std::string path = "/Shows/" + url_encode(series.id) + "/Seasons?";
        path += "UserId=" + url_encode(cfg_.user_id);
        path += "&Fields=Overview,DateCreated,PremiereDate,RunTimeTicks,ImageTags,BackdropImageTags,CommunityRating,CriticRating,ParentThumbImageTag,ParentBackdropImageTags";
        path += "&SortBy=SortName&SortOrder=Ascending";
        HttpResponse res = curl_request("GET", join_url(cfg_.base_url, path), cfg_.access_token);
        if (!res.ok) {
            error = "分季加载失败。";
            return false;
        }
        seasons.clear();
        for (const auto &obj : json_objects_in_array(res.body, "Items")) {
            MediaItem season = parse_item(obj);
            if (!season.id.empty()) seasons.push_back(season);
        }
        if (seasons.empty()) {
            MediaItem all;
            all.id = "";
            all.name = "全部";
            all.type = "Season";
            seasons.push_back(all);
        }
        return true;
    }

    bool load_episodes(const std::string &series_id,
                       const std::string &season_id,
                       std::vector<MediaItem> &episodes,
                       std::string &error)
    {
        std::string path = "/Shows/" + url_encode(series_id) + "/Episodes?";
        path += "UserId=" + url_encode(cfg_.user_id);
        if (!season_id.empty()) path += "&SeasonId=" + url_encode(season_id);
        path += "&Fields=Overview,DateCreated,PremiereDate,RunTimeTicks,MediaSources,MediaStreams,SeriesName,SeriesId,SeasonId,Path,Genres,ProductionYear,ImageTags,BackdropImageTags,CommunityRating,CriticRating,ParentThumbImageTag,ParentBackdropImageTags";
        path += "&SortBy=ParentIndexNumber,IndexNumber&SortOrder=Ascending";
        HttpResponse res = curl_request("GET", join_url(cfg_.base_url, path), cfg_.access_token);
        if (!res.ok) {
            error = "集数加载失败。";
            return false;
        }
        episodes.clear();
        for (const auto &obj : json_objects_in_array(res.body, "Items")) {
            MediaItem episode = parse_item(obj);
            if (!episode.id.empty()) episodes.push_back(episode);
        }
        return true;
    }

    bool playback_info(const MediaItem &item, PlaybackChoice &choice, std::string &error,
                       bool small_screen = false)
    {
        const int target_width = small_screen ? kSmallScreenTranscodeMaxWidth : kTranscodeMaxWidth;
        const int target_height = small_screen ? kSmallScreenTranscodeMaxHeight : kTranscodeMaxHeight;
        const int target_video_bitrate = small_screen ? kSmallScreenTranscodeVideoBitrate : kTranscodeVideoBitrate;
        const int target_max_bitrate = small_screen ? kSmallScreenTranscodeMaxBitrate : kTranscodeMaxBitrate;
        const std::string target_label = small_screen ? "320x170" : "1080p30";
        app_log("PlaybackInfo request item=" + item.id + " name=" + item.name +
                " codec=" + item.video_codec + "/" + item.audio_codec +
                " size=" + std::to_string(item.width) + "x" + std::to_string(item.height) +
                " target=" + target_label +
                " h264/aac bitrate=" + std::to_string(target_max_bitrate));
        const std::string max_bitrate = std::to_string(target_max_bitrate);
        const std::string video_bitrate = std::to_string(target_video_bitrate);
        const std::string audio_bitrate = std::to_string(kTranscodeAudioBitrate);
        const std::string max_width = std::to_string(target_width);
        const std::string max_height = std::to_string(target_height);
        const std::string max_framerate = std::to_string(kTranscodeMaxFramerate);
        std::string body =
            "{\"UserId\":\"" + json_escape(cfg_.user_id) + "\","
            "\"MaxStreamingBitrate\":" + max_bitrate + ","
            "\"MaxAudioChannels\":2,"
            "\"EnableDirectPlay\":false,"
            "\"EnableDirectStream\":false,"
            "\"EnableTranscoding\":true,"
            "\"AllowVideoStreamCopy\":false,"
            "\"AllowAudioStreamCopy\":false,"
            "\"IsPlayback\":true,"
            "\"DeviceProfile\":{"
            "\"Name\":\"" + std::string(small_screen ? "M5CardputerZero Small Screen" : "M5CardputerZero HDMI") + "\","
            "\"SupportedMediaTypes\":\"Video\","
            "\"MaxStreamingBitrate\":" + max_bitrate + ","
            "\"MaxStaticBitrate\":" + max_bitrate + ","
            "\"MaxAudioChannels\":2,"
            "\"DirectPlayProfiles\":[],"
            "\"TranscodingProfiles\":[{\"Container\":\"ts\",\"Type\":\"Video\",\"VideoCodec\":\"h264\",\"AudioCodec\":\"aac\",\"Protocol\":\"hls\",\"Context\":\"Streaming\",\"MaxWidth\":" + max_width + ",\"MaxHeight\":" + max_height + ",\"MaxAudioChannels\":\"2\"}],"
            "\"SubtitleProfiles\":[{\"Format\":\"srt\",\"Method\":\"External\"},{\"Format\":\"ass\",\"Method\":\"External\"}]"
            "}}";

        std::string url = join_url(cfg_.base_url, "/Items/" + url_encode(item.id) + "/PlaybackInfo");
        url = append_query(url, "UserId", cfg_.user_id);
        url = append_query(url, "MaxStreamingBitrate", max_bitrate);
        url = append_query(url, "VideoCodec", "h264");
        url = append_query(url, "AudioCodec", "aac");
        url = append_query(url, "VideoBitrate", video_bitrate);
        url = append_query(url, "AudioBitrate", audio_bitrate);
        url = append_query(url, "MaxWidth", max_width);
        url = append_query(url, "MaxHeight", max_height);
        url = append_query(url, "MaxFramerate", max_framerate);
        url = append_query(url, "MaxAudioChannels", "2");
        url = append_query(url, "SegmentContainer", "ts");
        url = append_query(url, "SegmentLength", "3");
        HttpResponse res = curl_request("POST", url, cfg_.access_token, body);
        if (!res.ok) {
            error = "PlaybackInfo failed: " + ellipsize(trim(res.error), 80);
            app_log("PlaybackInfo failed item=" + item.id + " error=" + error);
            return false;
        }

        choice = PlaybackChoice();
        choice.play_session_id = json_get_string(res.body, "PlaySessionId");
        std::vector<std::string> sources = json_objects_in_array(res.body, "MediaSources");
        app_log("PlaybackInfo response item=" + item.id +
                " session=" + choice.play_session_id +
                " sources=" + std::to_string(sources.size()));
        std::string best_transcode;
        std::string best_source_id;
        bool server_can_transcode = false;
        int best_source_score = 0x7fffffff;
        for (const auto &source : sources) {
            std::string source_id = json_get_string(source, "Id");
            bool supports_transcode = json_get_bool(source, "SupportsTranscoding", false);
            std::string transcode = json_get_string(source, "TranscodingUrl");
            if (!transcode.empty() && transcode[0] == '/') transcode = join_url(cfg_.base_url, transcode);
            if (!transcode.empty()) {
                transcode = set_query_param(transcode, "VideoCodec", "h264");
                transcode = set_query_param(transcode, "AudioCodec", "aac");
                transcode = set_query_param(transcode, "VideoBitrate", video_bitrate);
                transcode = set_query_param(transcode, "AudioBitrate", audio_bitrate);
                transcode = set_query_param(transcode, "MaxWidth", max_width);
                transcode = set_query_param(transcode, "MaxHeight", max_height);
                transcode = set_query_param(transcode, "MaxFramerate", max_framerate);
                transcode = set_query_param(transcode, "MaxAudioChannels", "2");
                transcode = set_query_param(transcode, "TranscodingMaxAudioChannels", "2");
                transcode = set_query_param(transcode, "SegmentContainer", "ts");
                transcode = set_query_param(transcode, "SegmentLength", "3");
                transcode = set_query_param(transcode, "allowVideoStreamCopy", "false");
                transcode = set_query_param(transcode, "allowAudioStreamCopy", "false");
                transcode = ensure_api_key(transcode, cfg_.access_token);
            }
            if (best_source_id.empty()) best_source_id = source_id;
            if (supports_transcode) server_can_transcode = true;
            if (supports_transcode) {
                int score = transcode_source_score(source, target_width, target_height);
                app_log("PlaybackInfo source item=" + item.id + " source=" + source_id +
                        " supports_transcode=" + bool_text(supports_transcode) +
                        " has_url=" + bool_text(!transcode.empty()) +
                        " width=" + std::to_string(json_get_int(source, "Width", 0)) +
                        " height=" + std::to_string(json_get_int(source, "Height", 0)) +
                        " bitrate=" + std::to_string(json_get_int(source, "Bitrate", 0)) +
                        " score=" + std::to_string(score));
                if (best_transcode.empty() || score < best_source_score) {
                    best_transcode = transcode.empty()
                        ? fallback_hls_url(item.id, source_id, target_video_bitrate, target_width, target_height)
                        : transcode;
                    best_source_id = source_id;
                    best_source_score = score;
                }
            } else {
                app_log("PlaybackInfo source item=" + item.id + " source=" + source_id +
                        " supports_transcode=0 has_url=" + bool_text(!transcode.empty()));
            }
            if (choice.streams.empty()) choice.streams = parse_streams(source);
        }

        if (sources.empty()) {
            error = "服务器没有返回媒体源。";
            app_log("PlaybackInfo no sources item=" + item.id);
            return false;
        }
        if (!server_can_transcode || best_transcode.empty()) {
            error = "当前视频需要服务器转码为 " + target_label +
                    " H.264/AAC，但服务器没有返回可用转码能力。";
            app_log("PlaybackInfo no transcode item=" + item.id +
                    " server_can_transcode=" + bool_text(server_can_transcode) +
                    " best_url=" + bool_text(!best_transcode.empty()));
            return false;
        }

        choice.url = best_transcode;
        choice.transcoding = true;
        choice.summary = target_label + " H.264/AAC transcode";
        choice.media_source_id = best_source_id;
        app_log("PlaybackInfo selected item=" + item.id +
                " source=" + best_source_id +
                " score=" + std::to_string(best_source_score) +
                " url=" + choice.url);
        return !choice.url.empty();
    }

    bool report_playing(const MediaItem &item,
                        const PlaybackChoice &choice,
                        long long position_ticks,
                        bool paused)
    {
        return report_playback("/Sessions/Playing", item, choice, position_ticks, paused, "play");
    }

    bool report_progress(const MediaItem &item,
                         const PlaybackChoice &choice,
                         long long position_ticks,
                         bool paused)
    {
        return report_playback("/Sessions/Playing/Progress", item, choice, position_ticks, paused, "timeupdate");
    }

    bool report_stopped(const MediaItem &item,
                        const PlaybackChoice &choice,
                        long long position_ticks)
    {
        return report_playback("/Sessions/Playing/Stopped", item, choice, position_ticks, false, "stop");
    }

    bool load_next_episode(const MediaItem &current, MediaItem &next, std::string &error)
    {
        if (current.series_id.empty()) {
            error = "Current item is not an episode.";
            return false;
        }
        std::string path = "/Shows/" + url_encode(current.series_id) + "/Episodes?";
        path += "UserId=" + url_encode(cfg_.user_id);
        path += "&Fields=Overview,DateCreated,PremiereDate,RunTimeTicks,MediaStreams,SeriesName,SeriesId,SeasonId,ImageTags,BackdropImageTags,ParentThumbImageTag,ParentBackdropImageTags";
        path += "&SortBy=ParentIndexNumber,IndexNumber&SortOrder=Ascending";
        HttpResponse res = curl_request("GET", join_url(cfg_.base_url, path), cfg_.access_token);
        if (!res.ok) {
            error = "Next episode load failed.";
            return false;
        }
        bool seen_current = false;
        for (const auto &obj : json_objects_in_array(res.body, "Items")) {
            MediaItem item = parse_item(obj);
            if (seen_current && !item.id.empty()) {
                next = item;
                return true;
            }
            if (item.id == current.id) seen_current = true;
        }
        error = "No next episode found.";
        return false;
    }

private:
    static int transcode_source_score(const std::string &source, int target_width, int target_height)
    {
        std::string hay = to_lower(source);
        int score = 0;
        if (contains_ci(hay, "dolby vision") || contains_ci(hay, "dovi") ||
            contains_ci(hay, "dvhe") || contains_ci(hay, "dvh1")) {
            score += 1000000;
        }
        if (contains_ci(hay, "hdr10") || contains_ci(hay, "\"hdr\"") ||
            contains_ci(hay, "video range\":\"hdr")) {
            score += 100000;
        }

        int width = static_cast<int>(json_get_int(source, "Width", 0));
        int height = static_cast<int>(json_get_int(source, "Height", 0));
        long long bitrate = json_get_int(source, "Bitrate", 0);
        if (width > target_width) score += (width - target_width) / 10;
        if (height > target_height) score += (height - target_height) / 10;
        if (bitrate > 0) score += static_cast<int>(std::min<long long>(bitrate / 100000, 10000));
        return score;
    }

    static bool source_is_local_profile(const std::string &source,
                                        const MediaItem &item,
                                        const std::string &container)
    {
        std::string c = to_lower(container.empty() ? item.container : container);
        bool container_ok = contains_ci(c, "mp4") || contains_ci(c, "m4v") || contains_ci(c, "mov");
        std::string video = to_lower(item.video_codec);
        std::string audio = to_lower(item.audio_codec);
        for (const auto &stream : parse_streams(source)) {
            if (stream.type == "Video" && !stream.codec.empty()) video = to_lower(stream.codec);
            if (stream.type == "Audio" && !stream.codec.empty() && audio.empty()) audio = to_lower(stream.codec);
        }
        bool video_ok = video.empty() || video == "h264" || video == "mpeg4" || video == "mpeg4video";
        bool audio_ok = audio.empty() || audio == "aac" || audio == "mp3" || audio == "mp2";
        int width = static_cast<int>(json_get_int(source, "Width", item.width));
        int height = static_cast<int>(json_get_int(source, "Height", item.height));
        double fps = json_get_number(source, "AverageFrameRate", item.frame_rate);
        bool size_ok = (width <= 0 || width <= kTranscodeMaxWidth) &&
                       (height <= 0 || height <= kTranscodeMaxHeight);
        bool fps_ok = fps <= 0.1 || fps <= kTranscodeMaxFramerate + 0.5;
        return container_ok && video_ok && audio_ok && size_ok && fps_ok;
    }

    std::string fallback_direct_url(const std::string &item_id, const std::string &source_id)
    {
        std::string url = join_url(cfg_.base_url, "/Videos/" + url_encode(item_id) + "/stream.mp4");
        url = append_query(url, "static", "true");
        if (!source_id.empty()) url = append_query(url, "MediaSourceId", source_id);
        return ensure_api_key(url, cfg_.access_token);
    }

    std::string fallback_hls_url(const std::string &item_id, const std::string &source_id,
                                 int video_bitrate = kTranscodeVideoBitrate,
                                 int max_width = kTranscodeMaxWidth,
                                 int max_height = kTranscodeMaxHeight)
    {
        std::string url = join_url(cfg_.base_url, "/Videos/" + url_encode(item_id) + "/master.m3u8");
        if (!source_id.empty()) url = append_query(url, "MediaSourceId", source_id);
        url = set_query_param(url, "VideoCodec", "h264");
        url = set_query_param(url, "AudioCodec", "aac");
        url = set_query_param(url, "VideoBitrate", std::to_string(video_bitrate));
        url = set_query_param(url, "AudioBitrate", std::to_string(kTranscodeAudioBitrate));
        url = set_query_param(url, "MaxWidth", std::to_string(max_width));
        url = set_query_param(url, "MaxHeight", std::to_string(max_height));
        url = set_query_param(url, "MaxFramerate", std::to_string(kTranscodeMaxFramerate));
        url = set_query_param(url, "MaxAudioChannels", "2");
        url = set_query_param(url, "TranscodingMaxAudioChannels", "2");
        url = set_query_param(url, "SegmentContainer", "ts");
        url = set_query_param(url, "SegmentLength", "3");
        url = set_query_param(url, "allowVideoStreamCopy", "false");
        url = set_query_param(url, "allowAudioStreamCopy", "false");
        return ensure_api_key(url, cfg_.access_token);
    }

    ServerConfig cfg_;

    bool report_playback(const std::string &endpoint,
                         const MediaItem &item,
                         const PlaybackChoice &choice,
                         long long position_ticks,
                         bool paused,
                         const char *event_name)
    {
        if (cfg_.access_token.empty() || item.id.empty() || choice.play_session_id.empty()) return false;
        std::string method = choice.transcoding ? "Transcode" :
                             (choice.direct_playable ? "DirectPlay" : "DirectStream");
        std::string body =
            "{\"ItemId\":\"" + json_escape(item.id) + "\","
            "\"CanSeek\":true,"
            "\"PlayMethod\":\"" + method + "\","
            "\"PlaySessionId\":\"" + json_escape(choice.play_session_id) + "\","
            "\"PositionTicks\":" + std::to_string(std::max<long long>(0, position_ticks)) + ","
            "\"IsPaused\":" + std::string(paused ? "true" : "false") + ","
            "\"IsMuted\":false,"
            "\"EventName\":\"" + event_name + "\"";
        if (!choice.media_source_id.empty()) {
            body += ",\"MediaSourceId\":\"" + json_escape(choice.media_source_id) + "\"";
        }
        body += "}";
        HttpResponse res = curl_request("POST", join_url(cfg_.base_url, endpoint), cfg_.access_token, body);
        app_log(std::string("Playback report ") + endpoint + " item=" + item.id +
                " session=" + choice.play_session_id +
                " position_ticks=" + std::to_string(position_ticks) +
                " paused=" + bool_text(paused) +
                " method=" + method +
                " ok=" + bool_text(res.ok) +
                (res.ok ? "" : (" exit=" + std::to_string(res.exit_code) +
                                " error=" + ellipsize(trim(res.error), 120))));
        return res.ok;
    }
};

class HdmiPlayer {
public:
    ~HdmiPlayer() { stop(); }

    bool start(const std::string &url, const std::string &token, std::string &error,
               bool keep_open = false, bool video_mode = true,
               AudioOutput audio_output = AudioOutput::Speaker,
               long long start_seconds = 0,
               bool small_screen_output = false)
    {
        audio_output_ = audio_output;
        last_start_offset_supported_ = start_seconds <= 0;
        app_log("HdmiPlayer start request keep_open=" + bool_text(keep_open) +
                " video_mode=" + bool_text(video_mode) +
                " audio_output=" + audio_output_label(audio_output_) +
                " start_seconds=" + std::to_string(start_seconds) +
                " small_screen=" + bool_text(small_screen_output) +
                " backend_env=" + (getenv("M5_HDMI_BACKEND") ? getenv("M5_HDMI_BACKEND") : "<auto>") +
                " custom=" + bool_text(getenv("M5_HDMI_PLAYER") && getenv("M5_HDMI_PLAYER")[0]) +
                " url=" + url);
        stop();
        apply_pipewire_default_output(audio_output_);
        if (small_screen_output) {
            if (command_exists("ffmpeg")) {
                return launch_ffmpeg_small_fbdev(url, token, error, start_seconds, video_mode);
            }
            error = "小屏播放需要 ffmpeg。";
            app_log("HdmiPlayer small-screen failed no ffmpeg");
            return false;
        }
        const char *override_cmd = getenv("M5_HDMI_PLAYER");
        if (override_cmd && override_cmd[0]) {
            std::string cmd = "PULSE_SINK=" + shell_quote(audio_output_pulse_sink(audio_output_)) + " " +
                              std::string(override_cmd) + " " + shell_quote(url) +
                              " >/tmp/m5streamplayer-player.log 2>&1 &";
            int rc = system(cmd.c_str());
            if (rc == 0) {
                backend_ = "custom";
                last_start_offset_supported_ = start_seconds <= 0;
                app_log("HdmiPlayer custom started command=" + std::string(override_cmd));
                return true;
            }
            error = "M5_HDMI_PLAYER failed.";
            app_log("HdmiPlayer custom failed rc=" + std::to_string(rc));
            return false;
        }

        const char *backend = getenv("M5_HDMI_BACKEND");
        if (backend && strcmp(backend, "desktop") == 0) {
            if (launch_desktop_player(url, token, error, keep_open, video_mode, start_seconds)) return true;
            if (error.empty()) error = "Desktop HDMI player failed.";
            return false;
        }
        if (backend && strcmp(backend, "ffmpeg-fbdev") == 0 && command_exists("ffmpeg")) {
            return launch_ffmpeg_fbdev(url, token, error, start_seconds);
        }
        if (backend && strcmp(backend, "ffplay") == 0 && command_exists("ffplay")) {
            return launch_simple("ffplay", url, error, keep_open, video_mode, start_seconds);
        }
        if (backend && strcmp(backend, "vlc") == 0 && command_exists("vlc")) {
            return launch_simple("vlc", url, error, keep_open, video_mode, start_seconds);
        }
        if (backend && strcmp(backend, "mpv") == 0 && command_exists("mpv")) {
            return launch_mpv(url, token, error, keep_open, start_seconds);
        }

        if (desktop_session_available() && launch_desktop_player(url, token, error, keep_open, video_mode, start_seconds)) return true;
        if (command_exists("mpv")) return launch_mpv(url, token, error, keep_open, start_seconds);
        if (!video_mode && command_exists("ffplay")) return launch_simple("ffplay", url, error, keep_open, video_mode, start_seconds);
        if (command_exists("ffmpeg")) return launch_ffmpeg_fbdev(url, token, error, start_seconds);
        if (command_exists("ffplay")) return launch_simple("ffplay", url, error, keep_open, video_mode, start_seconds);
        if (command_exists("vlc")) return launch_simple("vlc", url, error, keep_open, video_mode, start_seconds);
        error = "Install mpv, ffmpeg, ffplay, vlc, or set M5_HDMI_PLAYER.";
        app_log("HdmiPlayer start failed no backend url=" + url);
        return false;
    }

    void stop()
    {
        if (pid_ > 0) {
            app_log("HdmiPlayer stop pid=" + std::to_string(static_cast<long long>(pid_)) +
                    " backend=" + backend_);
            kill(pid_, SIGTERM);
            for (int i = 0; i < 20; ++i) {
                if (waitpid(pid_, nullptr, WNOHANG) == pid_) {
                    app_log("HdmiPlayer stopped pid=" + std::to_string(static_cast<long long>(pid_)));
                    pid_ = -1;
                    break;
                }
                usleep(10000);
            }
            if (pid_ > 0) {
                kill(pid_, SIGKILL);
                waitpid(pid_, nullptr, WNOHANG);
                app_log("HdmiPlayer killed pid=" + std::to_string(static_cast<long long>(pid_)));
            }
            pid_ = -1;
        }
        paused_ = false;
        unlink(kMpvSocket);
    }

    bool pause()
    {
        if (backend_ == "mpv") {
            bool ok = mpv_command("{\"command\":[\"cycle\",\"pause\"]}\n");
            if (ok) paused_ = !paused_;
            app_log("HdmiPlayer pause backend=mpv ok=" + bool_text(ok) +
                    " paused=" + bool_text(paused_));
            return ok;
        }
        if (pid_ > 0 && kill(pid_, paused_ ? SIGCONT : SIGSTOP) == 0) {
            paused_ = !paused_;
            app_log("HdmiPlayer pause signal pid=" + std::to_string(static_cast<long long>(pid_)) +
                    " paused=" + bool_text(paused_));
            return true;
        }
        app_log("HdmiPlayer pause failed backend=" + backend_);
        return false;
    }
    bool seek(int seconds)
    {
        return mpv_command("{\"command\":[\"seek\"," + std::to_string(seconds) + ",\"relative\"]}\n");
    }
    bool cycle_audio() { return mpv_command("{\"command\":[\"cycle\",\"audio\"]}\n"); }
    bool cycle_subtitle() { return mpv_command("{\"command\":[\"cycle\",\"sub\"]}\n"); }

    const std::string &backend() const { return backend_; }
    bool paused() const { return paused_; }
    bool running() const { return pid_ > 0; }
    bool start_offset_supported() const { return last_start_offset_supported_; }

private:
    static std::string seconds_arg(long long seconds)
    {
        return std::to_string(std::max<long long>(0, seconds));
    }

    static void exec_args(const std::vector<std::string> &args)
    {
        std::vector<char *> argv;
        argv.reserve(args.size() + 1);
        for (const std::string &arg : args) {
            argv.push_back(const_cast<char *>(arg.c_str()));
        }
        argv.push_back(nullptr);
        execvp(args.front().c_str(), argv.data());
    }

    static void setup_audio_output_environment(AudioOutput output)
    {
        if (!getenv("XDG_RUNTIME_DIR") && access("/run/user/1000", F_OK) == 0) {
            setenv("XDG_RUNTIME_DIR", "/run/user/1000", 1);
        }
        std::string sink = audio_output_pulse_sink(output);
        if (!sink.empty()) setenv("PULSE_SINK", sink.c_str(), 1);
        else unsetenv("PULSE_SINK");
    }

    static std::string runtime_dir()
    {
        const char *xdg = getenv("XDG_RUNTIME_DIR");
        if (xdg && *xdg) return xdg;
        if (getuid() == 0 && path_exists("/run/user/1000")) return "/run/user/1000";
        return "/run/user/" + std::to_string(static_cast<unsigned long>(getuid()));
    }

    static bool desktop_session_available()
    {
        if (getenv("DISPLAY") || getenv("WAYLAND_DISPLAY")) return true;
        std::string rt = runtime_dir();
        if (path_exists((rt + "/wayland-0").c_str()) ||
            path_exists((rt + "/wayland-1").c_str())) {
            return true;
        }
        return path_exists("/tmp/.X11-unix/X0");
    }

    static bool setup_desktop_environment()
    {
        bool ok = false;
        std::string rt = runtime_dir();
        if (path_exists(rt.c_str())) setenv("XDG_RUNTIME_DIR", rt.c_str(), 1);

        if (!getenv("WAYLAND_DISPLAY")) {
            if (path_exists((rt + "/wayland-0").c_str())) {
                setenv("WAYLAND_DISPLAY", "wayland-0", 1);
                ok = true;
            } else if (path_exists((rt + "/wayland-1").c_str())) {
                setenv("WAYLAND_DISPLAY", "wayland-1", 1);
                ok = true;
            }
        } else {
            ok = true;
        }

        if (!ok && !getenv("DISPLAY") && path_exists("/tmp/.X11-unix/X0")) {
            setenv("DISPLAY", ":0", 1);
            if (!getenv("XAUTHORITY") && path_exists("/home/pi/.Xauthority")) {
                setenv("XAUTHORITY", "/home/pi/.Xauthority", 1);
            }
            ok = true;
        } else if (getenv("DISPLAY")) {
            ok = true;
        }

        return ok;
    }

    static void prepare_child_process()
    {
        setsid();

        int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            if (devnull > STDERR_FILENO) close(devnull);
        }

        int log = open("/tmp/m5streamplayer-player.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (log >= 0) {
            dup2(log, STDOUT_FILENO);
            dup2(log, STDERR_FILENO);
            if (log > STDERR_FILENO) close(log);
        }

        long max_fd = sysconf(_SC_OPEN_MAX);
        if (max_fd < 0 || max_fd > 1024) max_fd = 1024;
        for (int fd = STDERR_FILENO + 1; fd < max_fd; ++fd) close(fd);
    }

    bool launch_desktop_player(const std::string &url, const std::string &token,
                               std::string &error, bool keep_open, bool video_mode,
                               long long start_seconds)
    {
        app_log("HdmiPlayer desktop dispatch mpv=" + bool_text(command_exists("mpv")) +
                " ffplay=" + bool_text(command_exists("ffplay")) +
                " cvlc=" + bool_text(command_exists("cvlc")) +
                " vlc=" + bool_text(command_exists("vlc")));
        if (command_exists("mpv")) return launch_mpv(url, token, error, keep_open, start_seconds);
        if (command_exists("ffplay")) return launch_simple("ffplay", url, error, keep_open, video_mode, start_seconds);
        if (command_exists("cvlc")) return launch_simple("cvlc", url, error, keep_open, video_mode, start_seconds);
        if (command_exists("vlc")) return launch_simple("vlc", url, error, keep_open, video_mode, start_seconds);
        error = "No desktop-capable player found.";
        app_log("HdmiPlayer desktop failed no player");
        return false;
    }

    bool launch_mpv(const std::string &url, const std::string &token, std::string &error,
                    bool keep_open, long long start_seconds)
    {
        unlink(kMpvSocket);
        pid_ = fork();
        if (pid_ < 0) {
            error = "fork failed.";
            app_log("HdmiPlayer mpv fork failed");
            return false;
        }
        if (pid_ == 0) {
            prepare_child_process();
            setup_desktop_environment();
            setup_audio_output_environment(audio_output_);
            std::string header = "X-Emby-Token: " + token;
            std::vector<std::string> args = {
                "mpv", "--fs", "--no-terminal",
                keep_open ? "--keep-open=yes" : "--keep-open=no",
                "--really-quiet", "--audio-display=no",
                "--input-ipc-server=/tmp/m5cardputer-streamplayer-mpv.sock",
                "--http-header-fields=" + header,
            };
            if (keep_open) args.push_back("--image-display-duration=inf");
            if (start_seconds > 0) args.push_back("--start=" + seconds_arg(start_seconds));
            args.push_back(url);
            exec_args(args);
            _exit(127);
        }
        backend_ = "mpv";
        last_start_offset_supported_ = true;
        app_log("HdmiPlayer mpv started pid=" + std::to_string(static_cast<long long>(pid_)) +
                " keep_open=" + bool_text(keep_open) +
                " start_seconds=" + std::to_string(start_seconds));
        return true;
    }

    bool launch_ffmpeg_fbdev(const std::string &url, const std::string &token,
                             std::string &error, long long start_seconds)
    {
        pid_ = fork();
        if (pid_ < 0) {
            error = "fork failed.";
            app_log("HdmiPlayer ffmpeg-fbdev fork failed");
            return false;
        }
        if (pid_ == 0) {
            prepare_child_process();
            setup_audio_output_environment(audio_output_);
            std::string header = "X-Emby-Token: " + token + "\r\n";
            const char *vf = "scale=2048:1080:force_original_aspect_ratio=decrease,"
                             "pad=2048:1080:(ow-iw)/2:(oh-ih)/2,format=rgb565le";
            std::vector<std::string> args = {
                "ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "warning", "-re",
            };
            if (!token.empty()) {
                args.push_back("-headers");
                args.push_back(header);
            }
            if (start_seconds > 0) {
                args.push_back("-ss");
                args.push_back(seconds_arg(start_seconds));
            }
            args.push_back("-i");
            args.push_back(url);
            args.insert(args.end(), {"-an", "-vf", vf, "-pix_fmt", "rgb565le", "-f", "fbdev", "/dev/fb0"});
            exec_args(args);
            _exit(127);
        }
        backend_ = "ffmpeg-fbdev";
        last_start_offset_supported_ = true;
        app_log("HdmiPlayer ffmpeg-fbdev started pid=" +
                std::to_string(static_cast<long long>(pid_)) +
                " start_seconds=" + std::to_string(start_seconds));
        return true;
    }

    bool launch_ffmpeg_small_fbdev(const std::string &url, const std::string &token,
                                   std::string &error, long long start_seconds,
                                   bool video_mode)
    {
        if (!video_mode) {
            error = "小屏播放仅用于视频。";
            return false;
        }
        pid_ = fork();
        if (pid_ < 0) {
            error = "fork failed.";
            app_log("HdmiPlayer small-fbdev fork failed");
            return false;
        }
        if (pid_ == 0) {
            prepare_child_process();
            setup_audio_output_environment(audio_output_);
            std::string header = "X-Emby-Token: " + token + "\r\n";
            std::string fbdev = small_lcd_fbdev_path();
            const char *vf = "scale=320:170:force_original_aspect_ratio=decrease,"
                             "pad=320:170:(ow-iw)/2:(oh-ih)/2,format=rgb565le";
            std::vector<std::string> args = {
                "ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "warning", "-re",
            };
            if (!token.empty()) {
                args.push_back("-headers");
                args.push_back(header);
            }
            if (start_seconds > 0) {
                args.push_back("-ss");
                args.push_back(seconds_arg(start_seconds));
            }
            args.push_back("-i");
            args.push_back(url);
            args.insert(args.end(), {
                "-map", "0:v:0", "-vf", vf, "-pix_fmt", "rgb565le", "-f", "fbdev", fbdev,
                "-map", "0:a:0", "-ac", "2", "-ar", "48000", "-f", "alsa", local_alsa_output_device()
            });
            exec_args(args);
            _exit(127);
        }
        backend_ = "small-fbdev";
        last_start_offset_supported_ = true;
        app_log("HdmiPlayer small-fbdev started pid=" +
                std::to_string(static_cast<long long>(pid_)) +
                " fbdev=" + small_lcd_fbdev_path() +
                " start_seconds=" + std::to_string(start_seconds));
        return true;
    }

    bool launch_simple(const char *cmd, const std::string &url, std::string &error,
                       bool keep_open, bool video_mode, long long start_seconds)
    {
        pid_ = fork();
        if (pid_ < 0) {
            error = "fork failed.";
            app_log(std::string("HdmiPlayer ") + cmd + " fork failed");
            return false;
        }
        if (pid_ == 0) {
            prepare_child_process();
            setup_desktop_environment();
            setup_audio_output_environment(audio_output_);
            if (strcmp(cmd, "ffplay") == 0) {
                std::vector<std::string> args = {"ffplay", "-fs"};
                if (video_mode && !keep_open) {
                    args.insert(args.end(), {"-autoexit", "-loglevel", "warning",
                                             "-framedrop", "-probesize", "1048576",
                                             "-analyzeduration", "2000000",
                                             "-codec:v", "h264_v4l2m2m"});
                } else if (keep_open) {
                    args.insert(args.end(), {"-loop", "0", "-loglevel", "warning"});
                } else {
                    args.insert(args.end(), {"-autoexit", "-loglevel", "warning"});
                }
                if (start_seconds > 0) {
                    args.push_back("-ss");
                    args.push_back(seconds_arg(start_seconds));
                }
                args.insert(args.end(), {"-window_title", "M5 StreamPlayer", url});
                exec_args(args);
            } else if (strcmp(cmd, "cvlc") == 0) {
                std::vector<std::string> args = {
                    "cvlc", "-I", "dummy", "--no-video-title-show", "--no-osd",
                    "--play-and-exit", "--fullscreen",
                };
                if (start_seconds > 0) args.push_back("--start-time=" + seconds_arg(start_seconds));
                args.push_back(url);
                exec_args(args);
            } else {
                std::vector<std::string> args = {cmd, "--fullscreen"};
                if (strcmp(cmd, "vlc") == 0 && start_seconds > 0) {
                    args.push_back("--start-time=" + seconds_arg(start_seconds));
                }
                args.push_back(url);
                exec_args(args);
            }
            _exit(127);
        }
        backend_ = cmd;
        last_start_offset_supported_ = start_seconds <= 0 ||
            strcmp(cmd, "ffplay") == 0 || strcmp(cmd, "cvlc") == 0 || strcmp(cmd, "vlc") == 0;
        app_log("HdmiPlayer simple started backend=" + backend_ +
                " pid=" + std::to_string(static_cast<long long>(pid_)) +
                " keep_open=" + bool_text(keep_open) +
                " video_mode=" + bool_text(video_mode) +
                " start_seconds=" + std::to_string(start_seconds));
        return true;
    }

    bool mpv_command(const std::string &json)
    {
        if (backend_ != "mpv") return false;
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) return false;
        sockaddr_un addr {};
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, kMpvSocket, sizeof(addr.sun_path) - 1);
        bool ok = connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0;
        if (ok) ok = write(fd, json.c_str(), json.size()) == static_cast<ssize_t>(json.size());
        close(fd);
        return ok;
    }

    pid_t pid_ = -1;
    std::string backend_;
    bool paused_ = false;
    AudioOutput audio_output_ = AudioOutput::Speaker;
    bool last_start_offset_supported_ = true;
};

class MediaController {
public:
    void init()
    {
        app_log("MediaController init pid=" + std::to_string(static_cast<long long>(getpid())) +
                " version=" + kAppVersion +
                " config_path=" + config_path());
        init_runtime_fonts();
        config_loaded_ = load_config(config_);
        if (config_.base_url.empty()) config_.base_url = kDefaultServerUrl;
        if (config_.language.empty()) config_.language = language_code(language_from_code(system_language_code()));
        app_log("Config loaded=" + bool_text(config_loaded_) +
                " server=" + server_name(config_.type) +
                " language=" + config_.language +
                " base_url=" + config_.base_url +
                " user=" + config_.username +
                " token=" + bool_text(!config_.access_token.empty()) +
                " user_id=" + config_.user_id);
        const char *audio_env = getenv("M5_STREAMPLAYER_AUDIO_OUTPUT");
        if (audio_env && *audio_env) {
            std::string value = to_lower(audio_env);
            if (value == "local" || value == "analog" || value == "speaker") {
                audio_output_ = AudioOutput::Speaker;
            } else if (value == "headphone" || value == "3.5" || value == "35mm") {
                audio_output_ = AudioOutput::Headphone;
            } else if (value == "bluetooth" || value == "bt" || value == "bluez") {
                audio_output_ = audio_output_available(AudioOutput::Bluetooth)
                    ? AudioOutput::Bluetooth
                    : AudioOutput::Speaker;
            } else if (value == "hdmi") {
                audio_output_ = AudioOutput::Hdmi;
            }
        }
        app_log(std::string("Audio output initial=") + audio_output_label(audio_output_));
        client_ = new MediaServerClient(config_);
        prepare_fullscreen_shell();
        root_ = ui_apppage ? ui_apppage : lv_screen_active();
        lv_obj_clean(root_);
        lv_obj_add_event_cb(root_, key_event_cb, LV_EVENT_KEY, this);
        lv_obj_add_flag(root_, LV_OBJ_FLAG_CLICKABLE);
        attach_keyboard(nullptr);
        setup_ssh_control();
        if (config_loaded_) load_after_token();
        else if (has_configured_password()) login_with_password(config_.password);
        else show_setup();
    }

    void attach_keyboard(lv_indev_t *preferred)
    {
        if (!group_) group_ = lv_group_create();
        lv_group_set_default(group_);
        if (preferred) {
            lv_indev_set_group(preferred, group_);
        }
        for (lv_indev_t *indev = lv_indev_get_next(NULL); indev; indev = lv_indev_get_next(indev)) {
            if (lv_indev_get_type(indev) == LV_INDEV_TYPE_KEYPAD ||
                lv_indev_get_type(indev) == LV_INDEV_TYPE_ENCODER) {
                lv_indev_set_group(indev, group_);
            }
        }
    }

private:
    UiLanguage current_language() const
    {
        return language_from_code(config_.language.empty() ? system_language_code() : config_.language);
    }

    const char *t(TextKey key) const
    {
        return ui_text(current_language(), key);
    }

    void prepare_fullscreen_shell()
    {
        lv_obj_t *chrome[] = {ui_logo, ui_time, ui_power, ui_appname};
        for (size_t i = 0; i < sizeof(chrome) / sizeof(chrome[0]); ++i) {
            if (chrome[i]) lv_obj_add_flag(chrome[i], LV_OBJ_FLAG_HIDDEN);
        }
        if (ui_apppage) {
            lv_obj_set_pos(ui_apppage, 0, 0);
            lv_obj_set_size(ui_apppage, kScreenW, kScreenH);
            lv_obj_set_align(ui_apppage, LV_ALIGN_TOP_LEFT);
        }
    }

    static void key_event_cb(lv_event_t *event)
    {
        auto *self = static_cast<MediaController *>(lv_event_get_user_data(event));
        if (self) self->handle_key(lv_event_get_key(event));
    }

    static void click_event_cb(lv_event_t *event)
    {
        auto *self = static_cast<MediaController *>(lv_event_get_user_data(event));
        if (!self) return;
        lv_obj_t *target = static_cast<lv_obj_t *>(lv_event_get_target(event));
        if (target == self->login_btn_) self->login_from_setup();
        else if (target == self->logout_btn_) self->logout();
        else if (target == self->small_screen_btn_ ||
                 (target && lv_obj_get_parent(target) == self->small_screen_btn_)) {
            self->play_pending_on_small_screen();
        }
    }

    static void setup_dropdown_event_cb(lv_event_t *event)
    {
        auto *self = static_cast<MediaController *>(lv_event_get_user_data(event));
        if (!self) return;
        lv_obj_t *target = static_cast<lv_obj_t *>(lv_event_get_target(event));
        if (target == self->server_dropdown_) {
            self->config_.type = lv_dropdown_get_selected(target) == 1
                ? ServerType::Emby
                : ServerType::Jellyfin;
            save_config(self->config_);
            self->show_setup();
        } else if (target == self->language_dropdown_) {
            self->config_.language = lv_dropdown_get_selected(target) == 1 ? "zh-CN" : "en";
            save_config(self->config_);
            self->show_setup();
        }
    }

    static void focus_event_cb(lv_event_t *event)
    {
        auto *self = static_cast<MediaController *>(lv_event_get_user_data(event));
        if (self) self->update_setup_focus_marker();
    }

    static void player_timer_cb(lv_timer_t *timer)
    {
        auto *self = static_cast<MediaController *>(lv_timer_get_user_data(timer));
        if (self) self->update_player_progress();
    }

    static void control_timer_cb(lv_timer_t *timer)
    {
        auto *self = static_cast<MediaController *>(lv_timer_get_user_data(timer));
        if (self) self->poll_ssh_control();
    }

    static void esc_short_timer_cb(lv_timer_t *timer)
    {
        auto *self = static_cast<MediaController *>(lv_timer_get_user_data(timer));
        if (self) {
            self->esc_short_timer_ = nullptr;
            self->esc_hold_started_ms_ = 0;
            self->last_esc_key_ms_ = 0;
            self->esc_exit_armed_ = false;
            self->perform_escape_action();
        }
        lv_timer_delete(timer);
    }

    static uint32_t normalize_direction_alias(uint32_t key)
    {
        switch (key) {
        case STREAMPLAYER_KEY_SUBTITLE:
            return LV_KEY_DOWN;
        case 'f':
        case 'F':
            return LV_KEY_UP;
        case 'x':
        case 'X':
            return LV_KEY_DOWN;
        case 'z':
        case 'Z':
            return LV_KEY_LEFT;
        case 'c':
        case 'C':
            return LV_KEY_RIGHT;
        default:
            return key;
        }
    }

    static bool is_subtitle_shortcut(uint32_t key)
    {
        return key == STREAMPLAYER_KEY_SUBTITLE || key == 'x' || key == 'X';
    }

    void handle_key(uint32_t key)
    {
        uint32_t raw_key = key;
        if (handle_long_escape(key)) return;
        if (view_ == View::Browse && is_subtitle_shortcut(raw_key)) {
            open_search();
            return;
        }
        key = normalize_direction_alias(key);
        if (view_ == View::Setup) handle_setup_key(key);
        else if (view_ == View::Browse) handle_browse_key(key);
        else if (view_ == View::Detail) handle_detail_key(key);
        else if (view_ == View::Player) handle_player_key(key);
        else if (view_ == View::Sort) handle_sort_key(key);
        else if (view_ == View::Settings) handle_settings_key(key);
        else if (view_ == View::Search) handle_search_key(raw_key, key);
        else if (view_ == View::Error) {
            if (key == LV_KEY_ENTER && has_pending_small_screen_item_) play_pending_on_small_screen();
            else if (key == LV_KEY_ESC || key == LV_KEY_ENTER) {
                if (has_pending_small_screen_item_) cancel_pending_small_screen_prompt();
                render_browse();
            }
        }
    }

    bool handle_long_escape(uint32_t key)
    {
        uint64_t now = monotonic_ms();
        if (key != LV_KEY_ESC) {
            esc_hold_started_ms_ = 0;
            last_esc_key_ms_ = 0;
            return false;
        }

        if (esc_hold_started_ms_ == 0 || now - last_esc_key_ms_ > 700) {
            esc_hold_started_ms_ = now;
            esc_exit_armed_ = true;
            if (esc_short_timer_) lv_timer_delete(esc_short_timer_);
            esc_short_timer_ = lv_timer_create(esc_short_timer_cb, 650, this);
            lv_timer_set_repeat_count(esc_short_timer_, 1);
        } else if (esc_short_timer_) {
            lv_timer_delete(esc_short_timer_);
            esc_short_timer_ = nullptr;
        }
        last_esc_key_ms_ = now;

        if (esc_exit_armed_ && now - esc_hold_started_ms_ >= 900) {
            esc_exit_armed_ = false;
            if (esc_short_timer_) {
                lv_timer_delete(esc_short_timer_);
                esc_short_timer_ = nullptr;
            }
            if (view_ == View::Player) stop_playback_to_detail();
            else exit_application("long-esc");
            return true;
        }
        return true;
    }

    void stop_playback_to_detail()
    {
        report_playback_stopped();
        player_.stop();
        small_screen_playback_ = false;
        playback_paused_ = false;
        render_detail();
    }

    void cancel_pending_small_screen_prompt()
    {
        has_pending_small_screen_item_ = false;
        small_screen_prompt_ready_at_ms_ = 0;
        pending_small_screen_item_ = MediaItem();
    }

    void perform_escape_action()
    {
        if (view_ == View::Browse) {
            LibraryState *state = current_library_state();
            if (state && state->expanded) {
                reset_expansion(*state);
                last_vertical_dir_ = 0;
                last_horizontal_dir_ = 0;
                render_browse();
            }
        } else if (view_ == View::Detail || view_ == View::Sort || view_ == View::Search ||
                   view_ == View::Settings || view_ == View::Error) {
            if (view_ == View::Error && has_pending_small_screen_item_) cancel_pending_small_screen_prompt();
            render_browse();
        } else if (view_ == View::Player) {
            stop_playback_to_detail();
        }
    }

    void exit_application(const char *reason)
    {
        app_log(std::string("Application exit reason=") + (reason ? reason : "<unknown>"));
        report_playback_stopped();
        player_.stop();
        if (esc_short_timer_) {
            lv_timer_delete(esc_short_timer_);
            esc_short_timer_ = nullptr;
        }
        if (control_timer_) {
            lv_timer_delete(control_timer_);
            control_timer_ = nullptr;
        }
        if (control_fd_ >= 0) {
            close(control_fd_);
            control_fd_ = -1;
        }
        if (control_keepalive_fd_ >= 0) {
            close(control_keepalive_fd_);
            control_keepalive_fd_ = -1;
        }
        std::exit(0);
    }

    uint32_t control_command_to_key(std::string command) const
    {
        command = to_lower(trim(command));
        if (command.empty()) return 0;
        if (command == "up") return LV_KEY_UP;
        if (command == "down") return LV_KEY_DOWN;
        if (command == "left") return LV_KEY_LEFT;
        if (command == "right") return LV_KEY_RIGHT;
        if (command == "enter" || command == "return") return LV_KEY_ENTER;
        if (command == "space") return ' ';
        if (command == "esc" || command == "escape") return LV_KEY_ESC;
        if (command == "tab") return LV_KEY_NEXT;
        if (command == "backtab") return LV_KEY_PREV;
        if (command == "audio") return LV_KEY_UP;
        if (command == "subtitle") return STREAMPLAYER_KEY_SUBTITLE;
        if (command == "ctrlleft" || command == "ctrl-left" || command == "ctrl_left") return STREAMPLAYER_KEY_CTRL_LEFT;
        if (command == "ctrlright" || command == "ctrl-right" || command == "ctrl_right") return STREAMPLAYER_KEY_CTRL_RIGHT;
        if (command == "output") return 'o';
        if (command.size() == 1) return static_cast<unsigned char>(command[0]);
        return 0;
    }

    void handle_control_command(const std::string &command)
    {
        uint32_t key = control_command_to_key(command);
        if (!key) {
            app_log("SSH control ignored command=" + command);
            return;
        }
        app_log("SSH control command=" + command + " key=" + std::to_string(key));
        handle_key(key);
    }

    void poll_ssh_control()
    {
        if (control_fd_ < 0) return;
        char buf[128];
        for (;;) {
            ssize_t n = read(control_fd_, buf, sizeof(buf));
            if (n > 0) {
                control_buffer_.append(buf, static_cast<size_t>(n));
                continue;
            }
            if (n == 0 || errno == EAGAIN || errno == EWOULDBLOCK) break;
            app_log("SSH control read failed errno=" + std::to_string(errno));
            break;
        }

        size_t pos = 0;
        while ((pos = control_buffer_.find('\n')) != std::string::npos) {
            std::string command = control_buffer_.substr(0, pos);
            control_buffer_.erase(0, pos + 1);
            handle_control_command(command);
        }
        if (control_buffer_.size() > 512) control_buffer_.erase(0, control_buffer_.size() - 512);
    }

    void setup_ssh_control()
    {
        struct stat st {};
        if (stat(kControlPipe, &st) == 0 && !S_ISFIFO(st.st_mode)) {
            unlink(kControlPipe);
        }
        if (mkfifo(kControlPipe, 0666) != 0 && errno != EEXIST) {
            app_log(std::string("SSH control mkfifo failed path=") + kControlPipe +
                    " errno=" + std::to_string(errno));
            return;
        }
        chmod(kControlPipe, 0666);
        control_fd_ = open(kControlPipe, O_RDONLY | O_NONBLOCK);
        if (control_fd_ < 0) {
            app_log(std::string("SSH control open read failed path=") + kControlPipe +
                    " errno=" + std::to_string(errno));
            return;
        }
        control_keepalive_fd_ = open(kControlPipe, O_WRONLY | O_NONBLOCK);
        if (control_keepalive_fd_ < 0) {
            app_log("SSH control keepalive writer unavailable errno=" + std::to_string(errno));
        }
        control_timer_ = lv_timer_create(control_timer_cb, 50, this);
        app_log(std::string("SSH control ready pipe=") + kControlPipe);
    }

    void reset_group()
    {
        if (!group_) group_ = lv_group_create();
        lv_group_remove_all_objs(group_);
        attach_keyboard(nullptr);
    }

    void add_to_group(lv_obj_t *obj)
    {
        if (group_ && obj) lv_group_add_obj(group_, obj);
    }

    lv_obj_t *focused_obj() const
    {
        return group_ ? lv_group_get_focused(group_) : nullptr;
    }

    bool has_configured_password() const
    {
        return !config_.base_url.empty() && !config_.username.empty() && !config_.password.empty();
    }

    void stop_player_timer()
    {
        if (player_timer_) {
            lv_timer_delete(player_timer_);
            player_timer_ = nullptr;
        }
        player_time_label_ = nullptr;
        player_status_label_ = nullptr;
        player_progress_fill_ = nullptr;
        player_progress_w_ = 0;
    }

    void clear(lv_color_t bg = lv_color_hex(0x05070A))
    {
        stop_player_timer();
        lv_obj_clean(root_);
        lv_obj_set_style_bg_color(root_, bg, 0);
        lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, 0);
        small_screen_btn_ = nullptr;
        server_dropdown_ = nullptr;
        language_dropdown_ = nullptr;
        reset_group();
    }

    lv_obj_t *label(lv_obj_t *parent, const char *text, int x, int y, int w, int h,
                    lv_color_t color, const lv_font_t *font = nullptr)
    {
        if (!font) font = ui_font_14();
        lv_obj_t *obj = lv_label_create(parent);
        lv_obj_set_pos(obj, x, y);
        lv_obj_set_size(obj, w, h);
        std::string clean = single_line_text(text);
        lv_label_set_text(obj, clean.c_str());
        lv_label_set_long_mode(obj, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_color(obj, color, 0);
        lv_obj_set_style_text_font(obj, font, 0);
        return obj;
    }

    lv_obj_t *marquee_label(lv_obj_t *parent, const char *text, int x, int y, int w, int h,
                            lv_color_t color, const lv_font_t *font = nullptr,
                            uint16_t speed = 18)
    {
        if (!font) font = ui_font_14();
        lv_obj_t *obj = lv_label_create(parent);
        lv_obj_set_pos(obj, x, y);
        lv_obj_set_size(obj, w, h);
        std::string clean = single_line_text(text);
        lv_label_set_text(obj, clean.c_str());
        lv_label_set_long_mode(obj, LV_LABEL_LONG_DOT);
        lv_obj_set_style_anim_duration(obj, lv_anim_speed_clamped(speed, 2500, 10000), 0);
        lv_obj_set_style_text_color(obj, color, 0);
        lv_obj_set_style_text_font(obj, font, 0);
        return obj;
    }

    void start_vertical_text_scroll(lv_obj_t *obj, int overflow)
    {
        if (!obj || overflow <= 4) return;
        lv_anim_t anim;
        lv_anim_init(&anim);
        lv_anim_set_var(&anim, obj);
        lv_anim_set_values(&anim, 0, -overflow);
        lv_anim_set_exec_cb(&anim, anim_set_y);
        lv_anim_set_path_cb(&anim, lv_anim_path_linear);
        lv_anim_set_repeat_count(&anim, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_repeat_delay(&anim, 1200);
        lv_anim_set_playback_delay(&anim, 1200);
        lv_anim_set_time(&anim, static_cast<uint32_t>(std::max(9000, overflow * 170)));
        lv_anim_set_playback_time(&anim, static_cast<uint32_t>(std::max(9000, overflow * 170)));
        lv_anim_start(&anim);
    }

    lv_obj_t *box(lv_obj_t *parent, int x, int y, int w, int h, lv_color_t bg, lv_color_t border)
    {
        lv_obj_t *obj = lv_obj_create(parent);
        lv_obj_set_pos(obj, x, y);
        lv_obj_set_size(obj, w, h);
        lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_radius(obj, 5, 0);
        lv_obj_set_style_bg_color(obj, bg, 0);
        lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(obj, 1, 0);
        lv_obj_set_style_border_color(obj, border, 0);
        lv_obj_set_style_pad_all(obj, 3, 0);
        return obj;
    }

    lv_obj_t *button(lv_obj_t *parent, const char *text, int x, int y, int w, int h)
    {
        lv_obj_t *obj = box(parent, x, y, w, h, lv_color_hex(0x101820), accent_color(config_.type));
        lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(obj, click_event_cb, LV_EVENT_CLICKED, this);
        label(obj, text, 2, 4, w - 4, h - 6, lv_color_hex(0xFFFFFF));
        add_to_group(obj);
        return obj;
    }

    lv_obj_t *text_button(lv_obj_t *parent, const char *text, int x, int y, int w, int h, lv_color_t color)
    {
        lv_obj_t *obj = lv_label_create(parent);
        lv_obj_set_pos(obj, x, y);
        lv_obj_set_size(obj, w, h);
        lv_label_set_text(obj, text);
        lv_label_set_long_mode(obj, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_color(obj, color, 0);
        lv_obj_set_style_text_font(obj, ui_font_16(), 0);
        lv_obj_set_style_outline_width(obj, 0, 0);
        lv_obj_set_style_bg_color(obj, lv_color_hex(0x0D1720), LV_STATE_FOCUSED);
        lv_obj_set_style_bg_opa(obj, LV_OPA_60, LV_STATE_FOCUSED);
        lv_obj_set_style_text_color(obj, accent_color(config_.type), LV_STATE_FOCUSED);
        lv_obj_set_style_radius(obj, 3, LV_STATE_FOCUSED);
        lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(obj, click_event_cb, LV_EVENT_CLICKED, this);
        lv_obj_add_event_cb(obj, focus_event_cb, LV_EVENT_FOCUSED, this);
        lv_obj_add_event_cb(obj, focus_event_cb, LV_EVENT_DEFOCUSED, this);
        add_to_group(obj);
        return obj;
    }

    lv_obj_t *dropdown(lv_obj_t *parent, const char *options, uint16_t selected,
                       int x, int y, int w, int h)
    {
        lv_obj_t *obj = lv_dropdown_create(parent);
        lv_obj_set_pos(obj, x, y);
        lv_obj_set_size(obj, w, h);
        lv_dropdown_set_options(obj, options);
        lv_dropdown_set_selected(obj, selected);
        lv_obj_set_style_radius(obj, 4, 0);
        lv_obj_set_style_border_width(obj, 1, 0);
        lv_obj_set_style_border_color(obj, accent_color(config_.type), 0);
        lv_obj_set_style_outline_width(obj, 0, 0);
        lv_obj_set_style_bg_color(obj, lv_color_hex(0x0C131B), 0);
        lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(obj, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_font(obj, ui_font_14(), 0);
        lv_obj_set_style_pad_left(obj, 6, 0);
        lv_obj_set_style_pad_top(obj, 1, 0);
        lv_obj_set_style_bg_color(obj, lv_color_hex(0x101820), LV_STATE_FOCUSED);
        lv_obj_set_style_text_color(obj, accent_color(config_.type), LV_STATE_FOCUSED);
        lv_obj_add_event_cb(obj, setup_dropdown_event_cb, LV_EVENT_VALUE_CHANGED, this);
        lv_obj_add_event_cb(obj, focus_event_cb, LV_EVENT_FOCUSED, this);
        lv_obj_add_event_cb(obj, focus_event_cb, LV_EVENT_DEFOCUSED, this);
        add_to_group(obj);
        return obj;
    }

    void rule(lv_obj_t *parent, int x, int y, int w, lv_color_t color)
    {
        lv_obj_t *line = lv_obj_create(parent);
        lv_obj_set_pos(line, x, y);
        lv_obj_set_size(line, w, 1);
        lv_obj_clear_flag(line, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_border_width(line, 0, 0);
        lv_obj_set_style_radius(line, 0, 0);
        lv_obj_set_style_bg_color(line, color, 0);
        lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_all(line, 0, 0);
    }

    lv_obj_t *textarea(lv_obj_t *parent, const char *placeholder, int x, int y, int w, int h, bool password)
    {
        lv_obj_t *ta = lv_textarea_create(parent);
        lv_obj_set_pos(ta, x, y);
        lv_obj_set_size(ta, w, h);
        lv_textarea_set_one_line(ta, true);
        lv_textarea_set_placeholder_text(ta, placeholder);
        lv_textarea_set_password_mode(ta, password);
        lv_obj_set_style_radius(ta, 0, 0);
        lv_obj_set_style_border_width(ta, 0, 0);
        lv_obj_set_style_outline_width(ta, 0, 0);
        lv_obj_set_style_bg_opa(ta, LV_OPA_TRANSP, 0);
        lv_obj_set_style_text_color(ta, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_font(ta, ui_font_14(), 0);
        lv_obj_set_style_bg_color(ta, lv_color_hex(0x0C131B), LV_STATE_FOCUSED);
        lv_obj_set_style_bg_opa(ta, LV_OPA_70, LV_STATE_FOCUSED);
        lv_obj_set_style_text_color(ta, accent_color(config_.type), LV_STATE_FOCUSED);
        lv_obj_set_style_bg_color(ta, accent_color(config_.type), LV_PART_CURSOR);
        lv_obj_set_style_width(ta, 2, LV_PART_CURSOR);
        lv_obj_set_style_pad_all(ta, 0, 0);
        lv_obj_set_style_pad_top(ta, 2, 0);
        lv_obj_add_event_cb(ta, focus_event_cb, LV_EVENT_FOCUSED, this);
        lv_obj_add_event_cb(ta, focus_event_cb, LV_EVENT_DEFOCUSED, this);
        add_to_group(ta);
        return ta;
    }

    void update_setup_focus_marker()
    {
        if (view_ != View::Setup || !setup_focus_marker_) return;

        lv_obj_t *focused = focused_obj();
        int y = -1;
        int h = 18;
        if (focused == server_dropdown_ || focused == language_dropdown_) {
            y = 16;
            h = 18;
        } else if (focused == url_ta_) {
            y = 41;
            h = 22;
        } else if (focused == user_ta_) {
            y = 73;
            h = 22;
        } else if (focused == pass_ta_) {
            y = 105;
            h = 22;
        } else if (focused == login_btn_) {
            y = 143;
            h = 18;
        }

        if (y < 0) {
            lv_obj_add_flag(setup_focus_marker_, LV_OBJ_FLAG_HIDDEN);
            return;
        }

        lv_obj_clear_flag(setup_focus_marker_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_y(setup_focus_marker_, y);
        lv_obj_set_height(setup_focus_marker_, h);
        lv_obj_set_style_bg_color(setup_focus_marker_, accent_color(config_.type), 0);
    }

    void set_status(const std::string &text)
    {
        if (status_) lv_label_set_text(status_, text.c_str());
    }

    void show_setup()
    {
        view_ = View::Setup;
        clear();
        setup_focus_marker_ = lv_obj_create(root_);
        lv_obj_set_pos(setup_focus_marker_, 8, 16);
        lv_obj_set_size(setup_focus_marker_, 3, 18);
        lv_obj_clear_flag(setup_focus_marker_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_border_width(setup_focus_marker_, 0, 0);
        lv_obj_set_style_radius(setup_focus_marker_, 2, 0);
        lv_obj_set_style_bg_color(setup_focus_marker_, accent_color(config_.type), 0);
        lv_obj_set_style_bg_opa(setup_focus_marker_, LV_OPA_COVER, 0);

        label(root_, t(TextKey::Server), 18, 18, 42, 16, lv_color_hex(0x7F8D9D));
        server_dropdown_ = dropdown(root_, "Jellyfin\nEmby",
                                    config_.type == ServerType::Emby ? 1 : 0,
                                    62, 14, 92, 24);
        label(root_, t(TextKey::Language), 166, 18, 42, 16, lv_color_hex(0x7F8D9D));
        language_dropdown_ = dropdown(root_, "English\n中文",
                                      current_language() == UiLanguage::Chinese ? 1 : 0,
                                      208, 14, 94, 24);

        label(root_, "URL", 18, 44, 54, 18, lv_color_hex(0x7F8D9D));
        url_ta_ = textarea(root_, kServerUrlPlaceholder, 78, 40, 224, 24, false);
        rule(root_, 78, 64, 224, lv_color_hex(0x263341));

        label(root_, t(TextKey::Account), 18, 76, 54, 18, lv_color_hex(0x7F8D9D));
        user_ta_ = textarea(root_, "username", 78, 72, 224, 24, false);
        rule(root_, 78, 96, 224, lv_color_hex(0x263341));

        label(root_, t(TextKey::Password), 18, 108, 54, 18, lv_color_hex(0x7F8D9D));
        pass_ta_ = textarea(root_, "password", 78, 104, 224, 24, true);
        rule(root_, 78, 128, 224, lv_color_hex(0x263341));

        login_btn_ = text_button(root_, t(TextKey::Login), 18, 142, 80, 22, lv_color_hex(0xFFFFFF));
        status_ = label(root_, "", 106, 145, 196, 18, lv_color_hex(0xFF9B9B));
        if (!config_.base_url.empty()) lv_textarea_set_text(url_ta_, config_.base_url.c_str());
        if (!config_.username.empty()) lv_textarea_set_text(user_ta_, config_.username.c_str());
        if (!config_.password.empty()) lv_textarea_set_text(pass_ta_, config_.password.c_str());
        lv_group_focus_obj(server_dropdown_);
        update_setup_focus_marker();
    }

    void show_loading(const std::string &text)
    {
        view_ = View::Loading;
        clear();
        label(root_, server_name(config_.type), 10, 18, 120, 18, accent_color(config_.type));
        if (should_marquee(text, 300, 14)) {
            marquee_label(root_, text.c_str(), 10, 50, 300, 22, lv_color_hex(0xFFFFFF), ui_font_14(), 13);
        } else {
            label(root_, text.c_str(), 10, 50, 300, 22, lv_color_hex(0xFFFFFF));
        }

        lv_obj_t *track = lv_obj_create(root_);
        lv_obj_remove_style_all(track);
        lv_obj_set_pos(track, 22, 88);
        lv_obj_set_size(track, 276, 6);
        lv_obj_clear_flag(track, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_radius(track, 3, 0);
        lv_obj_set_style_bg_color(track, lv_color_hex(0x17212C), 0);
        lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);

        lv_obj_t *pulse = lv_obj_create(track);
        lv_obj_remove_style_all(pulse);
        lv_obj_set_pos(pulse, 0, 0);
        lv_obj_set_size(pulse, 56, 6);
        lv_obj_set_style_radius(pulse, 3, 0);
        lv_obj_set_style_bg_color(pulse, accent_color(config_.type), 0);
        lv_obj_set_style_bg_opa(pulse, LV_OPA_COVER, 0);
        lv_anim_t anim;
        lv_anim_init(&anim);
        lv_anim_set_var(&anim, pulse);
        lv_anim_set_values(&anim, 0, 220);
        lv_anim_set_exec_cb(&anim, anim_set_x);
        lv_anim_set_path_cb(&anim, lv_anim_path_ease_in_out);
        lv_anim_set_time(&anim, 1150);
        lv_anim_set_playback_time(&anim, 1150);
        lv_anim_set_repeat_count(&anim, LV_ANIM_REPEAT_INFINITE);
        lv_anim_start(&anim);

        label(root_, current_language() == UiLanguage::Chinese ? "正在准备小屏控制和 HDMI 输出"
                                                               : "Preparing small-screen controls and HDMI output",
              10, 112, 300, 18, lv_color_hex(0x7E8A99));
        lv_timer_handler();
    }

    void show_error(const std::string &text)
    {
        app_log("UI error view=" + std::to_string(static_cast<int>(view_)) +
                " message=" + text);
        has_pending_small_screen_item_ = false;
        small_screen_prompt_ready_at_ms_ = 0;
        view_ = View::Error;
        clear(lv_color_hex(0x120709));
        label(root_, t(TextKey::Error), 8, 12, 80, 20, lv_color_hex(0xFF6B6B), ui_font_16());
        label(root_, text.c_str(), 8, 40, 300, 55, lv_color_hex(0xFFFFFF));
        label(root_, t(TextKey::EnterEscBack), 8, 118, 300, 18, lv_color_hex(0xA8B3C1));
        add_to_group(root_);
        lv_group_focus_obj(root_);
    }

    bool video_item_needs_hdmi(const MediaItem &item) const
    {
        return !is_audio_item(item) && !is_photo_item(item);
    }

    void show_hdmi_required_prompt(const MediaItem &item)
    {
        pending_small_screen_item_ = item;
        has_pending_small_screen_item_ = true;
        small_screen_prompt_ready_at_ms_ = monotonic_ms() + 600;
        app_log("HDMI missing prompt item=" + item.id + " name=" + item.name);
        view_ = View::Error;
        clear(lv_color_hex(0x05070A));
        label(root_, t(TextKey::HdmiTitle), 8, 10, 180, 20, accent_color(config_.type), ui_font_16());
        label(root_, t(TextKey::HdmiMessage), 8, 36, 304, 42, lv_color_hex(0xF2F6FA));
        label(root_, t(TextKey::SmallScreenAlso), 8, 79, 304, 18, lv_color_hex(0x91A1B3));

        small_screen_btn_ = lv_btn_create(root_);
        lv_obj_set_pos(small_screen_btn_, 40, 105);
        lv_obj_set_size(small_screen_btn_, 240, 34);
        lv_obj_set_style_radius(small_screen_btn_, 7, 0);
        lv_obj_set_style_bg_color(small_screen_btn_, accent_color(config_.type), 0);
        lv_obj_set_style_bg_opa(small_screen_btn_, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(small_screen_btn_, 0, 0);
        lv_obj_add_event_cb(small_screen_btn_, click_event_cb, LV_EVENT_CLICKED, this);
        lv_obj_t *btn_label = lv_label_create(small_screen_btn_);
        lv_label_set_text(btn_label, t(TextKey::SmallScreenButton));
        lv_obj_center(btn_label);
        lv_obj_set_style_text_font(btn_label, ui_font_14(), 0);
        lv_obj_set_style_text_color(btn_label, lv_color_hex(0x06100A), 0);
        label(root_, t(TextKey::EnterConfirmEscBack), 8, 146, 304, 18, lv_color_hex(0xA8B3C1));
        add_to_group(root_);
        add_to_group(small_screen_btn_);
        lv_group_focus_obj(root_);
    }

    void toggle_server_type()
    {
        config_.type = config_.type == ServerType::Emby ? ServerType::Jellyfin : ServerType::Emby;
        save_config(config_);
        show_setup();
    }

    void toggle_language()
    {
        config_.language = current_language() == UiLanguage::Chinese ? "en" : "zh-CN";
        save_config(config_);
        show_setup();
    }

    void login_with_password(const std::string &password)
    {
        while (!config_.base_url.empty() && config_.base_url.back() == '/') config_.base_url.pop_back();
        delete client_;
        client_ = new MediaServerClient(config_);
        show_loading(t(TextKey::Authenticating));
        std::string error;
        if (!client_->authenticate(password, error)) {
            show_setup();
            set_status(error);
            return;
        }
        config_ = client_->config();
        config_.password.clear();
        load_after_token();
    }

    void login_from_setup()
    {
        config_.base_url = trim(lv_textarea_get_text(url_ta_) ? lv_textarea_get_text(url_ta_) : "");
        config_.username = trim(lv_textarea_get_text(user_ta_) ? lv_textarea_get_text(user_ta_) : "");
        std::string password = lv_textarea_get_text(pass_ta_) ? lv_textarea_get_text(pass_ta_) : "";
        if (config_.base_url.empty() || config_.username.empty() || password.empty()) {
            set_status(t(TextKey::NeedCredentials));
            return;
        }
        login_with_password(password);
    }

    void load_after_token()
    {
        if (ui_appname) lv_obj_set_style_text_color(ui_appname, accent_color(config_.type), 0);
        if (getenv("M5_DEMO_MEDIA")) {
            load_demo_media();
            return;
        }
        show_loading(t(TextKey::LoadingLibraries));
        std::string error;
        if (!client_->load_libraries(libraries_, error)) {
            clear_config(&config_);
            show_setup();
            set_status(error);
            return;
        }
        library_index_ = 0;
        library_states_.assign(libraries_.size(), LibraryState());
        load_items_for_current_library(true);
    }

    LibraryState *current_library_state()
    {
        if (libraries_.empty() || library_index_ >= library_states_.size()) return nullptr;
        return &library_states_[library_index_];
    }

    const LibraryState *current_library_state() const
    {
        if (libraries_.empty() || library_index_ >= library_states_.size()) return nullptr;
        return &library_states_[library_index_];
    }

    bool append_items_for_library(size_t index, bool show_loader)
    {
        if (index >= libraries_.size() || index >= library_states_.size()) return false;
        LibraryState &state = library_states_[index];
        if (library_is_settings(libraries_[index])) {
            state.error.clear();
            state.exhausted = true;
            return true;
        }
        if (state.exhausted) return true;

        if (show_loader) show_loading(index == library_index_ ? t(TextKey::LoadingVideos)
                                                              : t(TextKey::PreloadingVideos));
        std::vector<MediaItem> batch;
        std::string error;
        if (!client_->load_items(libraries_[index], sort_, state.loaded, kItemsPageSize, batch, error)) {
            state.error = error;
            return false;
        }

        state.error.clear();
        state.loaded += static_cast<int>(batch.size());
        state.items.insert(state.items.end(), batch.begin(), batch.end());
        if (batch.size() < static_cast<size_t>(kItemsPageSize)) state.exhausted = true;
        if (!state.items.empty() && state.selected >= state.items.size()) {
            state.selected = state.items.size() - 1;
        }
        return true;
    }

    bool load_items_for_current_library(bool reset)
    {
        if (libraries_.empty()) return false;
        if (library_states_.size() != libraries_.size()) {
            library_states_.assign(libraries_.size(), LibraryState());
        }
        if (reset) library_states_[library_index_] = LibraryState();
        LibraryState &state = library_states_[library_index_];
        if (state.items.empty() && !state.exhausted) {
            if (!append_items_for_library(library_index_, true)) {
                show_error(state.error.empty() ? "视频加载失败。" : state.error);
                return false;
            }
        }
        render_browse();
        return true;
    }

    void reset_library_states()
    {
        library_states_.assign(libraries_.size(), LibraryState());
    }

    void maybe_lazyload_current_library()
    {
        LibraryState *state = current_library_state();
        if (!state || state->exhausted || state->items.empty()) return;
        if (state->selected + kLazyLoadThreshold < state->items.size()) return;
        append_items_for_library(library_index_, true);
    }

    void reset_search_results()
    {
        search_results_.clear();
        search_selected_ = 0;
        search_loaded_ = 0;
        search_exhausted_ = false;
        search_error_.clear();
        search_dirty_ = true;
    }

    bool append_search_results(bool show_loader)
    {
        std::string query = trim(search_query_);
        if (query.empty()) {
            reset_search_results();
            return true;
        }
        if (search_exhausted_) return true;
        if (show_loader) show_loading("搜索中...");

        std::vector<MediaItem> batch;
        std::string error;
        if (!client_->search_items(query, sort_, search_loaded_, kItemsPageSize, batch, error)) {
            search_error_ = error;
            return false;
        }

        search_error_.clear();
        search_dirty_ = false;
        search_loaded_ += static_cast<int>(batch.size());
        search_results_.insert(search_results_.end(), batch.begin(), batch.end());
        if (batch.size() < static_cast<size_t>(kItemsPageSize)) search_exhausted_ = true;
        if (!search_results_.empty() && search_selected_ >= search_results_.size()) {
            search_selected_ = search_results_.size() - 1;
        }
        return true;
    }

    bool run_search(bool show_loader)
    {
        search_results_.clear();
        search_selected_ = 0;
        search_loaded_ = 0;
        search_exhausted_ = false;
        search_error_.clear();
        search_dirty_ = false;
        bool ok = append_search_results(show_loader);
        render_search();
        return ok;
    }

    void open_search()
    {
        search_query_.clear();
        reset_search_results();
        search_dirty_ = false;
        render_search();
    }

    void cycle_sort_mode(int delta)
    {
        if (delta == 0) return;
        int next = static_cast<int>(sort_) + delta;
        next = (next % 5 + 5) % 5;
        sort_ = static_cast<SortMode>(next);

        reset_library_states();
        load_items_for_current_library(true);
    }

    void load_demo_media()
    {
        libraries_.clear();
        library_states_.clear();

        const char *names[] = {"中文影视", "电视剧", "音乐", "图片"};
        for (int i = 0; i < 4; ++i) {
            MediaLibrary lib;
            lib.id = std::string("demo-lib-") + std::to_string(i + 1);
            lib.name = names[i];
            lib.type = i == 1 ? "tvshows" : (i == 2 ? "music" : (i == 3 ? "photos" : "movies"));
            libraries_.push_back(lib);
            library_states_.push_back(LibraryState());
        }

        const char *demo_titles[][6] = {
            {"西部世界 VR", "流浪地球 2", "三体：黑暗森林", "The Return", "星际穿越", "沙丘"},
            {"漫长的季节", "黑镜", "庆余年", "繁花", "硅谷", "下一集测试"},
            {"午夜电台", "低频漫游", "片尾曲", "蓝色噪声", "城市节拍", "海边现场"},
            {"东京夜景", "海边日落", "设备合影", "露营灯光", "厨房一角", "窗外雨天"},
        };

        for (size_t lib_index = 0; lib_index < library_states_.size(); ++lib_index) {
            LibraryState &state = library_states_[lib_index];
            for (int i = 0; i < 6; ++i) {
                MediaItem item;
                item.id = "demo-" + std::to_string(lib_index) + "-" + std::to_string(i);
                item.name = demo_titles[lib_index][i];
                if (lib_index == 1) {
                    item.type = "Series";
                    item.media_type = "Video";
                } else if (lib_index == 2) {
                    item.type = "Audio";
                    item.media_type = "Audio";
                    item.album = "Cardputer Sessions";
                    item.album_artist = "M5 Demo";
                } else if (lib_index == 3) {
                    item.type = "Photo";
                    item.media_type = "Photo";
                } else {
                    item.type = "Movie";
                    item.media_type = "Video";
                }
                item.overview = "用于验证中文字体、目录展开和横向视频焦点的本地演示条目。";
                item.runtime_ticks = (5400LL + i * 900LL + static_cast<long long>(lib_index) * 600LL) * 10000000LL;
                item.container = is_audio_item(item) ? "mp3" : (is_photo_item(item) ? "jpg" : "mp4");
                item.video_codec = is_photo_item(item) ? "" : "h264";
                item.audio_codec = is_photo_item(item) ? "" : "aac";
                if (!is_audio_item(item) && !is_photo_item(item)) {
                    item.community_rating = 7.2f + static_cast<float>((i + lib_index) % 18) / 10.0f;
                }
                item.series_name = lib_index == 1 ? item.name : "";
                item.parent_index_number = 1;
                item.index_number = i + 1;
                state.items.push_back(item);
            }
            state.loaded = static_cast<int>(state.items.size());
            state.exhausted = true;
        }

        MediaLibrary recent;
        recent.id = kRecentPlayedLibraryId;
        recent.name = "最近播放";
        recent.type = kRecentPlayedLibraryType;
        libraries_.push_back(recent);
        LibraryState recent_state;
        if (!library_states_.empty() && !library_states_[0].items.empty()) {
            recent_state.items.push_back(library_states_[0].items[1]);
            recent_state.items.push_back(library_states_[0].items[0]);
        }
        if (library_states_.size() > 2 && !library_states_[2].items.empty()) {
            recent_state.items.push_back(library_states_[2].items[0]);
        }
        recent_state.loaded = static_cast<int>(recent_state.items.size());
        recent_state.exhausted = true;
        library_states_.push_back(recent_state);

        MediaLibrary settings;
        settings.id = kSettingsLibraryId;
        settings.name = "设置";
        settings.type = kSettingsLibraryType;
        libraries_.push_back(settings);
        LibraryState settings_state;
        settings_state.exhausted = true;
        library_states_.push_back(settings_state);

        library_index_ = 0;
        render_browse();
    }

    void reset_expansion(LibraryState &state)
    {
        state.expanded = false;
        state.expanded_series_id.clear();
        state.expanded_series_name.clear();
        state.seasons.clear();
        state.season_episodes.clear();
        state.season_loaded.clear();
        state.season_selected = 0;
        state.episode_selected = 0;
        state.expand_error.clear();
    }

    bool build_demo_series_expansion(LibraryState &state, const MediaItem &series)
    {
        state.seasons.clear();
        state.season_episodes.clear();
        state.season_loaded.clear();
        for (int season_index = 1; season_index <= 3; ++season_index) {
            MediaItem season;
            season.id = series.id + "-season-" + std::to_string(season_index);
            season.name = "第 " + std::to_string(season_index) + " 季";
            season.type = "Season";
            season.index_number = season_index;
            state.seasons.push_back(season);

            std::vector<MediaItem> episodes;
            for (int episode_index = 1; episode_index <= 8; ++episode_index) {
                MediaItem episode;
                episode.id = season.id + "-episode-" + std::to_string(episode_index);
                episode.name = "第 " + std::to_string(episode_index) + " 集";
                episode.type = "Episode";
                episode.series_name = series.name;
                episode.series_id = series.id;
                episode.season_id = season.id;
                episode.parent_index_number = season_index;
                episode.index_number = episode_index;
                episode.overview = "剧集展开动画和分季/集数选择的本地演示条目。";
                episode.runtime_ticks = (2700LL + episode_index * 120LL) * 10000000LL;
                episode.container = "mp4";
                episode.video_codec = "h264";
                episode.audio_codec = "aac";
                episode.community_rating = 7.5f + static_cast<float>((season_index + episode_index) % 14) / 10.0f;
                episodes.push_back(episode);
            }
            state.season_episodes.push_back(episodes);
            state.season_loaded.push_back(true);
        }
        state.season_selected = 0;
        state.episode_selected = 0;
        return true;
    }

    bool ensure_expanded_episodes(LibraryState &state, bool show_loader)
    {
        if (!state.expanded || state.season_selected >= state.seasons.size()) return false;
        if (state.season_selected < state.season_loaded.size() && state.season_loaded[state.season_selected]) {
            return true;
        }
        if (show_loader) show_loading("加载集数...");
        std::vector<MediaItem> episodes;
        std::string error;
        if (!client_->load_episodes(state.expanded_series_id,
                                    state.seasons[state.season_selected].id,
                                    episodes,
                                    error)) {
            state.expand_error = error;
            return false;
        }
        if (state.season_selected >= state.season_episodes.size()) {
            state.season_episodes.resize(state.season_selected + 1);
            state.season_loaded.resize(state.season_selected + 1, false);
        }
        state.season_episodes[state.season_selected] = episodes;
        state.season_loaded[state.season_selected] = true;
        state.episode_selected = 0;
        state.expand_error.clear();
        return true;
    }

    bool expand_selected_series()
    {
        LibraryState *state = current_library_state();
        if (!state || state->items.empty() || state->selected >= state->items.size()) return false;
        const MediaItem &series = state->items[state->selected];
        if (!is_series_item(series)) return false;

        if (state->expanded && state->expanded_series_id == series.id) {
            render_browse();
            return true;
        }

        reset_expansion(*state);
        state->expanded = true;
        state->expanded_series_id = series.id;
        state->expanded_series_name = series.name;

        if (series.id.find("demo-") == 0) {
            build_demo_series_expansion(*state, series);
            render_browse();
            return true;
        }

        show_loading("加载分季...");
        std::string error;
        if (!client_->load_seasons(series, state->seasons, error)) {
            state->expand_error = error;
            show_error(error);
            return false;
        }
        state->season_episodes.assign(state->seasons.size(), std::vector<MediaItem>());
        state->season_loaded.assign(state->seasons.size(), false);
        state->season_selected = 0;
        state->episode_selected = 0;
        if (!ensure_expanded_episodes(*state, true)) {
            show_error(state->expand_error.empty() ? "集数加载失败。" : state->expand_error);
            return false;
        }
        render_browse();
        return true;
    }

    const MediaItem *selected_item() const
    {
        const LibraryState *state = current_library_state();
        if (!state) return nullptr;
        if (state->expanded && state->season_selected < state->season_episodes.size()) {
            const std::vector<MediaItem> &episodes = state->season_episodes[state->season_selected];
            if (!episodes.empty() && state->episode_selected < episodes.size()) {
                return &episodes[state->episode_selected];
            }
        }
        if (state->items.empty() || state->selected >= state->items.size()) return nullptr;
        return &state->items[state->selected];
    }

    void animate_entry(lv_obj_t *obj, int x, int y, bool horizontal)
    {
        int dx = horizontal && last_horizontal_dir_ != 0 ? -last_horizontal_dir_ * 14 : 0;
        int dy = !horizontal && last_vertical_dir_ != 0 ? -last_vertical_dir_ * 8 : 0;
        lv_obj_set_pos(obj, x + dx, y + dy);
        lv_obj_set_style_opa(obj, LV_OPA_50, 0);
        if (horizontal) start_value_anim(obj, anim_set_x, x + dx, x, 150);
        else start_value_anim(obj, anim_set_y, y + dy, y, 150);
        start_value_anim(obj, anim_set_opa, LV_OPA_50, LV_OPA_COVER, 160);
    }

    void render_gradient_hint(lv_obj_t *parent, int x, int y, int w, int h, bool horizontal)
    {
        lv_obj_t *hint = lv_obj_create(parent);
        lv_obj_set_pos(hint, x, y);
        lv_obj_set_size(hint, w, h);
        lv_obj_clear_flag(hint, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_border_width(hint, 0, 0);
        lv_obj_set_style_radius(hint, 0, 0);
        lv_obj_set_style_bg_color(hint, accent_color(config_.type), 0);
        lv_obj_set_style_bg_grad_color(hint, lv_color_hex(0x05070A), 0);
        lv_obj_set_style_bg_grad_dir(hint, horizontal ? LV_GRAD_DIR_HOR : LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_bg_opa(hint, LV_OPA_50, 0);
        lv_obj_set_style_bg_grad_opa(hint, LV_OPA_TRANSP, 0);
        start_value_anim(hint, anim_set_opa, LV_OPA_TRANSP, LV_OPA_50, 180);
    }

    std::string cached_image_path(const MediaItem &item, const std::string &kind, int width, int height)
    {
        if (!client_ || item.id.empty() || kind.empty() || item.id.find("demo-") == 0) return "";
        mkdir("cache", 0700);
        std::string dir = "cache/posters";
        mkdir(dir.c_str(), 0700);
        std::string path = dir + "/" + safe_file_id(item.id) + "-v3-" + safe_file_id(kind) + "-" +
                           std::to_string(width) + "x" + std::to_string(height) + ".png";
        struct stat st {};
        if (stat(path.c_str(), &st) == 0 && st.st_size > 0) return path;

        std::string url = client_->image_url(item, kind, width, height);
        if (url.empty()) return "";
        std::string tmp = path + ".tmp";
        std::string cmd = "curl -fsSL --connect-timeout 3 --max-time 8 -o " +
                          shell_quote(tmp) + " " + shell_quote(url) + " >/dev/null 2>&1";
        if (system(cmd.c_str()) != 0) {
            unlink(tmp.c_str());
            return "";
        }
        struct stat tmp_st {};
        if (stat(tmp.c_str(), &tmp_st) != 0 || tmp_st.st_size <= 0) {
            unlink(tmp.c_str());
            return "";
        }
        rename(tmp.c_str(), path.c_str());
        return path;
    }

    std::string cached_image_fit_path(const MediaItem &item, const std::string &kind, int width, int height)
    {
        if (!client_ || item.id.empty() || kind.empty() || item.id.find("demo-") == 0) return "";
        mkdir("cache", 0700);
        std::string dir = "cache/posters";
        mkdir(dir.c_str(), 0700);
        std::string path = dir + "/" + safe_file_id(item.id) + "-v4fit-" + safe_file_id(kind) + "-" +
                           std::to_string(width) + "x" + std::to_string(height) + ".png";
        struct stat st {};
        if (stat(path.c_str(), &st) == 0 && st.st_size > 0) return path;

        std::string url = client_->image_fit_url(item, kind, width, height);
        if (url.empty()) return "";
        std::string tmp = path + ".tmp";
        std::string cmd = "curl -fsSL --connect-timeout 3 --max-time 8 -o " +
                          shell_quote(tmp) + " " + shell_quote(url) + " >/dev/null 2>&1";
        if (system(cmd.c_str()) != 0) {
            unlink(tmp.c_str());
            return "";
        }
        struct stat tmp_st {};
        if (stat(tmp.c_str(), &tmp_st) != 0 || tmp_st.st_size <= 0) {
            unlink(tmp.c_str());
            return "";
        }
        rename(tmp.c_str(), path.c_str());
        return path;
    }

    std::string cached_poster_path(const MediaItem &item, int width, int height)
    {
        return cached_image_path(item, "Primary", width, height);
    }

    std::string cached_landscape_fit_path(const MediaItem &item,
                                          const std::string &kind,
                                          int width,
                                          int height)
    {
        if (!item_image_kind_is_usable(item, kind)) return "";
        std::string path = cached_image_fit_path(item, kind, width, height);
        if (path.empty()) return "";
        if (should_keep_card_image(item, kind, path)) return path;
        unlink(path.c_str());
        return "";
    }

    std::string cached_episode_still_path(const MediaItem &item, int width, int height)
    {
        std::string path = cached_landscape_fit_path(item, "Primary", width, height);
        if (!path.empty()) return path;
        path = cached_landscape_fit_path(item, "Backdrop", width, height);
        if (!path.empty()) return path;
        path = cached_landscape_fit_path(item, "Thumb", width, height);
        return path;
    }

    std::string cached_landscape_background_path(const MediaItem &item)
    {
        if (item_prefers_still_art(item)) {
            std::string path = cached_episode_still_path(item, kScreenW, kScreenH);
            if (!path.empty()) return path;

            if (is_episode_item(item) && !item.series_id.empty()) {
                MediaItem series;
                series.id = item.series_id;
                series.name = item.series_name;
                series.type = "Series";
                path = cached_image_fit_path(series, "Backdrop", kScreenW, kScreenH);
                if (!path.empty() && looks_landscape_file(path)) return path;
                if (!path.empty()) unlink(path.c_str());
                path = cached_image_fit_path(series, "Thumb", kScreenW, kScreenH);
                if (!path.empty() && looks_landscape_file(path)) return path;
                if (!path.empty()) unlink(path.c_str());
            }
            return "";
        }

        std::string path = cached_image_fit_path(item, "Thumb", kScreenW, kScreenH);
        if (!path.empty()) return path;
        path = cached_image_fit_path(item, "Backdrop", kScreenW, kScreenH);
        if (!path.empty()) return path;

        if (is_episode_item(item) && !item.series_id.empty()) {
            MediaItem series;
            series.id = item.series_id;
            series.name = item.series_name;
            series.type = "Series";
            path = cached_image_fit_path(series, "Thumb", kScreenW, kScreenH);
            if (!path.empty()) return path;
            path = cached_image_fit_path(series, "Backdrop", kScreenW, kScreenH);
            if (!path.empty()) return path;
        }
        return "";
    }

    std::string cached_background_path(const MediaItem &item)
    {
        std::string path = cached_landscape_background_path(item);
        if (!path.empty()) return path;
        path = cached_image_fit_path(item, "Primary", kScreenW, kScreenH);
        if (!path.empty()) return path;
        if (is_episode_item(item) && !item.series_id.empty()) {
            MediaItem series;
            series.id = item.series_id;
            series.name = item.series_name;
            series.type = "Series";
            path = cached_image_fit_path(series, "Primary", kScreenW, kScreenH);
        }
        return path;
    }

    std::string cached_detail_art_path(const MediaItem &item, bool &poster_style)
    {
        poster_style = false;
        std::string path = cached_landscape_background_path(item);
        if (!path.empty()) return path;

        poster_style = true;
        const int poster_w = (is_audio_item(item) || is_photo_item(item)) ? 138 : 112;
        const int poster_h = (is_audio_item(item) || is_photo_item(item)) ? 138 : 168;
        path = cached_image_fit_path(item, "Primary", poster_w, poster_h);
        if (!path.empty()) return path;

        if (is_episode_item(item) && !item.series_id.empty()) {
            MediaItem series;
            series.id = item.series_id;
            series.name = item.series_name;
            series.type = "Series";
            path = cached_image_fit_path(series, "Primary", poster_w, poster_h);
        }
        return path;
    }

    void render_black_gradient(int x, int y, int w, int h, bool horizontal,
                               lv_opa_t start_opa, lv_opa_t end_opa)
    {
        if (w <= 0 || h <= 0) return;
        const int steps = horizontal ? 12 : 8;
        for (int i = 0; i < steps; ++i) {
            int pos0 = horizontal ? (i * w / steps) : (i * h / steps);
            int pos1 = horizontal ? ((i + 1) * w / steps) : ((i + 1) * h / steps);
            int sw = horizontal ? std::max(1, pos1 - pos0) : w;
            int sh = horizontal ? h : std::max(1, pos1 - pos0);
            int sx = horizontal ? x + pos0 : x;
            int sy = horizontal ? y : y + pos0;
            int opa = static_cast<int>(start_opa) +
                      (static_cast<int>(end_opa) - static_cast<int>(start_opa)) * i / std::max(1, steps - 1);
            if (opa <= 0) continue;

            lv_obj_t *shade = lv_obj_create(root_);
            lv_obj_remove_style_all(shade);
            lv_obj_set_pos(shade, sx, sy);
            lv_obj_set_size(shade, sw, sh);
            lv_obj_clear_flag(shade, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_style_bg_color(shade, lv_color_hex(0x020407), 0);
            lv_obj_set_style_bg_opa(shade, static_cast<lv_opa_t>(std::min(255, opa)), 0);
        }
    }

    void render_atmosphere_background(const MediaItem &item, bool detail_layout = false)
    {
        bool poster_style = false;
        std::string background = detail_layout ? cached_detail_art_path(item, poster_style)
                                               : cached_background_path(item);
        if (!background.empty()) {
            lv_obj_t *img = lv_image_create(root_);
            if (detail_layout && poster_style) {
                lv_obj_set_pos(img, 154, 0);
                lv_obj_set_size(img, 166, kScreenH);
            } else {
                lv_obj_set_pos(img, 0, 0);
                lv_obj_set_size(img, kScreenW, kScreenH);
            }
            std::string src = lvgl_file_path(background);
            lv_image_set_src(img, src.c_str());
            lv_image_set_inner_align(img, detail_layout && poster_style
                                          ? LV_IMAGE_ALIGN_CONTAIN
                                          : LV_IMAGE_ALIGN_COVER);

            render_black_gradient(0, 0, kScreenW, kScreenH, true,
                                  detail_layout ? LV_OPA_COVER : LV_OPA_80,
                                  detail_layout ? LV_OPA_20 : LV_OPA_60);
            render_black_gradient(0, 0, kScreenW, 64, false, LV_OPA_80, LV_OPA_TRANSP);
            render_black_gradient(0, 108, kScreenW, 62, false, LV_OPA_TRANSP, LV_OPA_90);
        }
    }

    void render_poster(lv_obj_t *card, const MediaItem &item, int x, int y, int w, int h)
    {
        std::string poster;
        if (item_prefers_still_art(item)) poster = cached_episode_still_path(item, w, h);
        if (poster.empty()) {
            poster = item_prefers_still_art(item) ? "" : cached_poster_path(item, w, h);
        }
        if (!poster.empty()) {
            lv_obj_t *img = lv_image_create(card);
            lv_obj_set_pos(img, x, y);
            lv_obj_set_size(img, w, h);
            std::string src = lvgl_file_path(poster);
            lv_image_set_src(img, src.c_str());
            lv_image_set_inner_align(img, item_prefers_still_art(item) ? LV_IMAGE_ALIGN_CONTAIN : LV_IMAGE_ALIGN_COVER);
        } else {
            const char *placeholder = is_audio_item(item) ? "封面" : (is_photo_item(item) ? "图片" : (item_prefers_still_art(item) ? "剧照" : "海报"));
            label(card, placeholder, x, y + h / 2 - 8, w, 16, lv_color_hex(0x586879));
        }
    }

    void card_art_size(const MediaItem &item, int card_w, int card_h, int &art_w, int &art_h)
    {
        if (is_episode_item(item)) {
            fit_art_box(item, card_w - 4, card_h - 40, art_w, art_h);
        } else if (is_audio_item(item) || is_photo_item(item)) {
            art_w = std::min(card_w - 8, card_h > 136 ? 72 : 62);
            art_h = art_w;
        } else {
            art_w = std::min(card_w - 8, card_h > 136 ? 64 : 56);
            art_h = art_w * 3 / 2;
        }
        if (art_h > card_h - 38) {
            art_h = card_h - 38;
            if (is_episode_item(item)) fit_art_box(item, card_w - 4, art_h, art_w, art_h);
            else if (is_audio_item(item) || is_photo_item(item)) art_w = art_h;
            else art_w = art_h * 2 / 3;
        }
        art_w = std::max(24, std::min(art_w, card_w));
        art_h = std::max(24, std::min(art_h, card_h));
    }

    void center_text(lv_obj_t *obj)
    {
        lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_CENTER, 0);
    }

    lv_obj_t *centered_card_label(lv_obj_t *parent,
                                  const char *text,
                                  int x,
                                  int y,
                                  int w,
                                  int h,
                                  lv_color_t color)
    {
        lv_obj_t *obj = lv_label_create(parent);
        lv_obj_set_pos(obj, x, y);
        lv_obj_set_size(obj, w, h);
        std::string clean = single_line_text(text);
        lv_label_set_text(obj, clean.c_str());
        lv_label_set_long_mode(obj, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(obj, color, 0);
        lv_obj_set_style_text_font(obj, ui_font_14(), 0);
        return obj;
    }

    void render_media_card(lv_obj_t *parent,
                           const MediaItem &item,
                           int x,
                           int y,
                           int w,
                           int h,
                           bool selected)
    {
        lv_obj_t *card = lv_obj_create(parent);
        lv_obj_remove_style_all(card);
        lv_obj_set_pos(card, x, y);
        lv_obj_set_size(card, w, h);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_opa(card, selected ? LV_OPA_COVER : LV_OPA_70, 0);
        animate_entry(card, x, y, true);

        std::string title = item_display_name(item);
        std::string meta = card_meta_label(item);
        bool show_meta = !meta.empty() && meta != "剧集" && meta != "图片";
        int poster_w = 0;
        int poster_h = 0;
        card_art_size(item, w, h, poster_w, poster_h);
        const int title_h = 20;
        const int text_h = title_h + (show_meta ? 18 : 0);
        const int max_art_h = std::max(24, h - 5 - text_h);
        if (poster_h > max_art_h) {
            if (is_episode_item(item)) {
                fit_art_box(item, w - 4, max_art_h, poster_w, poster_h);
            } else {
                poster_h = max_art_h;
                if (is_audio_item(item) || is_photo_item(item)) poster_w = poster_h;
                else poster_w = poster_h * 2 / 3;
            }
            poster_w = std::max(24, std::min(poster_w, w));
        }
        const int content_h = poster_h + 5 + text_h;
        const int content_y = std::max(0, (h - content_h) / 2);
        const int poster_x = (w - poster_w) / 2;
        render_poster(card, item, poster_x, content_y, poster_w, poster_h);

        int text_y = content_y + poster_h + 5;
        lv_color_t title_color = selected ? accent_color(config_.type) : lv_color_hex(0xB8C4D0);
        if (should_marquee(title, w, 14)) {
            marquee_label(card, title.c_str(), 0, text_y, w, title_h, title_color, ui_font_14(), 12);
        } else {
            centered_card_label(card, title.c_str(), 0, text_y, w, title_h, title_color);
        }
        if (show_meta) {
            text_y += title_h;
            centered_card_label(card, meta.c_str(), 0, text_y, w, 16, accent_color(config_.type));
        }
    }

    void render_series_expansion(lv_obj_t *parent, const LibraryState &state, int x, int y, int w, int h)
    {
        std::string title = state.expanded_series_name.empty() ? "剧集" : state.expanded_series_name;
        if (should_marquee(title, w, 14)) {
            marquee_label(parent, title.c_str(), x, y, w, 18, lv_color_hex(0xFFFFFF), ui_font_14(), 14);
        } else {
            label(parent, title.c_str(), x, y, w, 18, lv_color_hex(0xFFFFFF));
        }

        if (!state.expand_error.empty()) {
            label(parent, state.expand_error.c_str(), x, y + 48, w, 18, lv_color_hex(0xFF9B9B));
            return;
        }
        if (state.seasons.empty()) {
            label(parent, "没有分季。", x, y + 48, w, 18, lv_color_hex(0xFFFFFF));
            return;
        }

        int season_first = state.season_selected > 1 ? static_cast<int>(state.season_selected) - 1 : 0;
        if (state.seasons.size() > 3 && season_first + 3 > static_cast<int>(state.seasons.size())) {
            season_first = static_cast<int>(state.seasons.size()) - 3;
        }
        for (int i = 0; i < 3; ++i) {
            size_t season_idx = static_cast<size_t>(season_first + i);
            if (season_idx >= state.seasons.size()) break;
            bool selected = season_idx == state.season_selected;
            std::string name = state.seasons[season_idx].name;
            lv_color_t color = selected ? lv_color_hex(0xFFFFFF) : lv_color_hex(0xB8C6D4);
            if (selected && should_marquee(name, 72, 14)) {
                marquee_label(parent, name.c_str(), x + i * 82, y + 22, 72, 16, color, ui_font_14(), 14);
            } else {
                label(parent, name.c_str(), x + i * 82, y + 22, 72, 16, color);
            }
        }

        const std::vector<MediaItem> *episodes = nullptr;
        if (state.season_selected < state.season_episodes.size()) {
            episodes = &state.season_episodes[state.season_selected];
        }
        if (!episodes || episodes->empty()) {
            label(parent, "这个季没有集数。", x, y + 56, w, 18, lv_color_hex(0xFFFFFF));
            return;
        }

        const int visible_count = 2;
        const int gap = 8;
        const int card_w = (w - gap) / visible_count;
        int first = first_visible_index(state.episode_selected, episodes->size(), visible_count);
        for (int c = 0; c < visible_count; ++c) {
            size_t episode_idx = static_cast<size_t>(first + c);
            if (episode_idx >= episodes->size()) break;
            render_media_card(parent, (*episodes)[episode_idx], x + c * (card_w + gap), y + 48, card_w, 114,
                              episode_idx == state.episode_selected);
        }
    }

    void render_browse()
    {
        view_ = View::Browse;
        clear();
        add_to_group(root_);
        lv_group_focus_obj(root_);

        if (libraries_.empty()) {
            label(root_, t(TextKey::NoLibraries), 10, 62, 300, 20, lv_color_hex(0xFFFFFF));
            return;
        }

        const int left_w = 66;
        const int right_x = 72;
        const int right_w = 248;
        int y = 6;
        const LibraryState *focused_state = current_library_state();
        size_t start = library_index_ > 2 ? library_index_ - 2 : 0;
        if (library_index_ + 4 >= libraries_.size() && start > 0) start--;
        for (size_t idx = start; idx < libraries_.size(); ++idx) {
            if (idx >= libraries_.size() || y > 160) break;
            bool selected = idx == library_index_;
            int dist = idx > library_index_ ? static_cast<int>(idx - library_index_)
                                            : static_cast<int>(library_index_ - idx);
            const lv_font_t *font = selected ? ui_font_menu_focus() :
                                    (dist == 1 ? ui_font_menu_near() : ui_font_14());
            int h = selected ? 36 : (dist == 1 ? 28 : 20);
            lv_color_t color = selected ? accent_color(config_.type) : lv_color_hex(0x8B98A8);
            int font_px = selected ? 28 : (dist == 1 ? 21 : 14);
            if (should_marquee(libraries_[idx].name, left_w - 8, font_px)) {
                marquee_label(root_, libraries_[idx].name.c_str(), 6, y, left_w - 8, h,
                              color, font, selected ? 16 : 14);
            } else {
                label(root_, libraries_[idx].name.c_str(), 6, y, left_w - 8, h, color, font);
            }
            y += h + 4;
        }

        if (!focused_state) return;
        if (library_index_ < libraries_.size() && library_is_settings(libraries_[library_index_])) {
            label(root_, t(TextKey::SettingsTitle), right_x, 18, right_w, 20,
                  accent_color(config_.type), ui_font_16());
            label(root_, (std::string(t(TextKey::SettingsService)) + server_name(config_.type)).c_str(),
                  right_x, 48, right_w, 18, lv_color_hex(0xFFFFFF));
            label(root_, config_.base_url.c_str(), right_x, 68, right_w, 18, lv_color_hex(0xB7C4D2));
            label(root_, config_.username.c_str(), right_x, 90, right_w, 18, lv_color_hex(0xB7C4D2));
            label(root_, current_language() == UiLanguage::Chinese ? "Enter 打开设置 / 退出登录"
                                                                   : "Enter for settings / log out",
                  right_x, 132, right_w, 18, lv_color_hex(0x9BA8B6));
            return;
        }
        if (focused_state->expanded) {
            render_series_expansion(root_, *focused_state, right_x, 8, right_w, 158);
            return;
        }

        if (!focused_state->error.empty()) {
            label(root_, focused_state->error.c_str(), right_x, 64, right_w, 22, lv_color_hex(0xFF9B9B));
            return;
        }
        if (focused_state->items.empty()) {
            label(root_, t(TextKey::NoContent), right_x, 64, right_w, 22, lv_color_hex(0xFFFFFF));
            return;
        }

        const bool episode_cards = focused_state->selected < focused_state->items.size() &&
                                   is_episode_item(focused_state->items[focused_state->selected]);
        const int visible_count = episode_cards ? 2 : 3;
        const int gap = 8;
        const int card_w = (right_w - gap * (visible_count - 1)) / visible_count;
        int first = first_visible_index(focused_state->selected, focused_state->items.size(), visible_count);
        for (int c = 0; c < visible_count; ++c) {
            size_t item_idx = static_cast<size_t>(first + c);
            if (item_idx >= focused_state->items.size()) break;
            render_media_card(root_, focused_state->items[item_idx],
                              right_x + c * (card_w + gap), 10, card_w, 150,
                              item_idx == focused_state->selected);
        }
    }

    void render_search()
    {
        view_ = View::Search;
        clear();
        add_to_group(root_);
        lv_group_focus_obj(root_);

        label(root_, "搜索", 6, 5, 40, 18, accent_color(config_.type), ui_font_16());
        std::string query = search_query_.empty() ? "输入关键词" : search_query_ + (search_dirty_ ? "_" : "");
        label(root_, query.c_str(), 48, 6, 264, 18,
              search_query_.empty() ? lv_color_hex(0x778596) : lv_color_hex(0xFFFFFF));

        if (!search_error_.empty()) {
            label(root_, search_error_.c_str(), 8, 54, 304, 22, lv_color_hex(0xFF9B9B));
            label(root_, "Esc 返回", 8, 146, 304, 16, lv_color_hex(0x9BA8B6));
            return;
        }

        if (trim(search_query_).empty()) {
            label(root_, "输入后按 Enter 搜索", 8, 66, 304, 20, lv_color_hex(0xFFFFFF));
            label(root_, "Esc 返回", 8, 146, 304, 16, lv_color_hex(0x9BA8B6));
            return;
        }

        if (search_results_.empty()) {
            label(root_, search_dirty_ ? "Enter 搜索" : "没有结果", 8, 66, 304, 20, lv_color_hex(0xFFFFFF));
            label(root_, "Esc 返回", 8, 146, 304, 16, lv_color_hex(0x9BA8B6));
            return;
        }

        const bool episode_cards = search_selected_ < search_results_.size() &&
                                   is_episode_item(search_results_[search_selected_]);
        const int visible_count = episode_cards ? 2 : 3;
        const int gap = 8;
        const int card_w = (308 - gap * (visible_count - 1)) / visible_count;
        int first = first_visible_index(search_selected_, search_results_.size(), visible_count);
        for (int c = 0; c < visible_count; ++c) {
            size_t item_idx = static_cast<size_t>(first + c);
            if (item_idx >= search_results_.size()) break;
            render_media_card(root_, search_results_[item_idx],
                              6 + c * (card_w + gap), 38, card_w, 118,
                              item_idx == search_selected_);
        }
    }

    std::string detail_breadcrumb(const MediaItem &item) const
    {
        MediaItem copy = item;
        const LibraryState *state = current_library_state();
        if (state && state->expanded) {
            copy.type = "Episode";
            copy.media_type = "Video";
            if (copy.series_name.empty()) copy.series_name = state->expanded_series_name;
            if (copy.series_id.empty()) copy.series_id = state->expanded_series_id;
            if (copy.parent_index_number <= 0 && state->season_selected < state->seasons.size()) {
                int season = state->seasons[state->season_selected].index_number;
                copy.parent_index_number = season > 0 ? season : static_cast<int>(state->season_selected) + 1;
            }
            if (copy.index_number <= 0 && !name_looks_episode_labeled(copy.name)) {
                copy.index_number = static_cast<int>(state->episode_selected) + 1;
            }
        }
        return item_breadcrumb(copy);
    }

    void render_detail()
    {
        const MediaItem *item = selected_item();
        if (!item) return;
        view_ = View::Detail;
        clear();
        render_atmosphere_background(*item, true);
        add_to_group(root_);
        lv_group_focus_obj(root_);
        std::string crumb = detail_breadcrumb(*item);
        if (should_marquee(crumb, 308, 14)) {
            marquee_label(root_, crumb.c_str(), 6, 4, 308, 20, accent_color(config_.type), ui_font_14(), 14);
        } else {
            label(root_, crumb.c_str(), 6, 4, 308, 20, accent_color(config_.type));
        }
        std::string primary_meta = ticks_to_time(item->runtime_ticks);
        if (is_photo_item(*item)) primary_meta = item->container.empty() ? "图片" : item->container;
        else if (is_audio_item(*item) && !item->album_artist.empty()) {
            primary_meta = item->album_artist + "  " + primary_meta;
        }
        label(root_, primary_meta.c_str(), 6, 29, 144, 16, lv_color_hex(0xB7C4D2));
        std::string meta = item->container + " " + item->video_codec + " " + item->audio_codec;
        if (item->production_year > 0) meta += " " + std::to_string(item->production_year);
        label(root_, meta.c_str(), 154, 29, 160, 16, lv_color_hex(0xB7C4D2));
        std::string overview = item->overview.empty() ? t(TextKey::NoOverview) : item->overview;
        lv_obj_t *body_clip = lv_obj_create(root_);
        lv_obj_remove_style_all(body_clip);
        lv_obj_set_pos(body_clip, 7, 47);
        lv_obj_set_size(body_clip, 306, 104);
        lv_obj_clear_flag(body_clip, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *body = lv_label_create(body_clip);
        lv_obj_set_pos(body, 0, 0);
        lv_obj_set_width(body, 306);
        lv_label_set_text(body, overview.c_str());
        lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_color(body, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_font(body, ui_font_14(), 0);
        lv_obj_update_layout(body);
        int body_h = lv_obj_get_height(body);
        int clip_h = lv_obj_get_height(body_clip);
        start_vertical_text_scroll(body, body_h - clip_h + 6);
        label(root_, t(TextKey::DetailHelp), 6, 154, 306, 16, lv_color_hex(0x9BA8B6));
    }

    std::vector<size_t> stream_indices(const char *type) const
    {
        std::vector<size_t> out;
        for (size_t i = 0; i < playback_.streams.size(); ++i) {
            if (playback_.streams[i].type == type) out.push_back(i);
        }
        return out;
    }

    size_t first_stream_index(const char *type) const
    {
        for (size_t i = 0; i < playback_.streams.size(); ++i) {
            if (playback_.streams[i].type == type) return i;
        }
        return kNoStream;
    }

    int stream_ordinal(size_t stream_index) const
    {
        if (stream_index >= playback_.streams.size()) return 0;
        int ordinal = 0;
        const std::string type = playback_.streams[stream_index].type;
        for (size_t i = 0; i <= stream_index && i < playback_.streams.size(); ++i) {
            if (playback_.streams[i].type == type) ordinal++;
        }
        return ordinal;
    }

    std::string stream_caption(size_t stream_index, const char *fallback) const
    {
        if (stream_index >= playback_.streams.size()) return fallback;
        const MediaStream &s = playback_.streams[stream_index];
        std::string text;
        if (!s.language.empty()) text = s.language;
        if (!s.title.empty() && (text.empty() || !contains_ci(s.title, text))) {
            if (!text.empty()) text += " ";
            text += s.title;
        }
        if (!s.codec.empty()) {
            if (!text.empty()) text += " ";
            text += s.codec;
        }
        if (text.empty()) text = std::string(fallback) + " " + std::to_string(stream_ordinal(stream_index));
        return text;
    }

    std::string active_audio_caption() const
    {
        if (stream_indices("Audio").empty()) return "无音轨信息";
        size_t index = selected_audio_stream_ == kNoStream ? first_stream_index("Audio") : selected_audio_stream_;
        return stream_caption(index, "音轨");
    }

    std::string active_subtitle_caption() const
    {
        if (stream_indices("Subtitle").empty()) return "无字幕";
        if (selected_subtitle_stream_ == kNoStream) return "关闭";
        return stream_caption(selected_subtitle_stream_, "字幕");
    }

    void reset_player_stream_selection()
    {
        selected_audio_stream_ = first_stream_index("Audio");
        selected_subtitle_stream_ = kNoStream;
    }

    void begin_player_session()
    {
        playback_started_at_ = std::time(nullptr);
        playback_seek_seconds_ = 0;
        playback_paused_seconds_ = 0;
        playback_paused_ = false;
        playback_reported_started_ = false;
        last_progress_report_at_ = 0;
        player_notice_.clear();
        player_notice_until_ = 0;
        reset_player_stream_selection();
    }

    long long current_playback_seconds() const
    {
        if (playback_started_at_ <= 0) return playback_paused_seconds_;
        if (playback_paused_) return playback_paused_seconds_;
        long long elapsed = playback_seek_seconds_ +
                            static_cast<long long>(std::time(nullptr) - playback_started_at_);
        if (elapsed < 0) elapsed = 0;
        long long duration = ticks_to_seconds(current_item_.runtime_ticks);
        if (duration > 0 && elapsed > duration) elapsed = duration;
        return elapsed;
    }

    long long current_playback_ticks() const
    {
        return current_playback_seconds() * 10000000LL;
    }

    void report_playback_started()
    {
        if (!client_ || current_item_.id.empty() || playback_.play_session_id.empty()) return;
        if (client_->report_playing(current_item_, playback_, current_playback_ticks(), playback_paused_)) {
            playback_reported_started_ = true;
            last_progress_report_at_ = std::time(nullptr);
        }
    }

    void report_playback_progress(bool force = false)
    {
        if (!playback_reported_started_ || !client_) return;
        time_t now = std::time(nullptr);
        if (!force && last_progress_report_at_ > 0 && now - last_progress_report_at_ < 8) return;
        if (client_->report_progress(current_item_, playback_, current_playback_ticks(), playback_paused_)) {
            last_progress_report_at_ = now;
        }
    }

    void report_playback_stopped()
    {
        if (!playback_reported_started_ || !client_) return;
        client_->report_stopped(current_item_, playback_, current_playback_ticks());
        playback_reported_started_ = false;
        last_progress_report_at_ = 0;
    }

    void adjust_playback_position(int delta_seconds)
    {
        long long duration = ticks_to_seconds(current_item_.runtime_ticks);
        long long next = current_playback_seconds() + delta_seconds;
        if (next < 0) next = 0;
        if (duration > 0 && next > duration) next = duration;
        if (playback_paused_) {
            playback_paused_seconds_ = next;
        } else {
            playback_seek_seconds_ = next;
            playback_started_at_ = std::time(nullptr);
        }
    }

    long long clamped_playback_position(long long seconds) const
    {
        long long duration = ticks_to_seconds(current_item_.runtime_ticks);
        if (seconds < 0) seconds = 0;
        if (duration > 0 && seconds > duration) seconds = duration;
        return seconds;
    }

    std::string playback_url_for_position(long long seconds) const
    {
        std::string url = playback_.url;
        if (url.empty() || is_photo_item(current_item_)) return url;
        seconds = clamped_playback_position(seconds);
        if (is_audio_item(current_item_)) {
            return url;
        }
        url = set_query_param(url, "StartTimeTicks", std::to_string(seconds * 10000000LL));
        if (selected_audio_stream_ != kNoStream && selected_audio_stream_ < playback_.streams.size()) {
            int index = playback_.streams[selected_audio_stream_].index;
            if (index >= 0) url = set_query_param(url, "AudioStreamIndex", std::to_string(index));
        }
        if (selected_subtitle_stream_ != kNoStream && selected_subtitle_stream_ < playback_.streams.size()) {
            int index = playback_.streams[selected_subtitle_stream_].index;
            if (index >= 0) url = set_query_param(url, "SubtitleStreamIndex", std::to_string(index));
        } else {
            url = set_query_param(url, "SubtitleStreamIndex", "-1");
        }
        return url;
    }

    bool restart_video_playback_at(long long seconds, const std::string &notice)
    {
        if (is_audio_item(current_item_) || is_photo_item(current_item_)) return false;
        seconds = clamped_playback_position(seconds);
        std::string url = playback_url_for_position(seconds);
        std::string error;
        if (url.empty() || !player_.start(url, config_.access_token, error, false, true,
                                          audio_output_, 0, small_screen_playback_)) {
            set_player_notice(error.empty() ? "重新拉起播放失败" : ellipsize(error, 40), 4);
            app_log("Restart playback failed item=" + current_item_.id + " error=" + error);
            return false;
        }
        app_log("Restart playback item=" + current_item_.id + " position=" +
                std::to_string(seconds) +
                " small_screen=" + bool_text(small_screen_playback_) +
                " url=" + url);
        playback_seek_seconds_ = seconds;
        playback_started_at_ = std::time(nullptr);
        playback_paused_seconds_ = 0;
        playback_paused_ = false;
        set_player_notice(notice, 4);
        report_playback_progress(true);
        return true;
    }

    bool restart_playback_for_audio_output()
    {
        if (playback_.url.empty() || is_photo_item(current_item_)) return false;
        long long seconds = clamped_playback_position(current_playback_seconds());
        std::string url = playback_url_for_position(seconds);
        std::string error;
        bool video_mode = !is_audio_item(current_item_) && !is_photo_item(current_item_);
        long long start_seconds = is_audio_item(current_item_) ? seconds : 0;
        if (!player_.start(url, config_.access_token, error, false, video_mode, audio_output_,
                           start_seconds, video_mode && small_screen_playback_)) {
            set_player_notice(error.empty() ? "切换输出失败" : ellipsize(error, 40), 4);
            app_log("Audio output switch failed item=" + current_item_.id +
                    " output=" + audio_output_label(audio_output_) +
                    " small_screen=" + bool_text(small_screen_playback_) +
                    " error=" + error);
            return false;
        }
        long long effective_seconds = seconds;
        if (is_audio_item(current_item_) && start_seconds > 0 && !player_.start_offset_supported()) {
            effective_seconds = 0;
            set_player_notice(std::string("音频输出 ") + audio_output_label(audio_output_) + " / 从头播放", 4);
        }
        playback_seek_seconds_ = effective_seconds;
        playback_started_at_ = std::time(nullptr);
        playback_paused_seconds_ = 0;
        playback_paused_ = false;
        app_log("Audio output switched item=" + current_item_.id +
                " output=" + audio_output_label(audio_output_) +
                " small_screen=" + bool_text(small_screen_playback_) +
                " position=" + std::to_string(seconds) +
                " effective_position=" + std::to_string(effective_seconds) +
                " local_seek=" + bool_text(start_seconds > 0 && player_.start_offset_supported()) +
                " url=" + url);
        if (!is_audio_item(current_item_)) report_playback_progress(true);
        return true;
    }

    void cycle_audio_output()
    {
        audio_output_ = next_audio_output(audio_output_);
        std::string notice = std::string("音频输出 ") + audio_output_label(audio_output_);
        set_player_notice(notice, 4);
        app_log("Audio output cycle target=" + std::string(audio_output_label(audio_output_)) +
                " item=" + current_item_.id +
                " running=" + bool_text(player_.running()));
        if (view_ == View::Player && player_.running() && !is_photo_item(current_item_)) {
            restart_playback_for_audio_output();
        }
        render_player();
    }

    const char *video_display_output_label() const
    {
        return small_screen_playback_ ? "小屏" : "HDMI 大屏";
    }

    std::string player_output_label() const
    {
        if (is_audio_item(current_item_)) return audio_output_label(audio_output_);
        if (is_photo_item(current_item_)) return "HDMI 大屏";
        return video_display_output_label();
    }

    bool restart_video_for_display_output(bool small_screen_output, const std::string &notice)
    {
        if (is_audio_item(current_item_) || is_photo_item(current_item_)) return false;
        if (!small_screen_output && !hdmi_output_connected()) {
            set_player_notice("未连接 HDMI，仍在小屏播放", 4);
            app_log("Video output switch blocked no HDMI item=" + current_item_.id);
            return false;
        }

        long long seconds = clamped_playback_position(current_playback_seconds());
        std::string url = playback_url_for_position(seconds);
        std::string error;
        AudioOutput target_audio = small_screen_output ? AudioOutput::Speaker : audio_output_;
        if (url.empty() || !player_.start(url, config_.access_token, error, false, true,
                                          target_audio, 0, small_screen_output)) {
            set_player_notice(error.empty() ? "切换输出失败" : ellipsize(error, 40), 4);
            app_log("Video output switch failed item=" + current_item_.id +
                    " target_small=" + bool_text(small_screen_output) +
                    " error=" + error);
            return false;
        }

        small_screen_playback_ = small_screen_output;
        audio_output_ = target_audio;
        playback_seek_seconds_ = seconds;
        playback_started_at_ = std::time(nullptr);
        playback_paused_seconds_ = 0;
        playback_paused_ = false;
        set_player_notice(notice, 4);
        report_playback_progress(true);
        app_log("Video output switched item=" + current_item_.id +
                " output=" + video_display_output_label() +
                " position=" + std::to_string(seconds) +
                " audio_output=" + audio_output_label(audio_output_) +
                " url=" + url);
        return true;
    }

    void cycle_video_display_output()
    {
        bool target_small = !small_screen_playback_;
        std::string notice = target_small ? "输出切到小屏" : "输出切到 HDMI 大屏";
        restart_video_for_display_output(target_small, notice);
        render_player();
    }

    void cycle_playback_output()
    {
        if (is_audio_item(current_item_)) {
            cycle_audio_output();
            return;
        }
        if (is_photo_item(current_item_)) {
            set_player_notice("图片固定 HDMI 输出", 4);
            render_player();
            return;
        }
        cycle_video_display_output();
    }

    void set_player_notice(const std::string &text, int seconds = 3)
    {
        player_notice_ = text;
        player_notice_until_ = std::time(nullptr) + seconds;
    }

    std::string player_state_text() const
    {
        time_t now = std::time(nullptr);
        if (!player_notice_.empty() && now <= player_notice_until_) return player_notice_;
        if (playback_paused_) return "已暂停";
        if (is_photo_item(current_item_)) return "HDMI 图片输出";
        if (is_audio_item(current_item_)) return "音频播放中";
        if (small_screen_playback_) return "小屏播放中";
        if (playback_started_at_ > 0 && now - playback_started_at_ < 8) return "HDMI 启动中...";
        return "播放中";
    }

    void update_player_progress()
    {
        if (view_ != View::Player) return;
        if (small_screen_playback_) {
            report_playback_progress(false);
            return;
        }

        long long duration = ticks_to_seconds(current_item_.runtime_ticks);
        long long elapsed = current_playback_seconds();
        if (duration > 0 && elapsed > duration) elapsed = duration;

        if (player_time_label_) {
            std::string time_text = seconds_to_clock(elapsed) + " / " +
                                    (duration > 0 ? seconds_to_clock(duration) : "--:--");
            lv_label_set_text(player_time_label_, time_text.c_str());
        }

        if (player_status_label_) {
            std::string status = player_state_text();
            lv_label_set_text(player_status_label_, status.c_str());
            lv_obj_set_style_text_color(player_status_label_,
                                        playback_paused_ ? lv_color_hex(0xD6DEE8) : accent_color(config_.type), 0);
        }

        if (player_progress_fill_ && player_progress_w_ > 0) {
            int fill_w = 8;
            if (duration > 0) {
                fill_w = static_cast<int>((elapsed * player_progress_w_) / duration);
                if (elapsed > 0) fill_w = std::max(4, fill_w);
            } else {
                int pulse = static_cast<int>((std::time(nullptr) - playback_started_at_) % 5);
                fill_w = 24 + pulse * 18;
            }
            if (fill_w > player_progress_w_) fill_w = player_progress_w_;
            lv_obj_set_width(player_progress_fill_, fill_w);
            lv_obj_set_style_bg_color(player_progress_fill_,
                                      playback_paused_ ? lv_color_hex(0xAEB8C5) : accent_color(config_.type), 0);
        }

        report_playback_progress(false);
    }

    void start_player_timer()
    {
        if (player_timer_) lv_timer_delete(player_timer_);
        player_timer_ = lv_timer_create(player_timer_cb, 500, this);
    }

    void render_track_row(int y, const char *name, const std::string &value,
                          const char *key_hint, bool active)
    {
        lv_color_t label_color = active ? accent_color(config_.type) : lv_color_hex(0x7F8D9D);
        label(root_, name, 10, y, 42, 16, label_color);
        if (should_marquee(value, 198, 14)) {
            marquee_label(root_, value.c_str(), 54, y, 198, 16, lv_color_hex(0xE6EEF7), ui_font_14(), 13);
        } else {
            label(root_, value.c_str(), 54, y, 198, 16, lv_color_hex(0xE6EEF7));
        }
        label(root_, key_hint, 258, y, 52, 16, lv_color_hex(0x95A4B6));
    }

    void render_player()
    {
        view_ = View::Player;
        if (small_screen_playback_) {
            clear(lv_color_hex(0x000000));
            add_to_group(root_);
            lv_group_focus_obj(root_);
            lv_timer_handler();
            player_status_label_ = nullptr;
            player_time_label_ = nullptr;
            player_progress_fill_ = nullptr;
            player_progress_w_ = 0;
            start_player_timer();
            return;
        }

        clear(lv_color_hex(0x020407));
        render_atmosphere_background(current_item_);
        add_to_group(root_);
        lv_group_focus_obj(root_);

        std::string title = detail_breadcrumb(current_item_);
        if (should_marquee(title, 306, 14)) {
            marquee_label(root_, title.c_str(), 7, 4, 306, 18, accent_color(config_.type), ui_font_14(), 14);
        } else {
            label(root_, title.c_str(), 7, 4, 306, 18, accent_color(config_.type));
        }

        std::string mode = playback_.summary.empty() ? "HDMI 输出" : playback_.summary;
        if (!player_.backend().empty()) mode += " / " + player_.backend();
        mode += " / " + std::string(audio_output_label(audio_output_));
        if (small_screen_playback_) mode += " / 小屏";
        if (should_marquee(mode, 306, 14)) {
            marquee_label(root_, mode.c_str(), 7, 25, 306, 18, lv_color_hex(0xDCE5EF), ui_font_14(), 13);
        } else {
            label(root_, mode.c_str(), 7, 25, 306, 18, lv_color_hex(0xDCE5EF));
        }

        player_status_label_ = label(root_, "", 8, 47, 114, 18, accent_color(config_.type));
        player_time_label_ = label(root_, "", 128, 47, 184, 18, lv_color_hex(0xF3F7FB));
        lv_obj_set_style_text_align(player_time_label_, LV_TEXT_ALIGN_RIGHT, 0);

        lv_obj_t *bar = lv_obj_create(root_);
        lv_obj_remove_style_all(bar);
        lv_obj_set_pos(bar, 10, 70);
        lv_obj_set_size(bar, 300, 10);
        lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_radius(bar, 5, 0);
        lv_obj_set_style_bg_color(bar, lv_color_hex(0x15202B), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);

        player_progress_fill_ = lv_obj_create(bar);
        lv_obj_remove_style_all(player_progress_fill_);
        lv_obj_set_pos(player_progress_fill_, 0, 0);
        lv_obj_set_size(player_progress_fill_, 8, 10);
        lv_obj_set_style_radius(player_progress_fill_, 5, 0);
        lv_obj_set_style_bg_color(player_progress_fill_, accent_color(config_.type), 0);
        lv_obj_set_style_bg_opa(player_progress_fill_, LV_OPA_COVER, 0);
        player_progress_w_ = 300;

        if (is_photo_item(current_item_)) {
            label(root_, current_language() == UiLanguage::Chinese ? "Z/C 切图" : "Z/C Photo",
                  10, 91, 88, 16, lv_color_hex(0xD3DEE9));
            label(root_, current_language() == UiLanguage::Chinese ? "Esc 停止/列表" : "Esc Stop/List",
                  104, 91, 128, 16, lv_color_hex(0xD3DEE9));
            label(root_, current_language() == UiLanguage::Chinese ? "HDMI 上显示图片，小屏保留控制"
                                                                   : "Photo on HDMI; controls stay here",
                  10, 118, 300, 16, lv_color_hex(0x8F9FAF));
        } else if (is_audio_item(current_item_)) {
            label(root_, current_language() == UiLanguage::Chinese ? "空格 暂停" : "Space Pause",
                  10, 91, 90, 16, lv_color_hex(0xD3DEE9));
            label(root_, current_language() == UiLanguage::Chinese ? "Z/C 快进退" : "Z/C Seek",
                  104, 91, 104, 16, lv_color_hex(0xD3DEE9));
            label(root_, current_language() == UiLanguage::Chinese ? "N 下一首" : "N Next",
                  214, 91, 86, 16, lv_color_hex(0xD3DEE9));
            render_track_row(118, t(TextKey::Output), player_output_label(), "Tab", true);
            render_track_row(139, t(TextKey::Audio),
                             current_item_.album_artist.empty()
                                 ? (current_language() == UiLanguage::Chinese ? "音乐播放" : "Music playback")
                                 : current_item_.album_artist,
                             "Esc", false);
        } else {
            label(root_, current_language() == UiLanguage::Chinese ? "空格暂停  Z/C快进退  N下一集"
                                                                   : "Space Pause  Z/C Seek  N Next",
                  10, 88, 300, 16, lv_color_hex(0xD3DEE9));
            render_track_row(106, t(TextKey::Output), player_output_label(), "Tab", true);
            render_track_row(127, t(TextKey::Audio), active_audio_caption(), "F", false);
            render_track_row(148, t(TextKey::Subtitle), active_subtitle_caption(), "X", false);
        }

        update_player_progress();
        start_player_timer();
    }

    std::string stream_summary(const std::vector<MediaStream> &streams)
    {
        int audio = 0;
        int subs = 0;
        for (const auto &s : streams) {
            if (s.type == "Audio") audio++;
            if (s.type == "Subtitle") subs++;
        }
        return "音轨: " + std::to_string(audio) + "  字幕: " + std::to_string(subs);
    }

    void render_sort()
    {
        view_ = View::Sort;
        clear();
        add_to_group(root_);
        lv_group_focus_obj(root_);
        label(root_, t(TextKey::SortTitle), 8, 8, 160, 18, accent_color(config_.type), ui_font_16());
        SortMode modes[] = {SortMode::LatestUpdated, SortMode::Name, SortMode::PremiereDate,
                            SortMode::Runtime, SortMode::Unwatched};
        for (int i = 0; i < 5; ++i) {
            bool selected = modes[i] == sort_;
            lv_obj_t *b = box(root_, 16, 34 + i * 18, 288, 16,
                              selected ? lv_color_hex(0x12202A) : lv_color_hex(0x080C12),
                              selected ? accent_color(config_.type) : lv_color_hex(0x1A2430));
            label(b, sort_label(modes[i], current_language()), 3, -1, 260, 16,
                  selected ? lv_color_hex(0xFFFFFF) : lv_color_hex(0xB6C3D0));
        }
        label(root_, t(TextKey::SortHelp), 8, 128, 300, 16, lv_color_hex(0x7F8D9D));
    }

    void render_settings()
    {
        view_ = View::Settings;
        clear();
        label(root_, t(TextKey::SettingsTitle), 8, 8, 160, 18, accent_color(config_.type), ui_font_16());
        label(root_, (std::string(t(TextKey::SettingsService)) + server_name(config_.type)).c_str(),
              10, 34, 292, 18, lv_color_hex(0xFFFFFF));
        label(root_, config_.base_url.c_str(), 10, 54, 292, 18, lv_color_hex(0xB7C4D2));
        logout_btn_ = button(root_, t(TextKey::Logout), 10, 84, 92, 22);
        label(root_, t(TextKey::LogoutHelp), 10, 122, 292, 18, lv_color_hex(0x7F8D9D));
        lv_group_focus_obj(logout_btn_);
    }

    bool switch_expanded_season(LibraryState &state, int delta, bool wrap)
    {
        if (!state.expanded || state.seasons.empty() || delta == 0) return false;

        const size_t count = state.seasons.size();
        size_t next = state.season_selected;
        if (delta > 0) {
            if (next + 1 < count) next++;
            else if (wrap) next = 0;
        } else {
            if (next > 0) next--;
            else if (wrap) next = count - 1;
        }
        if (next == state.season_selected) return false;

        state.season_selected = next;
        state.episode_selected = 0;
        last_vertical_dir_ = delta > 0 ? 1 : -1;
        last_horizontal_dir_ = 0;
        ensure_expanded_episodes(state, true);
        play_ui_sound(true);
        render_browse();
        return true;
    }

    static bool is_search_text_key(uint32_t key)
    {
        if (key == STREAMPLAYER_KEY_SUBTITLE ||
            key == STREAMPLAYER_KEY_CTRL_LEFT ||
            key == STREAMPLAYER_KEY_CTRL_RIGHT) {
            return false;
        }
        if (key == LV_KEY_UP || key == LV_KEY_DOWN ||
            key == LV_KEY_LEFT || key == LV_KEY_RIGHT ||
            key == LV_KEY_ENTER || key == LV_KEY_ESC ||
            key == LV_KEY_BACKSPACE || key == LV_KEY_DEL ||
            key == LV_KEY_NEXT || key == LV_KEY_PREV ||
            key == LV_KEY_HOME || key == LV_KEY_END) {
            return false;
        }
        return key >= 0x20 && key <= 0x10FFFF;
    }

    bool handle_expanded_browse_key(uint32_t key)
    {
        LibraryState *state = current_library_state();
        if (!state || !state->expanded) return false;

        if (key == LV_KEY_ESC) {
            reset_expansion(*state);
            last_vertical_dir_ = 0;
            last_horizontal_dir_ = 0;
            render_browse();
            return true;
        }
        if (key == 'm' || key == 'M') {
            render_settings();
            return true;
        }
        if (key == LV_KEY_NEXT || key == '\t') {
            cycle_sort_mode(1);
            return true;
        }
        if (key == LV_KEY_PREV) {
            cycle_sort_mode(-1);
            return true;
        }
        if (key == STREAMPLAYER_KEY_CTRL_RIGHT) {
            switch_expanded_season(*state, 1, true);
            return true;
        }
        if (key == STREAMPLAYER_KEY_CTRL_LEFT) {
            switch_expanded_season(*state, -1, true);
            return true;
        }
        if (key == LV_KEY_LEFT || key == LV_KEY_RIGHT) {
            if (state->season_selected >= state->season_episodes.size()) return true;
            std::vector<MediaItem> &episodes = state->season_episodes[state->season_selected];
            if (episodes.empty()) return true;
            bool moved = false;
            if (key == LV_KEY_RIGHT && state->episode_selected + 1 < episodes.size()) {
                state->episode_selected++;
                moved = true;
                last_horizontal_dir_ = 1;
            } else if (key == LV_KEY_LEFT && state->episode_selected > 0) {
                state->episode_selected--;
                moved = true;
                last_horizontal_dir_ = -1;
            }
            if (moved) {
                last_vertical_dir_ = 0;
                play_ui_sound(false);
                render_browse();
            }
            return true;
        }
        if (key == LV_KEY_ENTER) {
            play_selected();
            return true;
        }
        if (key == ' ') {
            render_detail();
            return true;
        }
        return true;
    }

    void handle_search_key(uint32_t raw_key, uint32_t key)
    {
        if (key == LV_KEY_ESC) {
            render_browse();
            return;
        }
        if (key == LV_KEY_BACKSPACE || key == LV_KEY_DEL || raw_key == '\b' || raw_key == 127) {
            erase_last_utf8_char(search_query_);
            reset_search_results();
            render_search();
            return;
        }
        if (key == LV_KEY_ENTER) {
            if (trim(search_query_).empty()) return;
            if (search_dirty_ || search_results_.empty()) {
                run_search(true);
                return;
            }
            if (search_selected_ < search_results_.size()) {
                play_media_item(search_results_[search_selected_], false);
            }
            return;
        }
        if (is_search_text_key(raw_key)) {
            append_utf8_codepoint(search_query_, raw_key);
            reset_search_results();
            render_search();
            return;
        }
        if (key == LV_KEY_RIGHT) {
            if (search_selected_ + 1 >= search_results_.size() && !search_exhausted_) {
                append_search_results(true);
            }
            if (search_selected_ + 1 < search_results_.size()) {
                search_selected_++;
                play_ui_sound(false);
            }
            render_search();
            return;
        }
        if (key == LV_KEY_LEFT) {
            if (search_selected_ > 0) {
                search_selected_--;
                play_ui_sound(false);
            }
            render_search();
            return;
        }
    }

    void handle_setup_key(uint32_t key)
    {
        lv_obj_t *focused = focused_obj();

        if (key == LV_KEY_NEXT || key == LV_KEY_DOWN || key == '\t') {
            if (group_) lv_group_focus_next(group_);
            update_setup_focus_marker();
            return;
        }
        if (key == LV_KEY_PREV || key == LV_KEY_UP) {
            if (group_) lv_group_focus_prev(group_);
            update_setup_focus_marker();
            return;
        }
        if ((key == LV_KEY_LEFT || key == LV_KEY_RIGHT) && focused == server_dropdown_) {
            toggle_server_type();
            return;
        }
        if ((key == LV_KEY_LEFT || key == LV_KEY_RIGHT) && focused == language_dropdown_) {
            toggle_language();
            return;
        }
        if (key == LV_KEY_ENTER) {
            if (focused == server_dropdown_) {
                toggle_server_type();
            } else if (focused == language_dropdown_) {
                toggle_language();
            } else if (focused == login_btn_ || focused == pass_ta_) {
                login_from_setup();
            } else {
                if (group_) lv_group_focus_next(group_);
                update_setup_focus_marker();
            }
        }
    }

    void handle_browse_key(uint32_t key)
    {
        if (handle_expanded_browse_key(key)) return;
        LibraryState *state = current_library_state();
        const bool settings_selected = library_index_ < libraries_.size() &&
                                       library_is_settings(libraries_[library_index_]);
        if (settings_selected && (key == LV_KEY_ENTER || key == ' ' || key == 'm' || key == 'M')) {
            render_settings();
            return;
        }
        if (settings_selected && (key == LV_KEY_NEXT || key == LV_KEY_PREV || key == '\t')) {
            return;
        }
        if (key == LV_KEY_NEXT || key == '\t') {
            cycle_sort_mode(1);
        } else if (key == LV_KEY_PREV) {
            cycle_sort_mode(-1);
        } else if (key == LV_KEY_RIGHT && state && !state->items.empty()) {
            bool moved = false;
            if (state->selected + 1 < state->items.size()) {
                state->selected++;
                moved = true;
            } else if (!state->exhausted) {
                append_items_for_library(library_index_, true);
                if (state->selected + 1 < state->items.size()) {
                    state->selected++;
                    moved = true;
                }
            }
            if (moved) {
                last_horizontal_dir_ = 1;
                last_vertical_dir_ = 0;
                maybe_lazyload_current_library();
                play_ui_sound(false);
                render_browse();
            }
        } else if (key == LV_KEY_LEFT && state && !state->items.empty()) {
            if (state->selected > 0) {
                state->selected--;
                last_horizontal_dir_ = -1;
                last_vertical_dir_ = 0;
                play_ui_sound(false);
                render_browse();
            }
        } else if (key == LV_KEY_DOWN && !libraries_.empty()) {
            if (library_index_ + 1 < libraries_.size()) {
                library_index_++;
                last_vertical_dir_ = 1;
                last_horizontal_dir_ = 0;
                if (load_items_for_current_library(false)) play_ui_sound(true);
            }
        } else if (key == LV_KEY_UP && !libraries_.empty()) {
            if (library_index_ > 0) {
                library_index_--;
                last_vertical_dir_ = -1;
                last_horizontal_dir_ = 0;
                if (load_items_for_current_library(false)) play_ui_sound(true);
            }
        } else if (key == LV_KEY_ENTER) {
            const MediaItem *item = selected_item();
            if (item && is_series_item(*item)) expand_selected_series();
            else play_selected();
        } else if (key == ' ') {
            const MediaItem *item = selected_item();
            if (item && is_series_item(*item)) expand_selected_series();
            else render_detail();
        } else if (key == 's' || key == 'S') {
            render_sort();
        } else if (key == 'm' || key == 'M') {
            render_settings();
        }
    }

    void handle_detail_key(uint32_t key)
    {
        if (key == LV_KEY_ENTER) play_selected();
        else if (key == LV_KEY_UP || key == LV_KEY_DOWN) {
            int delta = key == LV_KEY_DOWN ? 1 : -1;
            if (select_adjacent_item(delta)) {
                last_vertical_dir_ = delta > 0 ? 1 : -1;
                last_horizontal_dir_ = 0;
                play_ui_sound(true);
                render_detail();
            }
        }
        else if (key == ' ' || key == LV_KEY_ESC) render_browse();
    }

    void cycle_audio_selection()
    {
        std::vector<size_t> tracks = stream_indices("Audio");
        if (tracks.empty()) {
            set_player_notice("没有可切换音轨");
            update_player_progress();
            return;
        }

        auto it = std::find(tracks.begin(), tracks.end(), selected_audio_stream_);
        if (it == tracks.end() || ++it == tracks.end()) selected_audio_stream_ = tracks.front();
        else selected_audio_stream_ = *it;

        bool ok = player_.cycle_audio();
        std::string notice = std::string("音轨 ") + active_audio_caption();
        if (!ok) restart_video_playback_at(current_playback_seconds(), notice);
        else {
            set_player_notice(notice);
            report_playback_progress(true);
        }
        render_player();
    }

    void cycle_subtitle_selection()
    {
        std::vector<size_t> tracks = stream_indices("Subtitle");
        if (tracks.empty()) {
            set_player_notice("没有字幕轨");
            update_player_progress();
            return;
        }

        if (selected_subtitle_stream_ == kNoStream) {
            selected_subtitle_stream_ = tracks.front();
        } else {
            auto it = std::find(tracks.begin(), tracks.end(), selected_subtitle_stream_);
            if (it == tracks.end() || ++it == tracks.end()) selected_subtitle_stream_ = kNoStream;
            else selected_subtitle_stream_ = *it;
        }

        bool ok = player_.cycle_subtitle();
        std::string notice = std::string("字幕 ") + active_subtitle_caption();
        if (!ok) restart_video_playback_at(current_playback_seconds(), notice);
        else {
            set_player_notice(notice);
            report_playback_progress(true);
        }
        render_player();
    }

    void handle_player_key(uint32_t key)
    {
        const bool output_switch_key = key == LV_KEY_NEXT || key == '\t';
        if (is_photo_item(current_item_)) {
            if (output_switch_key) cycle_playback_output();
            else if (key == LV_KEY_LEFT) play_adjacent_item(-1);
            else if (key == LV_KEY_RIGHT || key == 'n' || key == 'N') play_adjacent_item(1);
            else if (key == LV_KEY_ESC) {
                stop_playback_to_detail();
            }
            return;
        }

        if (output_switch_key) {
            cycle_playback_output();
            return;
        }

        if (key == 'o' || key == 'O') {
            cycle_audio_output();
            return;
        }

        if (key == ' ') {
            long long pos = current_playback_seconds();
            if (player_.pause()) {
                playback_paused_ = player_.paused();
                if (playback_paused_) {
                    playback_paused_seconds_ = pos;
                    set_player_notice("已暂停");
                } else {
                    playback_seek_seconds_ = playback_paused_seconds_;
                    playback_started_at_ = std::time(nullptr);
                    set_player_notice("继续播放");
                }
            } else {
                set_player_notice("播放器不支持暂停");
            }
            report_playback_progress(true);
            update_player_progress();
        }
        else if (key == LV_KEY_LEFT || key == LV_KEY_RIGHT) {
            int delta = key == LV_KEY_LEFT ? -10 : 30;
            if (player_.seek(delta)) {
                adjust_playback_position(delta);
                set_player_notice(delta < 0 ? "快退 10 秒" : "快进 30 秒");
                report_playback_progress(true);
            } else if (!is_audio_item(current_item_) && !is_photo_item(current_item_)) {
                long long next = clamped_playback_position(current_playback_seconds() + delta);
                restart_video_playback_at(next, std::string("跳转到 ") + seconds_to_clock(next));
            } else {
                set_player_notice("当前后端不支持精确快进");
            }
            update_player_progress();
        }
        else if (is_audio_item(current_item_) && (key == 'n' || key == 'N')) play_adjacent_item(1);
        else if (!is_audio_item(current_item_) && key == LV_KEY_UP) cycle_audio_selection();
        else if (!is_audio_item(current_item_) && key == LV_KEY_DOWN) cycle_subtitle_selection();
        else if (!is_audio_item(current_item_) && (key == 'n' || key == 'N')) play_next_episode();
        else if (key == LV_KEY_ESC) {
            stop_playback_to_detail();
        }
    }

    void handle_sort_key(uint32_t key)
    {
        if (key == LV_KEY_DOWN || key == LV_KEY_RIGHT) {
            sort_ = static_cast<SortMode>((static_cast<int>(sort_) + 1) % 5);
            render_sort();
        } else if (key == LV_KEY_UP || key == LV_KEY_LEFT) {
            sort_ = static_cast<SortMode>((static_cast<int>(sort_) + 4) % 5);
            render_sort();
        } else if (key == LV_KEY_ENTER) {
            reset_library_states();
            load_items_for_current_library(true);
        } else if (key == LV_KEY_ESC) {
            render_browse();
        }
    }

    void handle_settings_key(uint32_t key)
    {
        if (key == LV_KEY_ESC) render_browse();
        else if (key == LV_KEY_ENTER) logout();
    }

    bool select_adjacent_item(int delta)
    {
        LibraryState *state = current_library_state();
        if (!state || delta == 0) return false;

        if (state->expanded && state->season_selected < state->season_episodes.size()) {
            std::vector<MediaItem> &episodes = state->season_episodes[state->season_selected];
            if (episodes.empty()) return false;
            if (delta > 0 && state->episode_selected + 1 < episodes.size()) {
                state->episode_selected++;
                return true;
            }
            if (delta < 0 && state->episode_selected > 0) {
                state->episode_selected--;
                return true;
            }
            return false;
        }

        if (state->items.empty()) return false;
        if (delta > 0) {
            if (state->selected + 1 < state->items.size()) {
                state->selected++;
                return true;
            }
            if (!state->exhausted && append_items_for_library(library_index_, true) &&
                state->selected + 1 < state->items.size()) {
                state->selected++;
                return true;
            }
        } else if (state->selected > 0) {
            state->selected--;
            return true;
        }
        return false;
    }

    void play_adjacent_item(int delta)
    {
        if (!select_adjacent_item(delta)) return;
        last_horizontal_dir_ = delta > 0 ? 1 : -1;
        last_vertical_dir_ = 0;
        play_selected();
    }

    void play_selected()
    {
        const MediaItem *item = selected_item();
        if (!item) return;
        if (is_series_item(*item)) {
            expand_selected_series();
            return;
        }
        play_media_item(*item, false);
    }

    void play_pending_on_small_screen()
    {
        if (!has_pending_small_screen_item_) return;
        uint64_t now = monotonic_ms();
        if (small_screen_prompt_ready_at_ms_ && now < small_screen_prompt_ready_at_ms_) {
            app_log("Small-screen prompt ignored early confirm");
            return;
        }
        MediaItem item = pending_small_screen_item_;
        has_pending_small_screen_item_ = false;
        small_screen_prompt_ready_at_ms_ = 0;
        play_media_item(item, true);
    }

    void play_media_item(const MediaItem &item, bool force_small_screen)
    {
        const bool small_screen_playback = force_small_screen && video_item_needs_hdmi(item);
        if (video_item_needs_hdmi(item) && !small_screen_playback && !hdmi_output_connected()) {
            show_hdmi_required_prompt(item);
            return;
        }
        report_playback_stopped();
        player_.stop();
        current_item_ = item;
        small_screen_playback_ = small_screen_playback;
        if (small_screen_playback_) {
            view_ = View::Loading;
            clear(lv_color_hex(0x000000));
            add_to_group(root_);
            lv_group_focus_obj(root_);
            lv_timer_handler();
        } else {
            show_loading(is_audio_item(current_item_) ? t(TextKey::PreparingMusic) :
                         (is_photo_item(current_item_) ? t(TextKey::PreparingPhoto)
                                                       : t(TextKey::RequestTranscode)));
        }
        std::string error;
        playback_ = PlaybackChoice();
        bool keep_open = false;
        if (is_audio_item(current_item_)) {
            playback_.url = client_->audio_stream_url(current_item_);
            playback_.direct_playable = true;
            playback_.summary = "Audio stream";
        } else if (is_photo_item(current_item_)) {
            playback_.url = client_->photo_view_url(current_item_);
            playback_.direct_playable = true;
            playback_.summary = "HDMI photo";
            keep_open = true;
        } else {
            if (!client_->playback_info(current_item_, playback_, error, small_screen_playback_)) {
                show_error(error);
                return;
            }
        }
        if (playback_.url.empty()) {
            show_error(t(TextKey::NoPlayable));
            return;
        }
        bool video_mode = !is_audio_item(current_item_) && !is_photo_item(current_item_);
        if (small_screen_playback_) audio_output_ = AudioOutput::Speaker;
        if (!player_.start(playback_.url, config_.access_token, error, keep_open, video_mode,
                           audio_output_, 0, small_screen_playback_)) {
            show_error(error);
            app_log("Player start failed item=" + current_item_.id + " error=" + error);
            return;
        }
        begin_player_session();
        app_log("Player started item=" + current_item_.id + " backend=" + player_.backend() +
                " summary=" + playback_.summary +
                " audio_output=" + audio_output_label(audio_output_) +
                " small_screen=" + bool_text(small_screen_playback_) +
                " url=" + playback_.url);
        report_playback_started();
        render_player();
    }

    void play_next_episode()
    {
        MediaItem next;
        std::string error;
        if (!client_->load_next_episode(current_item_, next, error)) {
            show_error(error);
            return;
        }
        play_media_item(next, small_screen_playback_);
    }

    void logout()
    {
        report_playback_stopped();
        player_.stop();
        clear_config(&config_);
        ServerConfig remembered = config_;
        remembered.access_token.clear();
        remembered.user_id.clear();
        config_ = remembered;
        delete client_;
        client_ = new MediaServerClient(config_);
        libraries_.clear();
        library_states_.clear();
        show_setup();
    }

    ServerConfig config_;
    bool config_loaded_ = false;
    MediaServerClient *client_ = nullptr;
    HdmiPlayer player_;
    View view_ = View::Setup;
    SortMode sort_ = SortMode::LatestUpdated;
    std::vector<MediaLibrary> libraries_;
    std::vector<LibraryState> library_states_;
    size_t library_index_ = 0;
    std::string search_query_;
    std::vector<MediaItem> search_results_;
    size_t search_selected_ = 0;
    int search_loaded_ = 0;
    bool search_exhausted_ = false;
    bool search_dirty_ = false;
    std::string search_error_;
    int last_vertical_dir_ = 0;
    int last_horizontal_dir_ = 0;
    MediaItem current_item_;
    MediaItem pending_small_screen_item_;
    PlaybackChoice playback_;
    time_t playback_started_at_ = 0;
    long long playback_seek_seconds_ = 0;
    long long playback_paused_seconds_ = 0;
    bool playback_paused_ = false;
    bool playback_reported_started_ = false;
    bool small_screen_playback_ = false;
    bool has_pending_small_screen_item_ = false;
    uint64_t small_screen_prompt_ready_at_ms_ = 0;
    time_t last_progress_report_at_ = 0;
    AudioOutput audio_output_ = AudioOutput::Speaker;
    size_t selected_audio_stream_ = kNoStream;
    size_t selected_subtitle_stream_ = kNoStream;
    std::string player_notice_;
    time_t player_notice_until_ = 0;
    uint64_t esc_hold_started_ms_ = 0;
    uint64_t last_esc_key_ms_ = 0;
    bool esc_exit_armed_ = false;

    lv_obj_t *root_ = nullptr;
    lv_group_t *group_ = nullptr;
    lv_obj_t *server_dropdown_ = nullptr;
    lv_obj_t *language_dropdown_ = nullptr;
    lv_obj_t *login_btn_ = nullptr;
    lv_obj_t *logout_btn_ = nullptr;
    lv_obj_t *small_screen_btn_ = nullptr;
    lv_obj_t *url_ta_ = nullptr;
    lv_obj_t *user_ta_ = nullptr;
    lv_obj_t *pass_ta_ = nullptr;
    lv_obj_t *status_ = nullptr;
    lv_obj_t *setup_focus_marker_ = nullptr;
    lv_timer_t *player_timer_ = nullptr;
    lv_obj_t *player_status_label_ = nullptr;
    lv_obj_t *player_time_label_ = nullptr;
    lv_obj_t *player_progress_fill_ = nullptr;
    int player_progress_w_ = 0;
    lv_timer_t *esc_short_timer_ = nullptr;
    lv_timer_t *control_timer_ = nullptr;
    int control_fd_ = -1;
    int control_keepalive_fd_ = -1;
    std::string control_buffer_;
};

static MediaController g_controller;

}  // namespace

extern "C" void media_controller_init(void)
{
    g_controller.init();
}

extern "C" void media_controller_attach_keyboard(lv_indev_t *preferred)
{
    g_controller.attach_keyboard(preferred);
}
