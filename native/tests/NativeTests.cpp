#include "DesktopUpdateKit/UpdateKit.h"

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <iostream>

using namespace desktop_update_kit;

namespace {
int failures{};

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

std::filesystem::path write_text(const std::filesystem::path& path, const std::string& body) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output << body;
    return path;
}

int run(const std::filesystem::path& program, const std::wstring& arguments, DWORD timeout = 15000) {
    std::wstring command = L"\"" + std::filesystem::absolute(program).wstring() + L"\" " + arguments;
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
            program.parent_path().c_str(), &startup, &process)) return -1;
    const auto wait = WaitForSingleObject(process.hProcess, timeout);
    DWORD code = 999;
    if (wait == WAIT_TIMEOUT) TerminateProcess(process.hProcess, 998);
    else GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return wait == WAIT_TIMEOUT ? 998 : static_cast<int>(code);
}

void write_update(const std::filesystem::path& transaction_path, const std::filesystem::path& target,
    const std::filesystem::path& downloaded, const std::filesystem::path& backup,
    const std::filesystem::path& marker, int health_seconds = 3) {
    UpdateTransaction transaction{999999, std::filesystem::absolute(target), std::filesystem::absolute(downloaded),
        std::filesystem::absolute(backup), std::filesystem::absolute(marker), 1, health_seconds};
    std::string error;
    expect(write_update_transaction(transaction_path, transaction, error), "native update transaction can be written");
}
}

int wmain(int argc, wchar_t** argv) {
    expect(parse_version("1.2.3").has_value(), "three-part versions parse");
    expect(parse_version("v2.0.1-integration").has_value(), "tagged integration versions parse");
    expect(*parse_version("1.2.4") > *parse_version("1.2.3"), "version ordering compares patch");
    expect(!parse_version("1.2").has_value(), "incomplete version is rejected");

    DownloadControl control;
    control.pause();
    expect(control.paused(), "pause changes state");
    control.resume();
    expect(!control.paused(), "resume changes state");
    control.use_acceleration(false);
    expect(!control.acceleration_enabled(), "direct node mode is remembered");
    expect(control.consume_node_switch(), "mode switch requests a new node");

    const auto root = std::filesystem::temp_directory_path() / L"desktop-update-kit-native-tests";
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    std::filesystem::create_directories(root);
    const auto transaction = root / L"shape.json";
    write_text(transaction, R"({"ParentProcessId":1,"TargetExePath":"C:\\temp\\app.exe","DownloadedExePath":"C:\\temp\\new.exe","BackupExePath":"C:\\temp\\app.bak","HealthMarkerPath":"C:\\temp\\healthy.ok","ParentExitTimeoutSeconds":30,"HealthTimeoutSeconds":30})");
    std::string error;
    auto update = read_update_transaction(transaction, error);
    expect(update.has_value(), "managed update transaction shape remains readable");
    if (update) expect(update->target_exe.filename() == L"app.exe", "transaction retains target path");
    write_text(transaction, R"({"ParentProcessId":1,"SourceExePath":"C:\\temp\\old.exe","TargetExePath":"C:\\temp\\new.exe","BackupExePath":"C:\\temp\\backup.exe","HealthMarkerPath":"C:\\temp\\healthy.ok","ParentExitTimeoutSeconds":30,"HealthTimeoutSeconds":30})");
    auto rename = read_rename_transaction(transaction, error);
    expect(rename.has_value(), "managed rename transaction shape remains readable");

    if (argc == 3) {
        const auto source_stub = std::filesystem::absolute(argv[1]);
        const auto test_host = std::filesystem::absolute(argv[2]);
        SetEnvironmentVariableW(L"DUK_KEEP_TRANSACTION", L"1");

        const auto success = root / L"success target";
        const auto success_tx = root / L"success transaction";
        std::filesystem::create_directories(success);
        std::filesystem::create_directories(success_tx);
        const auto target = success / L"超级中键.exe";
        const auto downloaded = success / L"downloaded.exe";
        const auto backup = success / L"target.bak";
        const auto marker = success_tx / L"healthy.ok";
        const auto live_transaction = success_tx / L"update.json";
        const auto stub = success_tx / L"UpdaterStub.exe";
        std::filesystem::copy_file(test_host, target);
        std::filesystem::copy_file(test_host, downloaded);
        std::filesystem::copy_file(source_stub, stub);
        write_update(live_transaction, target, downloaded, backup, marker);
        expect(run(stub, L"--transaction \"" + live_transaction.wstring() + L"\"") == 0,
            "native stub replaces and health-checks a test host");
        expect(std::filesystem::exists(marker), "test host wrote native health marker");
        expect(std::filesystem::exists(target), "native stub left target executable in place");
        expect(!std::filesystem::exists(backup), "native stub removed backup after healthy launch");

        const auto missing = root / L"missing target";
        const auto missing_tx = root / L"missing transaction";
        std::filesystem::create_directories(missing);
        std::filesystem::create_directories(missing_tx);
        const auto missing_target = missing / L"超级中键.exe";
        const auto missing_downloaded = missing / L"downloaded.exe";
        const auto missing_backup = missing / L"target.bak";
        const auto missing_marker = missing_tx / L"healthy.ok";
        const auto missing_transaction = missing_tx / L"update.json";
        const auto missing_stub = missing_tx / L"UpdaterStub.exe";
        std::filesystem::copy_file(test_host, missing_downloaded);
        std::filesystem::copy_file(source_stub, missing_stub);
        write_update(missing_transaction, missing_target, missing_downloaded, missing_backup, missing_marker);
        expect(run(missing_stub, L"--transaction \"" + missing_transaction.wstring() + L"\"") == 0,
            "native stub installs when the target executable is absent");
        expect(std::filesystem::exists(missing_target), "missing target is created by the update transaction");
        expect(!std::filesystem::exists(missing_backup), "missing target transaction does not invent a backup");

        const auto rollback = root / L"rollback target";
        const auto rollback_tx = root / L"rollback transaction";
        std::filesystem::create_directories(rollback);
        std::filesystem::create_directories(rollback_tx);
        const auto rollback_target = rollback / L"超级中键.exe";
        const auto rollback_downloaded = rollback / L"downloaded.exe";
        const auto rollback_backup = rollback / L"target.bak";
        const auto rollback_marker = rollback_tx / L"healthy.ok";
        const auto rollback_transaction = rollback_tx / L"update.json";
        const auto rollback_stub = rollback_tx / L"UpdaterStub.exe";
        std::filesystem::copy_file(test_host, rollback_target);
        std::filesystem::copy_file(test_host, rollback_downloaded);
        std::filesystem::copy_file(source_stub, rollback_stub);
        write_update(rollback_transaction, rollback_target, rollback_downloaded, rollback_backup, rollback_marker, 1);
        SetEnvironmentVariableW(L"DUK_TEST_FAIL_HEALTH", L"1");
        expect(run(rollback_stub, L"--transaction \"" + rollback_transaction.wstring() + L"\"") == 30,
            "native stub reports health failure after rollback");
        SetEnvironmentVariableW(L"DUK_TEST_FAIL_HEALTH", nullptr);
        expect(std::filesystem::exists(rollback_target), "rollback restored the original executable");
        expect(!std::filesystem::exists(rollback_backup), "rollback consumed the backup");
        SetEnvironmentVariableW(L"DUK_KEEP_TRANSACTION", nullptr);

        const auto cleanup = root / L"cleanup target";
        const auto cleanup_tx = root / L"cleanup transaction";
        std::filesystem::create_directories(cleanup);
        std::filesystem::create_directories(cleanup_tx);
        const auto cleanup_target = cleanup / L"超级中键.exe";
        const auto cleanup_downloaded = cleanup / L"downloaded.exe";
        const auto cleanup_backup = cleanup / L"target.bak";
        const auto cleanup_marker = cleanup_tx / L"healthy.ok";
        const auto cleanup_transaction = cleanup_tx / L"update.json";
        const auto cleanup_stub = cleanup_tx / L"UpdaterStub.exe";
        std::filesystem::copy_file(test_host, cleanup_target);
        std::filesystem::copy_file(test_host, cleanup_downloaded);
        std::filesystem::copy_file(source_stub, cleanup_stub);
        write_update(cleanup_transaction, cleanup_target, cleanup_downloaded, cleanup_backup, cleanup_marker);
        expect(run(cleanup_stub, L"--transaction \"" + cleanup_transaction.wstring() + L"\"") == 0,
            "native stub completes a production-cleanup transaction");
        const auto cleanup_deadline = GetTickCount64() + 5000;
        while (std::filesystem::exists(cleanup_tx) && GetTickCount64() < cleanup_deadline) Sleep(50);
        expect(!std::filesystem::exists(cleanup_tx), "stub self-cleaner removes the empty transaction directory");
        expect(!std::filesystem::exists(cleanup_downloaded), "stub consumes the downloaded payload");
    }

    std::filesystem::remove_all(root, ignored);
    return failures ? 1 : 0;
}
