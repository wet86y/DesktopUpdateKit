#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>
#include <cstdint>

namespace desktop_update_kit {

struct Version { int major{}; int minor{}; int patch{}; std::string suffix{}; };
bool operator>(const Version& left, const Version& right);
std::optional<Version> parse_version(const std::string& text);

struct DownloadNode { std::string id; std::string url_template; int priority{}; bool enabled{true}; };
struct Release {
    Version version; std::string tag; std::string repository; std::string asset; std::string sha256_asset;
    std::string sha256; std::uint64_t size{}; std::string notes; std::vector<DownloadNode> nodes;
};
struct ClientOptions {
    std::wstring application_id; std::string repository; std::string exe_asset_name; std::string sha256_asset_name;
    Version current_version; std::filesystem::path temp_root{};
};
struct DownloadProgress { std::uint64_t received{}; std::uint64_t total{}; double bytes_per_second{}; std::string node_id; int connections{1}; bool parallel_fallback{}; };
enum class SessionState { idle, downloading, paused, completed, failed, cancelled };
struct SessionSnapshot { SessionState state{SessionState::idle}; std::optional<Release> release; std::optional<DownloadProgress> progress; std::filesystem::path downloaded_path; std::string error; bool background{}; bool acceleration{true}; };

class DownloadControl {
public:
    void pause(); void resume(); void cancel(); void use_acceleration(bool enabled); bool next_accelerated_node();
    [[nodiscard]] bool cancelled() const; [[nodiscard]] bool paused() const; [[nodiscard]] bool acceleration_enabled() const;
    bool consume_node_switch();
    void wait_if_paused(std::stop_token token) const;
private:
    mutable std::mutex mutex_; mutable std::condition_variable_any resume_signal_; bool paused_{}; bool cancelled_{}; bool acceleration_{true}; std::atomic_bool switch_node_{};
};

class UpdateClient {
public:
    explicit UpdateClient(ClientOptions options);
    std::optional<Release> check_for_update(std::stop_token token = {}) const;
    std::filesystem::path download_and_verify(const Release& release, DownloadControl& control,
        std::function<void(const DownloadProgress&)> progress = {}, std::stop_token token = {}) const;
private:
    ClientOptions options_;
};

class DownloadSession {
public:
    explicit DownloadSession(UpdateClient client); ~DownloadSession();
    bool start(Release release, bool acceleration = true); bool pause(); bool resume(); bool continue_in_background();
    bool cancel(); bool set_acceleration(bool enabled); bool next_accelerated_node();
    [[nodiscard]] SessionSnapshot snapshot() const; void set_changed_callback(std::function<void(const SessionSnapshot&)> callback);
private:
    void notify();
    UpdateClient client_; std::unique_ptr<DownloadControl> control_; std::jthread worker_; mutable std::mutex mutex_; SessionSnapshot snapshot_; std::function<void(const SessionSnapshot&)> changed_;
};

struct UpdateTransaction { int parent_process_id{}; std::filesystem::path target_exe; std::filesystem::path downloaded_exe; std::filesystem::path backup_exe; std::filesystem::path health_marker; int parent_exit_timeout_seconds{30}; int health_timeout_seconds{30}; };
struct RenameTransaction { int parent_process_id{}; std::filesystem::path source_exe; std::filesystem::path target_exe; std::filesystem::path backup_exe; std::filesystem::path health_marker; int parent_exit_timeout_seconds{30}; int health_timeout_seconds{30}; };
std::optional<UpdateTransaction> read_update_transaction(const std::filesystem::path& path, std::string& error);
std::optional<RenameTransaction> read_rename_transaction(const std::filesystem::path& path, std::string& error);

} // namespace desktop_update_kit
