#include "DesktopUpdateKit/UpdateKit.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace {
using namespace desktop_update_kit;

class unique_handle {
public:
    explicit unique_handle(HANDLE value = INVALID_HANDLE_VALUE) noexcept : value_(value) {}
    ~unique_handle() { if (value_ != INVALID_HANDLE_VALUE && value_ != nullptr) CloseHandle(value_); }
    unique_handle(const unique_handle&) = delete;
    unique_handle& operator=(const unique_handle&) = delete;
    [[nodiscard]] HANDLE get() const noexcept { return value_; }
    [[nodiscard]] explicit operator bool() const noexcept {
        return value_ != INVALID_HANDLE_VALUE && value_ != nullptr;
    }
private:
    HANDLE value_;
};

class bcrypt_algorithm {
public:
    ~bcrypt_algorithm() { if (value_) BCryptCloseAlgorithmProvider(value_, 0); }
    BCRYPT_ALG_HANDLE* put() noexcept { return &value_; }
    [[nodiscard]] BCRYPT_ALG_HANDLE get() const noexcept { return value_; }
private:
    BCRYPT_ALG_HANDLE value_{};
};

class bcrypt_hash {
public:
    ~bcrypt_hash() { if (value_) BCryptDestroyHash(value_); }
    BCRYPT_HASH_HANDLE* put() noexcept { return &value_; }
    [[nodiscard]] BCRYPT_HASH_HANDLE get() const noexcept { return value_; }
private:
    BCRYPT_HASH_HANDLE value_{};
};

bool copy_verified_payload(const std::filesystem::path& source, const std::filesystem::path& target,
    const std::string& expected_sha256) {
    // Omitting FILE_SHARE_WRITE and FILE_SHARE_DELETE binds verification and copying to one immutable file object.
    unique_handle input(CreateFileW(source.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (!input) return false;
    unique_handle output(CreateFileW(target.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (!output) return false;

    bcrypt_algorithm algorithm;
    DWORD object_size{};
    DWORD bytes{};
    if (BCryptOpenAlgorithmProvider(algorithm.put(), BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 ||
        BCryptGetProperty(algorithm.get(), BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&object_size),
            sizeof(object_size), &bytes, 0) < 0) return false;
    std::vector<UCHAR> hash_object(object_size);
    bcrypt_hash hash;
    if (BCryptCreateHash(algorithm.get(), hash.put(), hash_object.data(), object_size, nullptr, 0, 0) < 0)
        return false;

    std::vector<std::byte> buffer(1024 * 1024);
    for (;;) {
        DWORD read{};
        if (!ReadFile(input.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) return false;
        if (read == 0) break;
        if (BCryptHashData(hash.get(), reinterpret_cast<PUCHAR>(buffer.data()), read, 0) < 0) return false;
        DWORD offset{};
        while (offset < read) {
            DWORD written{};
            if (!WriteFile(output.get(), buffer.data() + offset, read - offset, &written, nullptr) || written == 0)
                return false;
            offset += written;
        }
    }
    if (!FlushFileBuffers(output.get())) return false;
    std::array<UCHAR, 32> digest{};
    if (BCryptFinishHash(hash.get(), digest.data(), static_cast<ULONG>(digest.size()), 0) < 0) return false;
    std::ostringstream actual;
    for (const auto byte : digest)
        actual << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(byte);
    return _stricmp(actual.str().c_str(), expected_sha256.c_str()) == 0;
}

bool keep_transaction() noexcept {
#if defined(DUK_DIAGNOSTICS)
    return GetEnvironmentVariableW(L"DUK_KEEP_TRANSACTION", nullptr, 0) != 0;
#else
    return false;
#endif
}

bool wait_for_exit(int process_id, int seconds) {
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(process_id));
    if (!process) return GetLastError() == ERROR_INVALID_PARAMETER;
    const DWORD timeout = static_cast<DWORD>(std::clamp(seconds, 1, 300)) * 1000;
    const auto result = WaitForSingleObject(process, timeout);
    CloseHandle(process);
    return result == WAIT_OBJECT_0;
}

bool start_process(const std::filesystem::path& target, const std::wstring& arguments,
    PROCESS_INFORMATION& process) {
    std::wstring command = L"\"" + target.wstring() + L"\"";
    if (!arguments.empty()) command += L" " + arguments;
    STARTUPINFOW startup{sizeof(startup)};
    return CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
        target.parent_path().c_str(), &startup, &process) != FALSE;
}

bool start_and_wait_for_health(const std::filesystem::path& target,
    const std::filesystem::path& marker, int seconds) {
    std::error_code ignored;
    std::filesystem::remove(marker, ignored);
    PROCESS_INFORMATION process{};
    const auto arguments = L"--update-health \"" + marker.wstring() + L"\"";
    if (!start_process(target, arguments, process)) {
        if (keep_transaction()) {
            std::ofstream output(marker.parent_path() / L"start-error.log");
            output << GetLastError();
        }
        return false;
    }
    if (!process.hProcess || !process.hThread) {
        if (process.hThread) CloseHandle(process.hThread);
        if (process.hProcess) CloseHandle(process.hProcess);
        return false;
    }
    unique_handle process_handle(process.hProcess);
    unique_handle thread_handle(process.hThread);
    const auto deadline = GetTickCount64() + static_cast<ULONGLONG>(std::clamp(seconds, 1, 300)) * 1000;
    bool healthy{};
    while (GetTickCount64() < deadline) {
        if (std::filesystem::exists(marker)) { healthy = true; break; }
        if (WaitForSingleObject(process_handle.get(), 100) == WAIT_OBJECT_0) break;
    }
    if (!healthy) healthy = std::filesystem::exists(marker);
    if (!healthy) {
        TerminateProcess(process_handle.get(), 1);
        WaitForSingleObject(process_handle.get(), 3000);
    }
    return healthy;
}

void restart(const std::filesystem::path& target) {
    PROCESS_INFORMATION process{};
    if (start_process(target, L"", process)) {
        if (process.hThread) CloseHandle(process.hThread);
        if (process.hProcess) CloseHandle(process.hProcess);
    }
}

void remove_if_exists(const std::filesystem::path& path) noexcept {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

void cleanup_download_payload(const std::filesystem::path& executable) noexcept {
    remove_if_exists(executable);
    remove_if_exists(executable.wstring() + L".sha256");
    RemoveDirectoryW(executable.parent_path().c_str());
}

bool move_no_replace(const std::filesystem::path& source, const std::filesystem::path& target) {
    return MoveFileExW(source.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH) != FALSE;
}

void schedule_cleanup(const std::filesystem::path& transaction_path) noexcept {
    try {
        if (keep_transaction()) return;
        std::vector<wchar_t> module_buffer(32768);
        const DWORD count = GetModuleFileNameW(nullptr, module_buffer.data(), static_cast<DWORD>(module_buffer.size()));
        if (!count) return;
        const std::filesystem::path module(module_buffer.data());
        const auto directory = transaction_path.parent_path();
        std::vector<wchar_t> temporary_buffer(32768);
        const DWORD temporary_count = GetTempPathW(static_cast<DWORD>(temporary_buffer.size()), temporary_buffer.data());
        if (!temporary_count || temporary_count >= temporary_buffer.size()) return;
        const auto cleaner_directory = std::filesystem::path(temporary_buffer.data()) / L"DesktopUpdateKit-native-cleaners";
        std::filesystem::create_directories(cleaner_directory);
        const auto cleaner = cleaner_directory /
            (L"cleanup-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()) + L".exe");
        if (!CopyFileW(module.c_str(), cleaner.c_str(), TRUE)) {
            MoveFileExW(module.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
            MoveFileExW(transaction_path.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
            return;
        }
        std::wstring command = L"\"" + cleaner.wstring() + L"\" --cleanup " +
            std::to_wstring(GetCurrentProcessId()) + L" \"" + module.wstring() + L"\" \"" +
            transaction_path.wstring() + L"\" \"" + directory.wstring() + L"\"";
        STARTUPINFOW startup{sizeof(startup)};
        PROCESS_INFORMATION process{};
        if (CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
                CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP, nullptr, cleaner_directory.c_str(), &startup, &process)) {
            if (process.hThread) CloseHandle(process.hThread);
            if (process.hProcess) CloseHandle(process.hProcess);
        } else {
            remove_if_exists(cleaner);
            MoveFileExW(module.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
            MoveFileExW(transaction_path.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
        }
    } catch (...) {
    }
}

int cleanup_after_stub(int process_id, const std::filesystem::path& module,
    const std::filesystem::path& transaction, const std::filesystem::path& directory) noexcept {
    try {
        if (module.parent_path() != directory || transaction.parent_path() != directory ||
            _wcsicmp(module.filename().c_str(), L"UpdaterStub.exe") != 0 ||
            (_wcsicmp(transaction.filename().c_str(), L"update.json") != 0 &&
             _wcsicmp(transaction.filename().c_str(), L"rename.json") != 0)) return 10;
        (void)wait_for_exit(process_id, 30);
        remove_if_exists(module);
        remove_if_exists(transaction);
        RemoveDirectoryW(directory.c_str());
        std::vector<wchar_t> self_buffer(32768);
        if (GetModuleFileNameW(nullptr, self_buffer.data(), static_cast<DWORD>(self_buffer.size())))
            MoveFileExW(self_buffer.data(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
        return 0;
    } catch (...) {
        return 40;
    }
}

void debug_event(const std::filesystem::path& transaction_path, const std::string& message) noexcept {
#if defined(DUK_DIAGNOSTICS)
    if (!keep_transaction()) return;
    std::ofstream output(transaction_path.parent_path() / L"stub.log", std::ios::app);
    output << message << " error=" << GetLastError() << '\n';
#else
    (void)transaction_path;
    (void)message;
#endif
}

int update(const UpdateTransaction& transaction, const std::filesystem::path& transaction_path) {
    if (!wait_for_exit(transaction.parent_process_id, transaction.parent_exit_timeout_seconds)) {
        cleanup_download_payload(transaction.downloaded_exe);
        schedule_cleanup(transaction_path);
        return 20;
    }
    const auto staged = transaction.target_exe.parent_path() /
        (L"." + transaction.target_exe.filename().wstring() + L"." + std::to_wstring(GetCurrentProcessId()) + L".new");
    remove_if_exists(staged);
    remove_if_exists(transaction.health_marker);
    if (!copy_verified_payload(transaction.downloaded_exe, staged, transaction.expected_sha256)) {
        debug_event(transaction_path, "payload verification or copy failed");
        remove_if_exists(staged);
        cleanup_download_payload(transaction.downloaded_exe);
        schedule_cleanup(transaction_path);
        return 40;
    }
    const bool target_present = std::filesystem::exists(transaction.target_exe);
    bool backup_created{};
    if (target_present) {
        if (!move_no_replace(transaction.target_exe, transaction.backup_exe)) {
            debug_event(transaction_path, "backup move failed");
            remove_if_exists(staged);
            cleanup_download_payload(transaction.downloaded_exe);
            schedule_cleanup(transaction_path);
            return 40;
        }
        backup_created = true;
    }
    if (!move_no_replace(staged, transaction.target_exe)) {
        debug_event(transaction_path, "staged move failed");
        move_no_replace(transaction.backup_exe, transaction.target_exe);
        remove_if_exists(staged);
        cleanup_download_payload(transaction.downloaded_exe);
        schedule_cleanup(transaction_path);
        return 40;
    }
    if (start_and_wait_for_health(transaction.target_exe, transaction.health_marker,
            transaction.health_timeout_seconds)) {
        remove_if_exists(transaction.backup_exe);
        cleanup_download_payload(transaction.downloaded_exe);
        if (!keep_transaction()) remove_if_exists(transaction.health_marker);
        schedule_cleanup(transaction_path);
        return 0;
    }
    debug_event(transaction_path, "health failed");
    remove_if_exists(transaction.target_exe);
    const bool restored = backup_created && move_no_replace(transaction.backup_exe, transaction.target_exe);
    if (restored) restart(transaction.target_exe);
    cleanup_download_payload(transaction.downloaded_exe);
    schedule_cleanup(transaction_path);
    return restored || !target_present ? 30 : 40;
}

int rename_executable(const RenameTransaction& transaction, const std::filesystem::path& transaction_path) {
    if (!wait_for_exit(transaction.parent_process_id, transaction.parent_exit_timeout_seconds)) {
        schedule_cleanup(transaction_path);
        return 20;
    }
    remove_if_exists(transaction.health_marker);
    const bool target_present = std::filesystem::exists(transaction.target_exe);
    if (target_present && !move_no_replace(transaction.target_exe, transaction.backup_exe)) {
        schedule_cleanup(transaction_path);
        return 40;
    }
    if (!move_no_replace(transaction.source_exe, transaction.target_exe)) {
        if (target_present) move_no_replace(transaction.backup_exe, transaction.target_exe);
        schedule_cleanup(transaction_path);
        return 40;
    }
    if (start_and_wait_for_health(transaction.target_exe, transaction.health_marker,
            transaction.health_timeout_seconds)) {
        remove_if_exists(transaction.backup_exe);
        if (!keep_transaction()) remove_if_exists(transaction.health_marker);
        schedule_cleanup(transaction_path);
        return 0;
    }
    const bool source_restored = move_no_replace(transaction.target_exe, transaction.source_exe);
    bool prior_target_restored = true;
    if (target_present) prior_target_restored = move_no_replace(transaction.backup_exe, transaction.target_exe);
    if (source_restored) restart(transaction.source_exe);
    schedule_cleanup(transaction_path);
    return source_restored && prior_target_restored ? 30 : 40;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc == 6 && _wcsicmp(argv[1], L"--cleanup") == 0)
        return cleanup_after_stub(_wtoi(argv[2]), argv[3], argv[4], argv[5]);
    if (argc != 3) return 10;
    std::string error;
    const std::filesystem::path transaction_path(argv[2]);
    if (_wcsicmp(argv[1], L"--transaction") == 0) {
        auto transaction = desktop_update_kit::read_update_transaction(transaction_path, error);
        return transaction ? update(*transaction, transaction_path) : 10;
    }
    if (_wcsicmp(argv[1], L"--rename-transaction") == 0) {
        auto transaction = desktop_update_kit::read_rename_transaction(transaction_path, error);
        return transaction ? rename_executable(*transaction, transaction_path) : 10;
    }
    return 10;
}
