#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

namespace desktop_update_kit {

struct Version {
    int major{};
    int minor{};
    int patch{};
};

bool operator>(const Version& left, const Version& right) noexcept;
std::optional<Version> parse_version(const std::string& text);
std::string format_version(const Version& version);

struct DownloadNode {
    std::string id;
    std::string url_template;
    int priority{};
    bool enabled{true};
};

struct Release {
    Version version;
    std::string tag;
    std::string repository;
    std::string asset;
    std::string sha256_asset;
    std::string sha256;
    std::uint64_t size{};
    std::string notes;
    std::vector<DownloadNode> nodes;
};

struct DownloadOptions {
    int max_connections{4};
    std::uint64_t parallel_threshold{16ull * 1024 * 1024};
    std::size_t buffer_size{512 * 1024};
    std::chrono::milliseconds probe_timeout{5000};
    std::chrono::milliseconds idle_timeout{20000};
    int retries_per_node{1};
};

struct ClientOptions {
    std::wstring application_id;
    std::string repository;
    std::string exe_asset_name;
    std::string sha256_asset_name;
    Version current_version;
    std::filesystem::path temp_root{};
    DownloadOptions download{};
};

struct ContentRange {
    std::uint64_t first{};
    std::uint64_t last{};
    std::uint64_t total{};
};

struct HttpRequest {
    std::string url;
    std::optional<std::pair<std::uint64_t, std::uint64_t>> range;
    std::chrono::milliseconds connect_timeout{5000};
    std::chrono::milliseconds idle_timeout{20000};
    std::optional<std::uint64_t> max_body_bytes;
};

struct HttpResponse {
    unsigned status{};
    std::string content_type;
    std::optional<std::uint64_t> content_length;
    std::optional<ContentRange> content_range;
};

class HttpTransport {
public:
    using ChunkSink = std::function<void(std::span<const std::byte>)>;
    virtual ~HttpTransport() = default;
    virtual HttpResponse get(const HttpRequest& request, const ChunkSink& sink,
        std::stop_token stop_token = {}) = 0;
    virtual void abort_active() noexcept = 0;
};

std::shared_ptr<HttpTransport> make_winhttp_transport(std::wstring user_agent = L"DesktopUpdateKit-Native/1.0");

struct DownloadProgress {
    std::uint64_t received{};
    std::uint64_t total{};
    double bytes_per_second{};
    std::string node_id;
    int connections{1};
    bool parallel_fallback{};
};

enum class SessionState { idle, downloading, paused, completed, failed, cancelled };

enum class NodeSwitchRequest {
    none,
    use_acceleration_nodes,
    use_official_node,
    next_accelerated_node,
};

struct SessionSnapshot {
    SessionState state{SessionState::idle};
    std::optional<Release> release;
    std::optional<DownloadProgress> progress;
    std::filesystem::path downloaded_path;
    std::string error;
    bool background{};
    bool acceleration{true};
};

class DownloadControl {
public:
    void pause();
    void resume();
    void cancel();
    void use_acceleration(bool enabled);
    bool next_accelerated_node();
    [[nodiscard]] bool cancelled() const;
    [[nodiscard]] bool paused() const;
    [[nodiscard]] bool acceleration_enabled() const;
    [[nodiscard]] bool node_switch_requested() const noexcept;
    NodeSwitchRequest consume_node_switch_request();
    bool consume_node_switch();
    void wait_if_paused(std::stop_token token) const;
    void set_interrupt(std::function<void()> interrupt);

private:
    void request_interrupt();
    mutable std::mutex mutex_;
    mutable std::condition_variable_any resume_signal_;
    bool paused_{};
    bool cancelled_{};
    bool acceleration_{true};
    std::atomic<NodeSwitchRequest> node_switch_request_{NodeSwitchRequest::none};
    std::function<void()> interrupt_;
};

class UpdateClient {
public:
    explicit UpdateClient(ClientOptions options, std::shared_ptr<HttpTransport> transport = {});
    std::optional<Release> check_for_update(std::stop_token token = {}) const;
    std::filesystem::path download_and_verify(const Release& release, DownloadControl& control,
        std::function<void(const DownloadProgress&)> progress = {}, std::stop_token token = {}) const;

private:
    ClientOptions options_;
    std::shared_ptr<HttpTransport> transport_;
};

class DownloadSession {
public:
    explicit DownloadSession(UpdateClient client);
    ~DownloadSession();
    bool start(Release release, bool acceleration = true);
    bool pause();
    bool resume();
    bool pause_when_ui_closes();
    bool continue_in_background();
    bool cancel();
    bool set_acceleration(bool enabled);
    bool next_accelerated_node();
    void discard_completed();
    [[nodiscard]] SessionSnapshot snapshot() const;
    void set_changed_callback(std::function<void(const SessionSnapshot&)> callback);

private:
    void notify();
    UpdateClient client_;
    std::unique_ptr<DownloadControl> control_;
    std::jthread worker_;
    mutable std::mutex mutex_;
    SessionSnapshot snapshot_;
    std::function<void(const SessionSnapshot&)> changed_;
};

struct UpdateTransaction {
    int parent_process_id{};
    std::filesystem::path target_exe;
    std::filesystem::path downloaded_exe;
    std::filesystem::path backup_exe;
    std::filesystem::path health_marker;
    int parent_exit_timeout_seconds{30};
    int health_timeout_seconds{30};
};

struct RenameTransaction {
    int parent_process_id{};
    std::filesystem::path source_exe;
    std::filesystem::path target_exe;
    std::filesystem::path backup_exe;
    std::filesystem::path health_marker;
    int parent_exit_timeout_seconds{30};
    int health_timeout_seconds{30};
};

std::optional<UpdateTransaction> read_update_transaction(const std::filesystem::path& path, std::string& error);
std::optional<RenameTransaction> read_rename_transaction(const std::filesystem::path& path, std::string& error);
bool write_update_transaction(const std::filesystem::path& path, const UpdateTransaction& transaction, std::string& error);
bool write_rename_transaction(const std::filesystem::path& path, const RenameTransaction& transaction, std::string& error);

struct LaunchResult {
    bool started{};
    std::filesystem::path transaction_directory;
    std::string error;
};

LaunchResult launch_update(std::span<const std::byte> updater_stub, const std::filesystem::path& downloaded_exe,
    const std::filesystem::path& target_exe, int parent_process_id);
LaunchResult launch_rename(std::span<const std::byte> updater_stub, const std::filesystem::path& source_exe,
    const std::filesystem::path& target_exe, int parent_process_id);

} // namespace desktop_update_kit
