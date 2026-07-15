#include "DesktopUpdateKit/UpdateKit.h"

#include <windows.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>

namespace {
using namespace desktop_update_kit;

bool keep_transaction() noexcept {
    return GetEnvironmentVariableW(L"DUK_KEEP_TRANSACTION", nullptr, 0) != 0;
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
    const auto deadline = GetTickCount64() + static_cast<ULONGLONG>(std::clamp(seconds, 1, 300)) * 1000;
    bool healthy{};
    while (GetTickCount64() < deadline) {
        if (std::filesystem::exists(marker)) { healthy = true; break; }
        if (WaitForSingleObject(process.hProcess, 100) == WAIT_OBJECT_0) break;
    }
    if (!healthy) healthy = std::filesystem::exists(marker);
    if (!healthy) {
        TerminateProcess(process.hProcess, 1);
        WaitForSingleObject(process.hProcess, 3000);
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return healthy;
}

void restart(const std::filesystem::path& target) {
    PROCESS_INFORMATION process{};
    if (start_process(target, L"", process)) {
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
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
        wchar_t module_buffer[32768]{};
        const DWORD count = GetModuleFileNameW(nullptr, module_buffer, static_cast<DWORD>(std::size(module_buffer)));
        if (!count) return;
        const std::filesystem::path module(module_buffer);
        const auto directory = transaction_path.parent_path();
        wchar_t temporary_buffer[32768]{};
        const DWORD temporary_count = GetTempPathW(static_cast<DWORD>(std::size(temporary_buffer)), temporary_buffer);
        if (!temporary_count || temporary_count >= std::size(temporary_buffer)) return;
        const auto cleaner_directory = std::filesystem::path(temporary_buffer) / L"DesktopUpdateKit-native-cleaners";
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
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
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
        wchar_t self_buffer[32768]{};
        if (GetModuleFileNameW(nullptr, self_buffer, static_cast<DWORD>(std::size(self_buffer))))
            MoveFileExW(self_buffer, nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
        return 0;
    } catch (...) {
        return 40;
    }
}

void debug_event(const std::filesystem::path& transaction_path, const std::string& message) noexcept {
    if (!keep_transaction()) return;
    std::ofstream output(transaction_path.parent_path() / L"stub.log", std::ios::app);
    output << message << " error=" << GetLastError() << '\n';
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
    if (!CopyFileW(transaction.downloaded_exe.c_str(), staged.c_str(), TRUE)) {
        debug_event(transaction_path, "copy failed");
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
    if (!wait_for_exit(transaction.parent_process_id, transaction.parent_exit_timeout_seconds)) return 20;
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
