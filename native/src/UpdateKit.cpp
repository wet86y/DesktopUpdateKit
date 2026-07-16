#include "DesktopUpdateKit/UpdateKit.h"

#include "Json.h"
#include "NodeSelection.h"

#include <windows.h>
#include <bcrypt.h>
#include <winhttp.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstring>
#include <cwctype>
#include <cmath>
#include <deque>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <ranges>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <tuple>

namespace desktop_update_kit {

std::size_t detail::next_accelerated_node_index(
    std::span<const DownloadNode> nodes, std::size_t current_index) noexcept {
    if (nodes.empty()) return 0;
    for (std::size_t offset = 1; offset <= nodes.size(); ++offset) {
        const auto index = (current_index + offset) % nodes.size();
        if (nodes[index].id != "github-direct") return index;
    }
    return nodes.size();
}

namespace {

using namespace std::chrono_literals;
constexpr std::uint64_t kProbeBytes = 64 * 1024;
constexpr std::chrono::seconds kNodeCacheLifetime = std::chrono::hours(24 * 7);

std::wstring wide(const std::string& value) {
    if (value.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    if (count <= 0) throw std::runtime_error("Invalid UTF-8 text");
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
        result.data(), count);
    return result;
}

std::string narrow(const std::wstring& value) {
    if (value.empty()) return {};
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (count <= 0) throw std::runtime_error("Invalid UTF-16 text");
    std::string result(static_cast<std::size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
        result.data(), count, nullptr, nullptr);
    return result;
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Unable to read file");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void write_all(std::ofstream& output, std::span<const std::byte> bytes) {
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!output) throw std::runtime_error("Unable to write update file");
}

std::string trim_hash(std::string value) {
    if (value.starts_with("\xEF\xBB\xBF")) value.erase(0, 3);
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos || value.size() - first < 64) return {};
    value = value.substr(first, 64);
    if (!std::all_of(value.begin(), value.end(), [](unsigned char character) { return std::isxdigit(character) != 0; })) return {};
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

bool valid_sha256(const std::string& value) {
    if (value.size() != 64) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char character) {
        return std::isxdigit(character) != 0;
    });
}

bool valid_repository(const std::string& value) {
    const auto slash = value.find('/');
    if (slash == 0 || slash == std::string::npos || slash + 1 >= value.size() || value.find('/', slash + 1) != std::string::npos)
        return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char character) {
        return std::isalnum(character) || character == '.' || character == '_' || character == '-' || character == '/';
    });
}

bool valid_asset_name(const std::string& value) {
    return !value.empty() && value.size() <= 255 && value != "." && value != ".." &&
        value.find('/') == std::string::npos && value.find('\\') == std::string::npos &&
        value.find(':') == std::string::npos;
}

bool valid_tag(const std::string& value) {
    return !value.empty() && value.size() <= 128 && value != "." && value != ".." &&
        value.find('/') == std::string::npos && value.find('\\') == std::string::npos &&
        value.find("..") == std::string::npos;
}

std::string url_encode_segment(const std::string& value) {
    static constexpr char digits[] = "0123456789ABCDEF";
    std::string result;
    for (const unsigned char character : value) {
        if (std::isalnum(character) || character == '-' || character == '_' || character == '.' || character == '~') {
            result.push_back(static_cast<char>(character));
        } else {
            result.push_back('%');
            result.push_back(digits[character >> 4]);
            result.push_back(digits[character & 15]);
        }
    }
    return result;
}

std::string official_url(const ClientOptions& options, const Release& release, bool hash) {
    return "https://github.com/" + options.repository + "/releases/download/" +
        url_encode_segment(release.tag) + "/" + url_encode_segment(hash ? release.sha256_asset : release.asset);
}

std::string expand_node(const DownloadNode& node, const std::string& official) {
    auto result = node.url_template;
    const auto marker = result.find("{url}");
    if (marker == std::string::npos || result.find("{url}", marker + 5) != std::string::npos)
        throw std::runtime_error("Invalid download node template");
    result.replace(marker, 5, official);
    return result;
}

std::optional<DownloadNode> normalize_node(const DownloadNode& input) {
    DownloadNode node = input;
    if (node.id.empty() || node.id.size() > 64 || !std::all_of(node.id.begin(), node.id.end(), [](unsigned char character) {
        return std::isalnum(character) || character == '-' || character == '_';
    })) return {};
    if (node.id == "github-direct") {
        if (node.url_template != "{url}" || !node.enabled) return {};
        return DownloadNode{"github-direct", "{url}", 1000, true};
    }
    const auto marker = node.url_template.find("{url}");
    if (marker == std::string::npos || node.url_template.find("{url}", marker + 5) != std::string::npos ||
        !node.url_template.starts_with("https://")) return {};
    node.priority = std::clamp(node.priority, -10000, 10000);
    return node;
}

std::vector<DownloadNode> normalize_nodes(const std::vector<DownloadNode>& nodes) {
    std::vector<DownloadNode> result;
    std::set<std::string> ids;
    for (const auto& input : nodes) {
        auto node = normalize_node(input);
        if (node && ids.insert(node->id).second) result.push_back(*node);
    }
    result.erase(std::remove_if(result.begin(), result.end(), [](const DownloadNode& node) {
        return node.id == "github-direct";
    }), result.end());
    result.push_back({"github-direct", "{url}", 1000, true});
    std::stable_sort(result.begin(), result.end(), [](const DownloadNode& left, const DownloadNode& right) {
        return left.priority < right.priority;
    });
    return result;
}

std::filesystem::path local_app_data() {
    std::wstring value(32768, L'\0');
    const DWORD count = GetEnvironmentVariableW(L"LOCALAPPDATA", value.data(), static_cast<DWORD>(value.size()));
    if (count == 0 || count >= value.size()) return std::filesystem::temp_directory_path();
    value.resize(count);
    return value;
}

std::wstring safe_application_id(const std::wstring& input) {
    std::wstring value;
    for (const wchar_t character : input) {
        if (std::iswalnum(character) || character == L'-' || character == L'_') value.push_back(character);
    }
    return value.empty() ? L"desktop-update-kit" : value;
}

std::int64_t unix_now() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

struct NodeCache {
    std::vector<DownloadNode> nodes;
    std::int64_t updated{};
    std::string last_success;
};

std::filesystem::path cache_path(const ClientOptions& options) {
    return local_app_data() / safe_application_id(options.application_id) / L"update-node-cache.json";
}

NodeCache load_cache(const ClientOptions& options) noexcept {
    try {
        const auto path = cache_path(options);
        if (!std::filesystem::exists(path)) return {};
        json::Value root;
        std::string error;
        if (!json::parse(read_file(path), root, error)) return {};
        const auto* object = json::object(&root);
        if (!object) return {};
        NodeCache result;
        if (auto number = json::number(json::member(*object, "updated"))) result.updated = static_cast<std::int64_t>(*number);
        if (auto value = json::string(json::member(*object, "lastSuccess"))) result.last_success = *value;
        if (const auto* items = json::array(json::member(*object, "nodes"))) {
            for (const auto& item : *items) {
                const auto* node_object = json::object(&item);
                if (!node_object) continue;
                const auto* id = json::string(json::member(*node_object, "id"));
                const auto* url = json::string(json::member(*node_object, "template"));
                const auto priority = json::number(json::member(*node_object, "priority"));
                const auto enabled = json::boolean(json::member(*node_object, "enabled"));
                if (id && url && priority && enabled) result.nodes.push_back({*id, *url, static_cast<int>(*priority), *enabled});
            }
        }
        if (unix_now() - result.updated > kNodeCacheLifetime.count()) result.nodes.clear();
        return result;
    } catch (...) {
        return {};
    }
}

std::string json_escape(const std::string& input) {
    std::string result;
    for (const unsigned char character : input) {
        switch (character) {
        case '"': result += "\\\""; break;
        case '\\': result += "\\\\"; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:
            if (character < 0x20) {
                std::ostringstream encoded;
                encoded << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(character);
                result += encoded.str();
            } else result.push_back(static_cast<char>(character));
        }
    }
    return result;
}

void save_cache(const ClientOptions& options, const std::vector<DownloadNode>& nodes, const std::string& success) noexcept {
    try {
        const auto path = cache_path(options);
        std::filesystem::create_directories(path.parent_path());
        const auto temporary = path.wstring() + L".tmp";
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output << "{\"version\":1,\"updated\":" << unix_now() << ",\"lastSuccess\":\""
               << json_escape(success) << "\",\"nodes\":[";
        bool first = true;
        for (const auto& node : nodes) {
            if (!first) output << ',';
            first = false;
            output << "{\"id\":\"" << json_escape(node.id) << "\",\"template\":\""
                   << json_escape(node.url_template) << "\",\"priority\":" << node.priority
                   << ",\"enabled\":" << (node.enabled ? "true" : "false") << '}';
        }
        output << "]}";
        output.close();
        if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            std::filesystem::remove(temporary);
    } catch (...) {
    }
}

std::vector<DownloadNode> resolve_nodes(const ClientOptions& options, const Release& release, bool acceleration) {
    if (!acceleration) return {{"github-direct", "{url}", 1000, true}};
    auto cache = load_cache(options);
    auto nodes = normalize_nodes(release.nodes.empty() ? cache.nodes : release.nodes);
    nodes.erase(std::remove_if(nodes.begin(), nodes.end(), [](const DownloadNode& node) { return !node.enabled; }), nodes.end());
    if (!cache.last_success.empty()) {
        const auto found = std::find_if(nodes.begin(), nodes.end(), [&](const DownloadNode& node) {
            return node.id == cache.last_success;
        });
        if (found != nodes.end() && found != nodes.begin()) std::rotate(nodes.begin(), found, found + 1);
    }
    if (std::none_of(nodes.begin(), nodes.end(), [](const DownloadNode& node) { return node.id == "github-direct"; }))
        nodes.push_back({"github-direct", "{url}", 1000, true});
    return nodes;
}

std::optional<Release> parse_release(const std::string& source, const ClientOptions& options, std::string& error) {
    json::Value root;
    if (!json::parse(source, root, error)) return {};
    const auto* object = json::object(&root);
    if (!object) { error = "Manifest must be an object"; return {}; }
    auto required = [&](const char* name) -> const std::string* { return json::string(json::member(*object, name)); };
    const auto* version_text = required("version");
    const auto* repository = required("repository");
    const auto* tag = required("tag");
    const auto* asset = required("asset");
    const auto* sha_asset = required("sha256Asset");
    const auto* sha = required("sha256");
    const auto size = json::number(json::member(*object, "size"));
    if (!version_text || !repository || !tag || !asset || !sha_asset || !sha || !size || *size < 1 ||
        *size > static_cast<double>((std::numeric_limits<std::uint64_t>::max)())) {
        error = "Manifest is missing required fields";
        return {};
    }
    auto version = parse_version(*version_text);
    if (!version) { error = "Manifest version is invalid"; return {}; }
    if (*repository != options.repository || *asset != options.exe_asset_name || *sha_asset != options.sha256_asset_name ||
        !valid_repository(*repository) || !valid_tag(*tag) || !valid_asset_name(*asset) ||
        !valid_asset_name(*sha_asset) || !valid_sha256(*sha)) {
        error = "Manifest does not match the configured release contract";
        return {};
    }
    Release release{*version, *tag, *repository, *asset, *sha_asset, trim_hash(*sha), static_cast<std::uint64_t>(*size)};
    if (const auto* notes = required("releaseNotes")) release.notes = notes->substr(0, 1800);
    const auto* nodes = json::array(json::member(*object, "downloadNodes"));
    if (!nodes || nodes->empty()) { error = "Manifest download nodes are missing"; return {}; }
    std::set<std::string> node_ids;
    bool has_official{};
    for (const auto& item : *nodes) {
        const auto* node_object = json::object(&item);
        if (!node_object) { error = "Manifest download node must be an object"; return {}; }
        const auto* id = json::string(json::member(*node_object, "id"));
        const auto* url = json::string(json::member(*node_object, "template"));
        const auto priority = json::number(json::member(*node_object, "priority"));
        const auto enabled = json::boolean(json::member(*node_object, "enabled"));
        if (!id || !url || !priority || !enabled || std::floor(*priority) != *priority ||
            *priority < -10000 || *priority > 10000 || !node_ids.insert(*id).second) {
            error = "Manifest download node fields are invalid";
            return {};
        }
        auto node = normalize_node({*id, *url, static_cast<int>(*priority), *enabled});
        if (!node) { error = "Manifest download node is insecure or invalid"; return {}; }
        has_official = has_official || node->id == "github-direct";
        release.nodes.push_back(std::move(*node));
    }
    if (!has_official) { error = "Manifest must include the official direct node"; return {}; }
    std::stable_sort(release.nodes.begin(), release.nodes.end(), [](const DownloadNode& left, const DownloadNode& right) {
        return left.priority < right.priority;
    });
    return release;
}

std::optional<std::uint64_t> parse_unsigned(std::wstring_view value) {
    std::uint64_t result{};
    const auto text = narrow(std::wstring(value));
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) return {};
    return result;
}

std::optional<ContentRange> parse_content_range(std::wstring value) {
    // bytes first-last/total
    if (!value.starts_with(L"bytes ")) return {};
    const auto dash = value.find(L'-', 6);
    const auto slash = value.find(L'/', dash == std::wstring::npos ? 6 : dash + 1);
    if (dash == std::wstring::npos || slash == std::wstring::npos) return {};
    auto first = parse_unsigned(std::wstring_view(value).substr(6, dash - 6));
    auto last = parse_unsigned(std::wstring_view(value).substr(dash + 1, slash - dash - 1));
    auto total = parse_unsigned(std::wstring_view(value).substr(slash + 1));
    if (!first || !last || !total || *last < *first || *total <= *last) return {};
    return ContentRange{*first, *last, *total};
}

std::wstring query_header(HINTERNET request, DWORD query) {
    DWORD bytes{};
    WinHttpQueryHeaders(request, query, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &bytes, WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes < sizeof(wchar_t)) return {};
    std::wstring value(bytes / sizeof(wchar_t), L'\0');
    if (!WinHttpQueryHeaders(request, query, WINHTTP_HEADER_NAME_BY_INDEX, value.data(), &bytes, WINHTTP_NO_HEADER_INDEX)) return {};
    value.resize(bytes / sizeof(wchar_t));
    while (!value.empty() && value.back() == L'\0') value.pop_back();
    return value;
}

class WinHttpTransport final : public HttpTransport {
public:
    explicit WinHttpTransport(const std::wstring& user_agent) {
        session_ = WinHttpOpen(user_agent.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session_) throw std::runtime_error("WinHTTP initialization failed");
    }

    ~WinHttpTransport() override {
        abort_active();
        if (session_) WinHttpCloseHandle(session_);
    }

    HttpResponse get(const HttpRequest& input, const ChunkSink& sink, std::stop_token stop_token) override {
        const auto url = wide(input.url);
        URL_COMPONENTS parts{sizeof(parts)};
        parts.dwSchemeLength = static_cast<DWORD>(-1);
        parts.dwHostNameLength = static_cast<DWORD>(-1);
        parts.dwUrlPathLength = static_cast<DWORD>(-1);
        parts.dwExtraInfoLength = static_cast<DWORD>(-1);
        if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &parts) ||
            parts.nScheme != INTERNET_SCHEME_HTTPS) throw std::runtime_error("Invalid or insecure update URL");
        const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
        std::wstring object(parts.lpszUrlPath, parts.dwUrlPathLength);
        if (parts.dwExtraInfoLength) object.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
        HINTERNET connection = WinHttpConnect(session_, host.c_str(), parts.nPort, 0);
        if (!connection) throw std::runtime_error("Unable to connect to update server");
        HINTERNET raw_request = WinHttpOpenRequest(connection, L"GET", object.c_str(), nullptr,
            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        if (!raw_request) {
            WinHttpCloseHandle(connection);
            throw std::runtime_error("Unable to create update request");
        }
        auto active = std::make_shared<ActiveRequest>();
        active->handle.store(raw_request);
        {
            std::scoped_lock lock(active_mutex_);
            active_.push_back(active);
        }
        const auto cleanup = [&] {
            if (auto handle = active->handle.exchange(nullptr)) WinHttpCloseHandle(handle);
            std::scoped_lock lock(active_mutex_);
            std::erase(active_, active);
            WinHttpCloseHandle(connection);
        };
        try {
            const DWORD connect = static_cast<DWORD>(std::clamp<std::int64_t>(input.connect_timeout.count(), 1, INT_MAX));
            const DWORD idle = static_cast<DWORD>(std::clamp<std::int64_t>(input.idle_timeout.count(), 1, INT_MAX));
            WinHttpSetTimeouts(raw_request, connect, connect, connect, idle);
            DWORD redirect_policy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
            WinHttpSetOption(raw_request, WINHTTP_OPTION_REDIRECT_POLICY, &redirect_policy, sizeof(redirect_policy));
            std::wstring headers = L"Cache-Control: no-cache\r\nAccept: application/octet-stream, application/json, text/plain\r\n";
            if (input.range) headers += L"Range: bytes=" + std::to_wstring(input.range->first) + L"-" + std::to_wstring(input.range->second) + L"\r\n";
            if (stop_token.stop_requested() ||
                !WinHttpSendRequest(raw_request, headers.c_str(), static_cast<DWORD>(headers.size()), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
                !WinHttpReceiveResponse(raw_request, nullptr)) throw std::runtime_error("Update request failed");
            HttpResponse response;
            DWORD status{};
            DWORD size = sizeof(status);
            if (!WinHttpQueryHeaders(raw_request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                    WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX))
                throw std::runtime_error("Update response status is missing");
            response.status = status;
            const auto type = query_header(raw_request, WINHTTP_QUERY_CONTENT_TYPE);
            if (!type.empty()) response.content_type = narrow(type);
            const auto length = query_header(raw_request, WINHTTP_QUERY_CONTENT_LENGTH);
            if (!length.empty()) response.content_length = parse_unsigned(length);
            const auto range = query_header(raw_request, WINHTTP_QUERY_CONTENT_RANGE);
            if (!range.empty()) response.content_range = parse_content_range(range);
            std::vector<std::byte> buffer(512 * 1024);
            std::uint64_t delivered{};
            for (;;) {
                if (stop_token.stop_requested()) throw std::runtime_error("Download cancelled");
                DWORD available{};
                if (!WinHttpQueryDataAvailable(raw_request, &available)) throw std::runtime_error("Update response interrupted");
                if (available == 0) break;
                DWORD read{};
                const DWORD request_bytes = static_cast<DWORD>(std::min<std::size_t>(buffer.size(), available));
                if (!WinHttpReadData(raw_request, buffer.data(), request_bytes, &read)) throw std::runtime_error("Update response read failed");
                if (read == 0) break;
                std::size_t use = read;
                if (input.max_body_bytes && delivered + use > *input.max_body_bytes)
                    use = static_cast<std::size_t>(*input.max_body_bytes - delivered);
                if (use) sink(std::span<const std::byte>(buffer.data(), use));
                delivered += use;
                if (input.max_body_bytes && delivered >= *input.max_body_bytes) break;
            }
            cleanup();
            return response;
        } catch (...) {
            cleanup();
            throw;
        }
    }

    void abort_active() noexcept override {
        std::vector<std::shared_ptr<ActiveRequest>> copy;
        {
            std::scoped_lock lock(active_mutex_);
            copy = active_;
        }
        for (const auto& active : copy) {
            if (auto handle = active->handle.exchange(nullptr)) WinHttpCloseHandle(handle);
        }
    }

private:
    struct ActiveRequest { std::atomic<HINTERNET> handle{}; };
    HINTERNET session_{};
    std::mutex active_mutex_;
    std::vector<std::shared_ptr<ActiveRequest>> active_;
};

std::string sha256_file(const std::filesystem::path& path) {
    BCRYPT_ALG_HANDLE algorithm{};
    BCRYPT_HASH_HANDLE hash{};
    DWORD object_size{};
    DWORD bytes{};
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 ||
        BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&object_size), sizeof(object_size), &bytes, 0) < 0)
        throw std::runtime_error("SHA-256 unavailable");
    std::vector<UCHAR> object(object_size);
    std::array<UCHAR, 32> digest{};
    if (BCryptCreateHash(algorithm, &hash, object.data(), object_size, nullptr, 0, 0) < 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        throw std::runtime_error("SHA-256 initialization failed");
    }
    try {
        std::ifstream input(path, std::ios::binary);
        if (!input) throw std::runtime_error("Unable to hash update file");
        std::vector<char> buffer(1024 * 1024);
        while (input) {
            input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const auto count = input.gcount();
            if (count > 0 && BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer.data()), static_cast<ULONG>(count), 0) < 0)
                throw std::runtime_error("SHA-256 update failed");
        }
        if (BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0)
            throw std::runtime_error("SHA-256 finalization failed");
    } catch (...) {
        BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(algorithm, 0);
        throw;
    }
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    std::ostringstream output;
    for (const auto byte : digest) output << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(byte);
    return output.str();
}

bool looks_like_html(const std::string& content_type, const std::vector<std::byte>& sample) {
    auto lower = content_type;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    if (lower.find("text/html") != std::string::npos) return true;
    std::string prefix;
    for (const auto byte : sample | std::views::take(std::min<std::size_t>(sample.size(), 128)))
        prefix.push_back(static_cast<char>(byte));
    std::transform(prefix.begin(), prefix.end(), prefix.begin(), [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return prefix.find("<html") != std::string::npos || prefix.find("<!doctype html") != std::string::npos;
}

class ProgressMeter {
public:
    ProgressMeter(std::uint64_t total, std::string node, int connections, bool fallback,
        std::function<void(const DownloadProgress&)> callback)
        : total_(total), node_(std::move(node)), connections_(connections), fallback_(fallback), callback_(std::move(callback)) {}

    void reset(int connections, bool fallback) {
        std::scoped_lock lock(mutex_);
        received_ = 0;
        samples_.clear();
        connections_ = connections;
        fallback_ = fallback;
        report_locked(true);
    }

    void add(std::size_t bytes) {
        std::scoped_lock lock(mutex_);
        received_ += bytes;
        const auto now = std::chrono::steady_clock::now();
        samples_.emplace_back(now, received_);
        while (!samples_.empty() && now - samples_.front().first > 2s) samples_.pop_front();
        if (now - last_report_ >= 100ms || received_ == total_) report_locked(false);
    }

private:
    void report_locked(bool force) {
        if (!callback_) return;
        const auto now = std::chrono::steady_clock::now();
        double speed{};
        if (samples_.size() >= 2) {
            const auto elapsed = std::chrono::duration<double>(samples_.back().first - samples_.front().first).count();
            if (elapsed > 0) speed = static_cast<double>(samples_.back().second - samples_.front().second) / elapsed;
        }
        last_report_ = now;
        const DownloadProgress progress{received_, total_, speed, node_, connections_, fallback_};
        // The callback must not re-enter the meter.
        auto callback = callback_;
        if (force || callback) callback(progress);
    }

    std::mutex mutex_;
    std::uint64_t total_{};
    std::uint64_t received_{};
    std::string node_;
    int connections_{};
    bool fallback_{};
    std::function<void(const DownloadProgress&)> callback_;
    std::deque<std::pair<std::chrono::steady_clock::time_point, std::uint64_t>> samples_;
    std::chrono::steady_clock::time_point last_report_{};
};

void ensure_running(DownloadControl& control, std::stop_token token) {
    control.wait_if_paused(token);
    if (token.stop_requested() || control.cancelled()) throw std::runtime_error("Download cancelled");
}

struct ProbeResult { bool ranges{}; };

ProbeResult probe_node(HttpTransport& transport, const std::string& url, std::uint64_t total,
    const DownloadOptions& options, DownloadControl& control, std::stop_token token) {
    ensure_running(control, token);
    std::vector<std::byte> sample;
    HttpRequest request{url, std::pair<std::uint64_t, std::uint64_t>{0, std::min(total, kProbeBytes) - 1},
        options.probe_timeout, options.idle_timeout, kProbeBytes};
    const auto response = transport.get(request, [&](std::span<const std::byte> bytes) {
        ensure_running(control, token);
        sample.insert(sample.end(), bytes.begin(), bytes.end());
    }, token);
    if (response.status != 200 && response.status != 206) throw std::runtime_error("Node probe returned an invalid status");
    if (looks_like_html(response.content_type, sample)) throw std::runtime_error("Node probe returned HTML");
    if (response.status == 206) {
        const auto expected_last = std::min(total, kProbeBytes) - 1;
        if (!response.content_range || response.content_range->first != 0 || response.content_range->last != expected_last ||
            response.content_range->total != total || sample.size() != expected_last + 1)
            throw std::runtime_error("Node probe returned an invalid Content-Range");
        return {true};
    }
    return {false};
}

void download_single(HttpTransport& transport, const std::string& url, const std::filesystem::path& path,
    std::uint64_t expected, const DownloadOptions& options, DownloadControl& control,
    ProgressMeter& meter, bool fallback, std::stop_token token) {
    meter.reset(1, fallback);
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("Unable to create update file");
    std::uint64_t written{};
    const auto response = transport.get({url, {}, options.probe_timeout, options.idle_timeout, {}},
        [&](std::span<const std::byte> bytes) {
            ensure_running(control, token);
            write_all(output, bytes);
            written += bytes.size();
            meter.add(bytes.size());
        }, token);
    output.close();
    if (response.status != 200 || written != expected || (response.content_length && *response.content_length != expected))
        throw std::runtime_error("Single-connection download size mismatch");
}

void download_parallel(HttpTransport& transport, const std::string& url, const std::filesystem::path& path,
    std::uint64_t expected, const DownloadOptions& options, DownloadControl& control,
    ProgressMeter& meter, std::stop_token token) {
    const int connections = std::clamp(options.max_connections, 1, 4);
    meter.reset(connections, false);
    std::vector<std::filesystem::path> parts;
    std::vector<std::exception_ptr> errors(static_cast<std::size_t>(connections));
    std::vector<std::jthread> workers;
    for (int index = 0; index < connections; ++index) {
        const auto begin = expected * static_cast<std::uint64_t>(index) / connections;
        const auto end = expected * static_cast<std::uint64_t>(index + 1) / connections - 1;
        const auto part = path.wstring() + L".part" + std::to_wstring(index);
        parts.emplace_back(part);
        workers.emplace_back([&, index, begin, end, part] {
            try {
                std::ofstream output(part, std::ios::binary | std::ios::trunc);
                if (!output) throw std::runtime_error("Unable to create range file");
                std::uint64_t written{};
                const auto response = transport.get({url, std::pair{begin, end}, options.probe_timeout, options.idle_timeout, {}},
                    [&](std::span<const std::byte> bytes) {
                        ensure_running(control, token);
                        write_all(output, bytes);
                        written += bytes.size();
                        meter.add(bytes.size());
                    }, token);
                output.close();
                if (response.status != 206 || !response.content_range || response.content_range->first != begin ||
                    response.content_range->last != end || response.content_range->total != expected || written != end - begin + 1)
                    throw std::runtime_error("Range response did not match the requested interval");
            } catch (...) {
                errors[static_cast<std::size_t>(index)] = std::current_exception();
                transport.abort_active();
            }
        });
    }
    workers.clear();
    if (std::any_of(errors.begin(), errors.end(), [](const auto& error) { return error != nullptr; })) {
        for (const auto& part : parts) { std::error_code ignored; std::filesystem::remove(part, ignored); }
        throw std::runtime_error("Parallel range download failed");
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("Unable to create merged update file");
    for (const auto& part : parts) {
        std::ifstream input(part, std::ios::binary);
        output << input.rdbuf();
        input.close();
        std::error_code ignored;
        std::filesystem::remove(part, ignored);
    }
    output.close();
    if (std::filesystem::file_size(path) != expected) throw std::runtime_error("Merged update size mismatch");
}

bool common_transaction_fields(const json::Value::object& object, int& pid, std::filesystem::path& target,
    std::filesystem::path& backup, std::filesystem::path& marker, int& exit_timeout, int& health_timeout,
    std::string& error) {
    auto number = [&](const char* name, int& value) {
        const auto result = json::number(json::member(object, name));
        if (!result || *result < 0 || *result > INT_MAX) { error = "Transaction missing " + std::string(name); return false; }
        value = static_cast<int>(*result);
        return true;
    };
    auto path = [&](const char* name, std::filesystem::path& value) {
        const auto* result = json::string(json::member(object, name));
        if (!result) { error = "Transaction missing " + std::string(name); return false; }
        value = wide(*result);
        if (!value.is_absolute()) { error = "Transaction paths must be absolute"; return false; }
        return true;
    };
    return number("ParentProcessId", pid) && path("TargetExePath", target) && path("BackupExePath", backup) &&
        path("HealthMarkerPath", marker) && number("ParentExitTimeoutSeconds", exit_timeout) &&
        number("HealthTimeoutSeconds", health_timeout);
}

std::string path_json(const std::filesystem::path& path) { return json_escape(narrow(path.wstring())); }

bool write_text_atomic(const std::filesystem::path& path, const std::string& value, std::string& error) {
    try {
        std::filesystem::create_directories(path.parent_path());
        const auto temporary = path.wstring() + L".tmp";
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output.write(value.data(), static_cast<std::streamsize>(value.size()));
        output.close();
        if (!output) throw std::runtime_error("Unable to write transaction");
        std::filesystem::rename(temporary, path);
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
}

bool writable_directory(const std::filesystem::path& directory) {
    const auto probe = directory / (L".update-write-test." + std::to_wstring(GetCurrentProcessId()) + L".tmp");
    HANDLE file = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    CloseHandle(file);
    return true;
}

bool valid_pe(std::span<const std::byte> bytes) {
    if (bytes.size() < 128 || std::to_integer<unsigned char>(bytes[0]) != 'M' || std::to_integer<unsigned char>(bytes[1]) != 'Z') return false;
    std::uint32_t offset{};
    std::memcpy(&offset, bytes.data() + 0x3c, sizeof(offset));
    if (offset > bytes.size() - 24) return false;
    std::uint32_t signature{};
    std::memcpy(&signature, bytes.data() + offset, sizeof(signature));
    if (signature != 0x00004550) return false;
    std::uint16_t machine{};
    std::memcpy(&machine, bytes.data() + offset + 4, sizeof(machine));
    return machine == IMAGE_FILE_MACHINE_AMD64;
}

LaunchResult launch_common(std::span<const std::byte> stub, const std::filesystem::path& directory,
    const std::filesystem::path& transaction_path, const std::string& transaction,
    const wchar_t* argument) {
    LaunchResult result;
    result.transaction_directory = directory;
    try {
        if (!valid_pe(stub)) throw std::runtime_error("Embedded updater Stub is not a valid x64 PE file");
        std::filesystem::create_directories(directory);
        const auto stub_path = directory / L"UpdaterStub.exe";
        {
            std::ofstream output(stub_path, std::ios::binary | std::ios::trunc);
            output.write(reinterpret_cast<const char*>(stub.data()), static_cast<std::streamsize>(stub.size()));
            if (!output) throw std::runtime_error("Unable to extract updater Stub");
        }
        std::string error;
        if (!write_text_atomic(transaction_path, transaction, error)) throw std::runtime_error(error);
        std::wstring command = L"\"" + stub_path.wstring() + L"\" " + argument + L" \"" + transaction_path.wstring() + L"\"";
        STARTUPINFOW startup{sizeof(startup)};
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                directory.c_str(), &startup, &process)) throw std::runtime_error("Unable to start updater Stub");
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        result.started = true;
    } catch (const std::exception& exception) {
        result.error = exception.what();
    }
    return result;
}

} // namespace

bool operator>(const Version& left, const Version& right) noexcept {
    return std::tie(left.major, left.minor, left.patch) > std::tie(right.major, right.minor, right.patch);
}

std::optional<Version> parse_version(const std::string& input) {
    std::string text = input;
    if (!text.empty() && (text.front() == 'v' || text.front() == 'V')) text.erase(text.begin());
    const auto suffix = text.find_first_not_of("0123456789.");
    if (suffix != std::string::npos) text.resize(suffix);
    Version version;
    char first{}, second{};
    std::istringstream stream(text);
    if (!(stream >> version.major >> first >> version.minor >> second >> version.patch) ||
        first != '.' || second != '.' || stream.peek() != std::char_traits<char>::eof() ||
        version.major < 0 || version.minor < 0 || version.patch < 0) return {};
    return version;
}

std::string format_version(const Version& version) {
    return std::to_string(version.major) + "." + std::to_string(version.minor) + "." + std::to_string(version.patch);
}

std::shared_ptr<HttpTransport> make_winhttp_transport(std::wstring user_agent) {
    return std::make_shared<WinHttpTransport>(user_agent);
}

void DownloadControl::pause() { std::scoped_lock lock(mutex_); if (!cancelled_) paused_ = true; }
void DownloadControl::resume() { { std::scoped_lock lock(mutex_); paused_ = false; } resume_signal_.notify_all(); }
void DownloadControl::cancel() { { std::scoped_lock lock(mutex_); cancelled_ = true; paused_ = false; } resume_signal_.notify_all(); request_interrupt(); }
void DownloadControl::use_acceleration(bool enabled) {
    {
        std::scoped_lock lock(mutex_);
        if (cancelled_ || acceleration_ == enabled) return;
        acceleration_ = enabled;
        node_switch_request_ = enabled
            ? NodeSwitchRequest::use_acceleration_nodes
            : NodeSwitchRequest::use_official_node;
    }
    request_interrupt();
}
bool DownloadControl::next_accelerated_node() {
    {
        std::scoped_lock lock(mutex_);
        if (!acceleration_ || cancelled_) return false;
        node_switch_request_ = NodeSwitchRequest::next_accelerated_node;
    }
    request_interrupt();
    return true;
}
bool DownloadControl::cancelled() const { std::scoped_lock lock(mutex_); return cancelled_; }
bool DownloadControl::paused() const { std::scoped_lock lock(mutex_); return paused_; }
bool DownloadControl::acceleration_enabled() const { std::scoped_lock lock(mutex_); return acceleration_; }
bool DownloadControl::node_switch_requested() const noexcept {
    return node_switch_request_.load() != NodeSwitchRequest::none;
}
NodeSwitchRequest DownloadControl::consume_node_switch_request() {
    return node_switch_request_.exchange(NodeSwitchRequest::none);
}
bool DownloadControl::consume_node_switch() {
    return consume_node_switch_request() != NodeSwitchRequest::none;
}
void DownloadControl::wait_if_paused(std::stop_token token) const {
    std::unique_lock lock(mutex_);
    resume_signal_.wait(lock, token, [&] { return !paused_ || cancelled_; });
}
void DownloadControl::set_interrupt(std::function<void()> interrupt) { std::scoped_lock lock(mutex_); interrupt_ = std::move(interrupt); }
void DownloadControl::request_interrupt() { std::function<void()> callback; { std::scoped_lock lock(mutex_); callback = interrupt_; } if (callback) callback(); }

UpdateClient::UpdateClient(ClientOptions options, std::shared_ptr<HttpTransport> transport)
    : options_(std::move(options)), transport_(std::move(transport)) {
    if (!valid_repository(options_.repository) || !valid_asset_name(options_.exe_asset_name) ||
        !valid_asset_name(options_.sha256_asset_name)) throw std::invalid_argument("Invalid update client options");
    if (options_.application_id.empty()) options_.application_id = L"desktop-update-kit";
    if (options_.temp_root.empty()) options_.temp_root = std::filesystem::temp_directory_path();
    options_.download.max_connections = std::clamp(options_.download.max_connections, 1, 4);
    options_.download.buffer_size = std::clamp<std::size_t>(options_.download.buffer_size, 64 * 1024, 1024 * 1024);
    options_.download.retries_per_node = std::clamp(options_.download.retries_per_node, 0, 1);
    if (!transport_) transport_ = make_winhttp_transport(options_.application_id + L"-UpdateClient/1.0");
}

std::optional<Release> UpdateClient::check_for_update(std::stop_token token) const {
    std::vector<std::byte> body;
    const auto url = "https://github.com/" + options_.repository + "/releases/latest/download/update.json";
    const auto response = transport_->get({url, {}, 5000ms, 20000ms, 2 * 1024 * 1024},
        [&](std::span<const std::byte> bytes) { body.insert(body.end(), bytes.begin(), bytes.end()); }, token);
    if (response.status != 200) throw std::runtime_error("Update manifest request failed");
    std::string source(reinterpret_cast<const char*>(body.data()), body.size());
    if (source.starts_with("\xEF\xBB\xBF")) source.erase(0, 3);
    std::string error;
    auto release = parse_release(source, options_, error);
    if (!release) throw std::runtime_error(error);
    save_cache(options_, release->nodes, load_cache(options_).last_success);
    return release->version > options_.current_version ? release : std::nullopt;
}

std::filesystem::path UpdateClient::download_and_verify(const Release& release, DownloadControl& control,
    std::function<void(const DownloadProgress&)> progress, std::stop_token token) const {
    if (release.repository != options_.repository || release.asset != options_.exe_asset_name ||
        release.sha256_asset != options_.sha256_asset_name || release.size == 0 || !valid_sha256(release.sha256))
        throw std::runtime_error("Release does not match update client options");
    const auto directory = options_.temp_root / (safe_application_id(options_.application_id) + L"-update") /
        (std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    std::filesystem::create_directories(directory);
    control.set_interrupt([transport = transport_] { transport->abort_active(); });
    try {
        ensure_running(control, token);
        const auto sha_path = directory / wide(release.sha256_asset);
        {
            std::ofstream output(sha_path, std::ios::binary | std::ios::trunc);
            const auto response = transport_->get({official_url(options_, release, true), {}, 5000ms, 20000ms, 4096},
                [&](std::span<const std::byte> bytes) { write_all(output, bytes); }, token);
            output.close();
            if (response.status != 200 || trim_hash(read_file(sha_path)) != release.sha256)
                throw std::runtime_error("Published SHA-256 does not match update manifest");
        }
        auto nodes = resolve_nodes(options_, release, control.acceleration_enabled());
        std::exception_ptr last_error;
        std::size_t index{};
        while (index < nodes.size()) {
            const auto node = nodes[index];
            try {
                ensure_running(control, token);
                const auto url = expand_node(node, official_url(options_, release, false));
                const auto probe = probe_node(*transport_, url, release.size, options_.download, control, token);
                const auto output_path = directory / wide(release.asset);
                ProgressMeter meter(release.size, node.id, 1, false, progress);
                bool completed{};
                if (probe.ranges && release.size >= options_.download.parallel_threshold && options_.download.max_connections > 1) {
                    try {
                        download_parallel(*transport_, url, output_path, release.size, options_.download, control, meter, token);
                        completed = true;
                    } catch (...) {
                        if (control.cancelled() || token.stop_requested() || control.node_switch_requested()) throw;
                        std::error_code ignored;
                        std::filesystem::remove(output_path, ignored);
                        for (int attempt = 0; attempt <= options_.download.retries_per_node; ++attempt) {
                            try {
                                download_single(*transport_, url, output_path, release.size, options_.download, control, meter, true, token);
                                completed = true;
                                break;
                            } catch (...) {
                                std::filesystem::remove(output_path, ignored);
                                if (attempt == options_.download.retries_per_node) throw;
                            }
                        }
                    }
                }
                if (!completed) {
                    for (int attempt = 0; attempt <= options_.download.retries_per_node; ++attempt) {
                        try {
                            download_single(*transport_, url, output_path, release.size, options_.download, control, meter, false, token);
                            completed = true;
                            break;
                        } catch (...) {
                            std::error_code ignored;
                            std::filesystem::remove(output_path, ignored);
                            if (attempt == options_.download.retries_per_node) throw;
                        }
                    }
                }
                if (!completed || trim_hash(sha256_file(output_path)) != release.sha256)
                    throw std::runtime_error("Downloaded executable failed SHA-256 verification");
                save_cache(options_, release.nodes, node.id);
                control.set_interrupt({});
                return output_path;
            } catch (...) {
                last_error = std::current_exception();
                if (control.cancelled() || token.stop_requested()) throw;
                const auto switch_request = control.consume_node_switch_request();
                if (switch_request != NodeSwitchRequest::none) {
                    auto refreshed = resolve_nodes(options_, release, control.acceleration_enabled());
                    if (switch_request == NodeSwitchRequest::next_accelerated_node) {
                        const auto current = std::find_if(refreshed.begin(), refreshed.end(), [&](const DownloadNode& candidate) {
                            return candidate.id == node.id;
                        });
                        const auto current_index = current == refreshed.end()
                            ? refreshed.size() - 1
                            : static_cast<std::size_t>(std::distance(refreshed.begin(), current));
                        index = detail::next_accelerated_node_index(refreshed, current_index);
                    } else {
                        index = 0;
                    }
                    nodes = std::move(refreshed);
                    if (index >= nodes.size()) throw std::runtime_error("No accelerated download node is available");
                } else {
                    ++index;
                }
            }
        }
        if (last_error) std::rethrow_exception(last_error);
        throw std::runtime_error("All update download nodes failed");
    } catch (...) {
        control.set_interrupt({});
        std::error_code ignored;
        std::filesystem::remove_all(directory, ignored);
        throw;
    }
}

DownloadSession::DownloadSession(UpdateClient client) : client_(std::move(client)) {}
DownloadSession::~DownloadSession() { cancel(); if (worker_.joinable()) worker_.join(); }

void DownloadSession::set_changed_callback(std::function<void(const SessionSnapshot&)> callback) {
    std::scoped_lock lock(mutex_);
    changed_ = std::move(callback);
}

SessionSnapshot DownloadSession::snapshot() const { std::scoped_lock lock(mutex_); return snapshot_; }

void DownloadSession::notify() {
    std::function<void(const SessionSnapshot&)> callback;
    SessionSnapshot copy;
    { std::scoped_lock lock(mutex_); callback = changed_; copy = snapshot_; }
    if (callback) callback(copy);
}

bool DownloadSession::start(Release release, bool acceleration) {
    {
        std::scoped_lock lock(mutex_);
        if (snapshot_.state == SessionState::downloading || snapshot_.state == SessionState::paused) return false;
    }
    if (worker_.joinable()) worker_.join();
    {
        std::scoped_lock lock(mutex_);
        control_ = std::make_unique<DownloadControl>();
        control_->use_acceleration(acceleration);
        (void)control_->consume_node_switch();
        snapshot_ = {SessionState::downloading, release, {}, {}, {}, false, acceleration};
    }
    worker_ = std::jthread([this, release](std::stop_token token) {
        try {
            auto path = client_.download_and_verify(release, *control_, [this](const DownloadProgress& value) {
                { std::scoped_lock lock(mutex_); snapshot_.progress = value; }
                notify();
            }, token);
            { std::scoped_lock lock(mutex_); snapshot_.state = SessionState::completed; snapshot_.downloaded_path = std::move(path); }
        } catch (const std::exception& exception) {
            std::scoped_lock lock(mutex_);
            snapshot_.state = control_ && control_->cancelled() ? SessionState::cancelled : SessionState::failed;
            snapshot_.error = exception.what();
        }
        notify();
    });
    notify();
    return true;
}

bool DownloadSession::pause() {
    if (!control_) return false;
    control_->pause();
    { std::scoped_lock lock(mutex_); if (snapshot_.state != SessionState::downloading) return false; snapshot_.state = SessionState::paused; }
    notify();
    return true;
}

bool DownloadSession::resume() {
    if (!control_) return false;
    { std::scoped_lock lock(mutex_); if (snapshot_.state != SessionState::paused) return false; snapshot_.state = SessionState::downloading; }
    control_->resume();
    notify();
    return true;
}

bool DownloadSession::pause_when_ui_closes() {
    const auto state = snapshot().state;
    return state == SessionState::downloading ? pause() : state == SessionState::paused;
}

bool DownloadSession::continue_in_background() {
    if (snapshot().state == SessionState::paused && !resume()) return false;
    { std::scoped_lock lock(mutex_); if (snapshot_.state != SessionState::downloading) return false; snapshot_.background = true; }
    notify();
    return true;
}

bool DownloadSession::cancel() {
    if (!control_) return false;
    control_->cancel();
    if (worker_.joinable()) worker_.request_stop();
    return true;
}

bool DownloadSession::set_acceleration(bool enabled) {
    if (!control_) return false;
    { std::scoped_lock lock(mutex_); if (snapshot_.acceleration == enabled) return false; }
    control_->use_acceleration(enabled);
    { std::scoped_lock lock(mutex_); snapshot_.acceleration = enabled; }
    notify();
    return true;
}

bool DownloadSession::next_accelerated_node() { return control_ && control_->next_accelerated_node(); }

void DownloadSession::discard_completed() {
    std::filesystem::path path;
    { std::scoped_lock lock(mutex_); path = snapshot_.downloaded_path; snapshot_.downloaded_path.clear(); }
    if (!path.empty()) { std::error_code ignored; std::filesystem::remove_all(path.parent_path(), ignored); }
}

std::optional<UpdateTransaction> read_update_transaction(const std::filesystem::path& path, std::string& error) {
    try {
        json::Value root;
        if (!json::parse(read_file(path), root, error)) return {};
        const auto* object = json::object(&root);
        if (!object) { error = "Transaction must be object"; return {}; }
        UpdateTransaction transaction;
        if (!common_transaction_fields(*object, transaction.parent_process_id, transaction.target_exe, transaction.backup_exe,
                transaction.health_marker, transaction.parent_exit_timeout_seconds, transaction.health_timeout_seconds, error)) return {};
        const auto* downloaded = json::string(json::member(*object, "DownloadedExePath"));
        if (!downloaded) { error = "Transaction missing DownloadedExePath"; return {}; }
        transaction.downloaded_exe = wide(*downloaded);
        if (!transaction.downloaded_exe.is_absolute()) { error = "Transaction paths must be absolute"; return {}; }
        return transaction;
    } catch (const std::exception& exception) { error = exception.what(); return {}; }
}

std::optional<RenameTransaction> read_rename_transaction(const std::filesystem::path& path, std::string& error) {
    try {
        json::Value root;
        if (!json::parse(read_file(path), root, error)) return {};
        const auto* object = json::object(&root);
        if (!object) { error = "Transaction must be object"; return {}; }
        RenameTransaction transaction;
        if (!common_transaction_fields(*object, transaction.parent_process_id, transaction.target_exe, transaction.backup_exe,
                transaction.health_marker, transaction.parent_exit_timeout_seconds, transaction.health_timeout_seconds, error)) return {};
        const auto* source = json::string(json::member(*object, "SourceExePath"));
        if (!source) { error = "Transaction missing SourceExePath"; return {}; }
        transaction.source_exe = wide(*source);
        if (!transaction.source_exe.is_absolute()) { error = "Transaction paths must be absolute"; return {}; }
        return transaction;
    } catch (const std::exception& exception) { error = exception.what(); return {}; }
}

bool write_update_transaction(const std::filesystem::path& path, const UpdateTransaction& transaction, std::string& error) {
    const auto body = "{\"ParentProcessId\":" + std::to_string(transaction.parent_process_id) +
        ",\"TargetExePath\":\"" + path_json(transaction.target_exe) + "\",\"DownloadedExePath\":\"" + path_json(transaction.downloaded_exe) +
        "\",\"BackupExePath\":\"" + path_json(transaction.backup_exe) + "\",\"HealthMarkerPath\":\"" + path_json(transaction.health_marker) +
        "\",\"ParentExitTimeoutSeconds\":" + std::to_string(transaction.parent_exit_timeout_seconds) +
        ",\"HealthTimeoutSeconds\":" + std::to_string(transaction.health_timeout_seconds) + "}";
    return write_text_atomic(path, body, error);
}

bool write_rename_transaction(const std::filesystem::path& path, const RenameTransaction& transaction, std::string& error) {
    const auto body = "{\"ParentProcessId\":" + std::to_string(transaction.parent_process_id) +
        ",\"SourceExePath\":\"" + path_json(transaction.source_exe) + "\",\"TargetExePath\":\"" + path_json(transaction.target_exe) +
        "\",\"BackupExePath\":\"" + path_json(transaction.backup_exe) + "\",\"HealthMarkerPath\":\"" + path_json(transaction.health_marker) +
        "\",\"ParentExitTimeoutSeconds\":" + std::to_string(transaction.parent_exit_timeout_seconds) +
        ",\"HealthTimeoutSeconds\":" + std::to_string(transaction.health_timeout_seconds) + "}";
    return write_text_atomic(path, body, error);
}

LaunchResult launch_update(std::span<const std::byte> stub, const std::filesystem::path& downloaded,
    const std::filesystem::path& target, int parent_process_id) {
    if (!target.is_absolute() || !downloaded.is_absolute() || !std::filesystem::exists(downloaded) || !writable_directory(target.parent_path()))
        return {false, {}, "The program directory is not writable or the downloaded file is missing"};
    const auto directory = std::filesystem::temp_directory_path() / L"DesktopUpdateKit-native" /
        (std::to_wstring(parent_process_id) + L"-" + std::to_wstring(GetTickCount64()));
    const auto transaction_path = directory / L"update.json";
    UpdateTransaction transaction{parent_process_id, std::filesystem::absolute(target), std::filesystem::absolute(downloaded),
        target.parent_path() / (L"." + target.filename().wstring() + L"." + std::to_wstring(GetTickCount64()) + L".bak"),
        directory / L"healthy.ok", 30, 30};
    std::string error;
    const auto body_path = directory / L"body.json";
    if (!write_update_transaction(body_path, transaction, error)) return {false, directory, error};
    const auto body = read_file(body_path);
    std::error_code ignored;
    std::filesystem::remove(body_path, ignored);
    return launch_common(stub, directory, transaction_path, body, L"--transaction");
}

LaunchResult launch_rename(std::span<const std::byte> stub, const std::filesystem::path& source,
    const std::filesystem::path& target, int parent_process_id) {
    if (!source.is_absolute() || !target.is_absolute() || !std::filesystem::exists(source) || !writable_directory(target.parent_path()))
        return {false, {}, "The executable directory is not writable or the source file is missing"};
    const auto directory = std::filesystem::temp_directory_path() / L"DesktopUpdateKit-native-name" /
        (std::to_wstring(parent_process_id) + L"-" + std::to_wstring(GetTickCount64()));
    const auto transaction_path = directory / L"rename.json";
    RenameTransaction transaction{parent_process_id, std::filesystem::absolute(source), std::filesystem::absolute(target),
        target.parent_path() / (L"." + target.filename().wstring() + L"." + std::to_wstring(GetTickCount64()) + L".bak"),
        directory / L"healthy.ok", 30, 30};
    std::string error;
    const auto body_path = directory / L"body.json";
    if (!write_rename_transaction(body_path, transaction, error)) return {false, directory, error};
    const auto body = read_file(body_path);
    std::error_code ignored;
    std::filesystem::remove(body_path, ignored);
    return launch_common(stub, directory, transaction_path, body, L"--rename-transaction");
}

} // namespace desktop_update_kit
