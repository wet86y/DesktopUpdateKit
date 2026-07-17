#include "DesktopUpdateKit/UpdateKit.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <chrono>

using namespace desktop_update_kit;

namespace {
constexpr auto kHash = "44edc80377f7d227f3e05a12c7a6458262ffa104db638068e7f48cae2917a26e";
int failures{};

void expect(bool condition, const char* message) {
    if (!condition) { std::cerr << "FAILED: " << message << '\n'; ++failures; }
}

class FixtureTransport final : public HttpTransport {
public:
    explicit FixtureTransport(std::string manifest = {}) : manifest_(std::move(manifest)) {
        payload_.resize(2 * 1024 * 1024);
        for (std::size_t index = 0; index < payload_.size(); ++index)
            payload_[index] = static_cast<std::byte>((index * 31) % 251);
    }

    void set_parallel_failure(bool value) { parallel_failure_ = value; }
    void set_bad_manifest(bool value) { bad_manifest_ = value; }
    void set_slow(bool value) { slow_ = value; }
    void set_range_unsupported(bool value) { range_unsupported_ = value; }
    void set_wrong_range(bool value) { wrong_range_ = value; }
    void set_html_payload(bool value) { html_payload_ = value; }
    void set_bad_sha_asset(bool value) { bad_sha_asset_ = value; }
    void set_final_hash_mismatch(bool value) { final_hash_mismatch_ = value; }
    void set_insecure_node(bool value) { insecure_node_ = value; }
    int range_requests() const { return range_requests_; }
    int full_requests() const { return full_requests_; }

    HttpResponse get(const HttpRequest& request, const ChunkSink& sink, std::stop_token token) override {
        if (request.url.ends_with("/update.json")) {
            if (!manifest_.empty()) {
                emit(request, sink, std::as_bytes(std::span(manifest_)), token);
                return {200, "application/json", manifest_.size(), {}};
            }
            const std::string repository = bad_manifest_ ? "attacker/repo" : "fixture/repo";
            const std::string declared_hash = final_hash_mismatch_ ? std::string(64, '0') : kHash;
            const std::string manifest = "{\"version\":\"2.0.1\",\"repository\":\"" + repository +
                "\",\"tag\":\"v2.0.1\",\"asset\":\"fixture-app.exe\","
                "\"sha256Asset\":\"fixture-app.exe.sha256\",\"sha256\":\"" + declared_hash +
                "\",\"size\":2097152,\"releaseNotes\":\"fixture\",\"downloadNodes\":[" +
                (insecure_node_ ? "{\"id\":\"insecure\",\"template\":\"http://example.invalid/{url}\",\"priority\":1,\"enabled\":true}," : "") +
                "{\"id\":\"github-direct\",\"template\":\"{url}\",\"priority\":1000,\"enabled\":true}]}";
            emit(request, sink, std::as_bytes(std::span(manifest)), token);
            return {200, "application/json", manifest.size(), {}};
        }
        if (request.url.ends_with(".sha256")) {
            const std::string published = bad_sha_asset_ ? std::string(64, 'f')
                : final_hash_mismatch_ ? std::string(64, '0') : std::string(kHash);
            const std::string hash = published + "  fixture-app.exe\n";
            emit(request, sink, std::as_bytes(std::span(hash)), token);
            return {200, "text/plain", hash.size(), {}};
        }
        if (!request.url.ends_with("fixture-app.exe")) return {404, "text/plain", 0, {}};
        if (html_payload_) {
            const std::string html = "<!doctype html><html><body>proxy error</body></html>";
            emit(request, sink, std::as_bytes(std::span(html)), token);
            return {200, "text/html", html.size(), {}};
        }
        if (request.range) {
            ++range_requests_;
            const auto [first, last] = *request.range;
            if (range_unsupported_) {
                emit(request, sink, payload_, token);
                return {200, "application/octet-stream", payload_.size(), {}};
            }
            if (parallel_failure_ && last - first + 1 > 64 * 1024) {
                const auto bytes = std::span<const std::byte>(payload_.data(), std::min<std::size_t>(payload_.size(), 128 * 1024));
                emit(request, sink, bytes, token);
                return {200, "application/octet-stream", payload_.size(), {}};
            }
            if (last >= payload_.size() || last < first) return {416, "text/plain", 0, {}};
            const auto bytes = std::span<const std::byte>(payload_.data() + first, static_cast<std::size_t>(last - first + 1));
            emit(request, sink, bytes, token);
            return {206, "application/octet-stream", bytes.size(),
                ContentRange{wrong_range_ ? first + 1 : first, last, payload_.size()}};
        }
        ++full_requests_;
        emit(request, sink, payload_, token);
        return {200, "application/octet-stream", payload_.size(), {}};
    }

    void abort_active() noexcept override { aborted_ = true; }

private:
    void emit(const HttpRequest& request, const ChunkSink& sink, std::span<const std::byte> bytes,
        std::stop_token token) {
        std::size_t delivered{};
        while (delivered < bytes.size()) {
            if (token.stop_requested()) throw std::runtime_error("fixture cancelled");
            auto count = std::min<std::size_t>(32768, bytes.size() - delivered);
            if (request.max_body_bytes && delivered + count > *request.max_body_bytes)
                count = static_cast<std::size_t>(*request.max_body_bytes - delivered);
            if (!count) break;
            if (slow_) std::this_thread::sleep_for(std::chrono::milliseconds(10));
            sink(bytes.subspan(delivered, count));
            delivered += count;
            if (request.max_body_bytes && delivered >= *request.max_body_bytes) break;
        }
    }

    std::vector<std::byte> payload_;
    std::string manifest_;
    bool parallel_failure_{};
    bool bad_manifest_{};
    bool slow_{};
    bool range_unsupported_{};
    bool wrong_range_{};
    bool html_payload_{};
    bool bad_sha_asset_{};
    bool final_hash_mismatch_{};
    bool insecure_node_{};
    std::atomic_bool aborted_{};
    std::atomic_int range_requests_{};
    std::atomic_int full_requests_{};
};

ClientOptions options(const std::filesystem::path& temporary) {
    ClientOptions value{L"desktop-update-kit-fixture", "fixture/repo", "fixture-app.exe",
        "fixture-app.exe.sha256", {2, 0, 0}, temporary};
    value.download.parallel_threshold = 1024 * 1024;
    value.download.max_connections = 4;
    return value;
}

ClientOptions contract_options(const std::filesystem::path& temporary) {
    return {L"desktop-update-kit-contract-fixture", "fixture/repo", "fixture.exe",
        "fixture.exe.sha256", {1, 0, 0}, temporary};
}

std::string read_text(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), {}};
}

void expect_download_failure(const std::filesystem::path& temporary,
    const std::shared_ptr<FixtureTransport>& transport, const char* message) {
    try {
        UpdateClient client(options(temporary), transport);
        const auto release = client.check_for_update();
        if (!release) { expect(false, message); return; }
        DownloadControl control;
        (void)client.download_and_verify(*release, control);
        expect(false, message);
    } catch (const std::exception&) {
        expect(true, message);
    }
}
}

int wmain(int argc, wchar_t** argv) {
    const auto temporary = std::filesystem::temp_directory_path() / L"desktop-update-kit-transport-tests";
    std::error_code ignored;
    std::filesystem::remove_all(temporary, ignored);

    auto transport = std::make_shared<FixtureTransport>();
    UpdateClient client(options(temporary), transport);
    const auto release = client.check_for_update();
    expect(release.has_value(), "fixture manifest exposes a newer release");
    if (release) {
        try {
            DownloadControl control;
            std::atomic_bool saw_progress{};
            const auto path = client.download_and_verify(*release, control, [&](const DownloadProgress& progress) {
                if (progress.received > 0) saw_progress = true;
            });
            expect(std::filesystem::file_size(path) == release->size, "parallel fixture download has the expected size");
            expect(transport->range_requests() >= 5, "range probe and four ranges were requested");
            expect(saw_progress, "streaming download reports progress");
            std::filesystem::remove_all(path.parent_path(), ignored);
        } catch (const std::exception& error) {
            std::cerr << "parallel fixture exception: " << error.what() << '\n';
            expect(false, "parallel fixture download completes without exception");
        }
    }

    auto fallback_transport = std::make_shared<FixtureTransport>();
    fallback_transport->set_parallel_failure(true);
    UpdateClient fallback_client(options(temporary), fallback_transport);
    const auto fallback_release = fallback_client.check_for_update();
    if (fallback_release) {
        try {
            DownloadControl control;
            std::atomic_bool saw_fallback{};
            const auto path = fallback_client.download_and_verify(*fallback_release, control, [&](const DownloadProgress& progress) {
                if (progress.parallel_fallback) saw_fallback = true;
            });
            expect(std::filesystem::exists(path), "single connection fallback completes");
            expect(fallback_transport->full_requests() >= 1, "parallel failure falls back to a full request");
            expect(saw_fallback, "fallback state is reported to the UI");
            std::filesystem::remove_all(path.parent_path(), ignored);
        } catch (const std::exception& error) {
            std::cerr << "fallback fixture exception: " << error.what() << '\n';
            expect(false, "fallback fixture download completes without exception");
        }
    }

    auto malicious = std::make_shared<FixtureTransport>();
    malicious->set_bad_manifest(true);
    try {
        UpdateClient malicious_client(options(temporary), malicious);
        (void)malicious_client.check_for_update();
        expect(false, "a manifest cannot redirect the configured repository");
    } catch (const std::exception&) {
        expect(true, "malicious manifest is rejected");
    }

    auto insecure_node = std::make_shared<FixtureTransport>();
    insecure_node->set_insecure_node(true);
    try {
        UpdateClient insecure_client(options(temporary), insecure_node);
        (void)insecure_client.check_for_update();
        expect(false, "an HTTP third-party node cannot be hidden in the manifest");
    } catch (const std::exception&) {
        expect(true, "an HTTP third-party node is rejected with the manifest");
    }

    auto no_range = std::make_shared<FixtureTransport>();
    no_range->set_range_unsupported(true);
    try {
        UpdateClient no_range_client(options(temporary), no_range);
        const auto no_range_release = no_range_client.check_for_update();
        DownloadControl control;
        const auto path = no_range_client.download_and_verify(*no_range_release, control);
        expect(std::filesystem::exists(path), "a server that rejects Range falls back to a full request");
        expect(no_range->full_requests() >= 1, "Range rejection uses the single connection path");
        std::filesystem::remove_all(path.parent_path(), ignored);
    } catch (const std::exception&) {
        expect(false, "a server that rejects Range remains downloadable");
    }

    auto wrong_range = std::make_shared<FixtureTransport>();
    wrong_range->set_wrong_range(true);
    expect_download_failure(temporary, wrong_range, "an incorrect Content-Range is rejected");

    auto html = std::make_shared<FixtureTransport>();
    html->set_html_payload(true);
    expect_download_failure(temporary, html, "an HTML proxy response is rejected");

    auto bad_sha_asset = std::make_shared<FixtureTransport>();
    bad_sha_asset->set_bad_sha_asset(true);
    expect_download_failure(temporary, bad_sha_asset, "a mismatched published SHA asset is rejected");

    auto final_hash = std::make_shared<FixtureTransport>();
    final_hash->set_final_hash_mismatch(true);
    expect_download_failure(temporary, final_hash, "a final executable SHA mismatch is rejected");

    auto slow = std::make_shared<FixtureTransport>();
    slow->set_slow(true);
    UpdateClient slow_client(options(temporary), slow);
    const auto slow_release = slow_client.check_for_update();
    if (slow_release) {
        DownloadSession session(std::move(slow_client));
        std::atomic_int notifications{};
        session.set_changed_callback([&notifications](const SessionSnapshot&) { ++notifications; });
        expect(session.start(*slow_release), "download session starts");
        const auto began = std::chrono::steady_clock::now();
        expect(!session.start(*slow_release), "a duplicate start is rejected while downloading");
        const auto elapsed = std::chrono::steady_clock::now() - began;
        expect(elapsed < std::chrono::milliseconds(100), "duplicate start never waits for the active worker");
        expect(!session.set_acceleration(true),
            "setting the active acceleration mode is idempotent at the session boundary");
        expect(session.pause(), "active session can be paused");
        expect(session.snapshot().state == SessionState::paused, "paused session publishes its state");
        expect(session.continue_in_background(), "paused session can continue in the background");
        expect(session.snapshot().background, "background continuation is retained by the session");
        expect(session.cancel(), "active session can be cancelled");
        expect(session.wait_for_stop(std::chrono::seconds(2)), "cancelled session stops within its caller deadline");
        expect(session.snapshot().state == SessionState::cancelled, "cancelled session publishes its terminal state");
        session.set_changed_callback({});
        const auto detached_count = notifications.load();
        session.set_acceleration(false);
        expect(notifications.load() == detached_count, "detached session callbacks are not invoked again");
    }

    std::filesystem::remove_all(temporary, ignored);

    if (argc == 2) {
        const std::filesystem::path fixtures(argv[1]);
        auto valid_transport = std::make_shared<FixtureTransport>(read_text(fixtures / L"valid-update.json"));
        UpdateClient contract_client(contract_options(temporary), valid_transport);
        expect(contract_client.check_for_update().has_value(),
            "the shared valid manifest fixture is accepted by native");

        auto invalid_transport = std::make_shared<FixtureTransport>(
            read_text(fixtures / L"invalid-repository-update.json"));
        try {
            UpdateClient invalid_client(contract_options(temporary), invalid_transport);
            (void)invalid_client.check_for_update();
            expect(false, "the shared repository mismatch fixture is rejected by native");
        } catch (const std::exception&) {
            expect(true, "the shared repository mismatch fixture is rejected by native");
        }
    } else {
        expect(false, "the shared contract fixture directory is provided");
    }

    return failures ? 1 : 0;
}
