#include "DesktopUpdateKit/UpdateKit.h"

#include <windows.h>
#include <filesystem>
#include <string>

namespace {
using namespace desktop_update_kit;

bool wait_for_exit(int pid, int seconds) {
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
    if (!process) return GetLastError() == ERROR_INVALID_PARAMETER;
    const auto result = WaitForSingleObject(process, static_cast<DWORD>(std::max(1, seconds)) * 1000);
    CloseHandle(process); return result == WAIT_OBJECT_0;
}
bool start_target(const std::filesystem::path& target, const std::filesystem::path& marker, int seconds) {
    std::wstring command = L"\"" + target.wstring() + L"\" --update-health \"" + marker.wstring() + L"\"";
    STARTUPINFOW startup{sizeof(startup)}; PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, target.parent_path().c_str(), &startup, &process)) return false;
    const auto deadline = GetTickCount64() + static_cast<ULONGLONG>(std::max(1, seconds)) * 1000;
    bool healthy = false;
    while (GetTickCount64() < deadline) {
        if (std::filesystem::exists(marker)) { healthy = true; break; }
        if (WaitForSingleObject(process.hProcess, 0) == WAIT_OBJECT_0) break;
        Sleep(100);
    }
    if (!healthy) TerminateProcess(process.hProcess, 1);
    CloseHandle(process.hThread); CloseHandle(process.hProcess); return healthy;
}
void restart(const std::filesystem::path& target) {
    STARTUPINFOW startup{sizeof(startup)}; PROCESS_INFORMATION process{}; std::wstring command=L"\""+target.wstring()+L"\"";
    if (CreateProcessW(nullptr,command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,target.parent_path().c_str(),&startup,&process)) { CloseHandle(process.hThread); CloseHandle(process.hProcess); }
}
int update(const UpdateTransaction& tx) {
    if (!wait_for_exit(tx.parent_process_id, tx.parent_exit_timeout_seconds)) return 20;
    const auto staged = tx.target_exe.parent_path() / (L"." + tx.target_exe.filename().wstring() + L".new");
    std::error_code error;
    std::filesystem::copy_file(tx.downloaded_exe, staged, std::filesystem::copy_options::none, error);
    if (!error) std::filesystem::rename(tx.target_exe, tx.backup_exe, error);
    if (!error) std::filesystem::rename(staged, tx.target_exe, error);
    if (!error && start_target(tx.target_exe, tx.health_marker, tx.health_timeout_seconds)) { std::filesystem::remove(tx.backup_exe, error); return 0; }
    std::filesystem::remove(staged, error);
    if (std::filesystem::exists(tx.backup_exe)) { std::filesystem::remove(tx.target_exe, error); std::filesystem::rename(tx.backup_exe, tx.target_exe, error); }
    restart(tx.target_exe); return error ? 40 : 30;
}
int rename(const RenameTransaction& tx) {
    if (!wait_for_exit(tx.parent_process_id, tx.parent_exit_timeout_seconds)) return 20;
    std::error_code error; const bool target_present=std::filesystem::exists(tx.target_exe);
    if (target_present) std::filesystem::rename(tx.target_exe,tx.backup_exe,error);
    if (!error) std::filesystem::rename(tx.source_exe,tx.target_exe,error);
    if (!error && start_target(tx.target_exe,tx.health_marker,tx.health_timeout_seconds)) { std::filesystem::remove(tx.backup_exe,error); return 0; }
    if (std::filesystem::exists(tx.target_exe)) std::filesystem::rename(tx.target_exe,tx.source_exe,error);
    if (target_present && std::filesystem::exists(tx.backup_exe)) std::filesystem::rename(tx.backup_exe,tx.target_exe,error);
    restart(tx.source_exe); return error ? 40 : 30;
}
}
int wmain(int argc, wchar_t** argv) {
    if (argc != 3) return 10; std::string error;
    if (_wcsicmp(argv[1], L"--transaction") == 0) { auto tx=desktop_update_kit::read_update_transaction(argv[2],error); return tx?update(*tx):10; }
    if (_wcsicmp(argv[1], L"--rename-transaction") == 0) { auto tx=desktop_update_kit::read_rename_transaction(argv[2],error); return tx?rename(*tx):10; }
    return 10;
}
