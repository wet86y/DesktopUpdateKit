#include "DesktopUpdateKit/UpdateKit.h"

#include <windows.h>
#include <filesystem>
#include <fstream>
#include <iostream>

using namespace desktop_update_kit;
namespace {
int failures{};
void expect(bool condition, const char* message) { if (!condition) { std::cerr << "FAILED: " << message << '\n'; ++failures; } }
std::filesystem::path write_text(const std::filesystem::path& path, const std::string& body) { std::ofstream out(path, std::ios::binary); out << body; return path; }
int run(const std::filesystem::path& program, const std::wstring& arguments) {
    std::wstring command=L"\""+program.wstring()+L"\" "+arguments; STARTUPINFOW startup{sizeof(startup)}; PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr,command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,program.parent_path().c_str(),&startup,&process)) return -1;
    WaitForSingleObject(process.hProcess, 10000); DWORD code=999; GetExitCodeProcess(process.hProcess,&code); CloseHandle(process.hThread); CloseHandle(process.hProcess); return static_cast<int>(code);
}
}
int wmain(int argc, wchar_t** argv) {
    expect(parse_version("1.2.3").has_value(), "three-part versions parse");
    expect(*parse_version("1.2.4") > *parse_version("1.2.3"), "version ordering compares patch");
    expect(!parse_version("1.2").has_value(), "incomplete version is rejected");
    DownloadControl control; control.pause(); expect(control.paused(), "pause changes state"); control.resume(); expect(!control.paused(), "resume changes state");
    control.use_acceleration(false); expect(!control.acceleration_enabled(), "direct node mode is remembered"); expect(control.consume_node_switch(), "mode switch requests a new node");
    const auto root=std::filesystem::temp_directory_path()/L"desktop-update-kit-native-tests"; std::error_code ignored; std::filesystem::remove_all(root,ignored); std::filesystem::create_directories(root);
    const auto transaction=root/L"update.json";
    write_text(transaction,R"({"ParentProcessId":1,"TargetExePath":"C:\\temp\\app.exe","DownloadedExePath":"C:\\temp\\new.exe","BackupExePath":"C:\\temp\\app.bak","HealthMarkerPath":"C:\\temp\\healthy.ok","ParentExitTimeoutSeconds":30,"HealthTimeoutSeconds":30})");
    std::string error; auto update=read_update_transaction(transaction,error); expect(update.has_value(), "managed update transaction shape remains readable"); if(update) expect(update->target_exe.filename()==L"app.exe", "transaction retains target path");
    write_text(transaction,R"({"ParentProcessId":1,"SourceExePath":"C:\\temp\\old.exe","TargetExePath":"C:\\temp\\new.exe","BackupExePath":"C:\\temp\\backup.exe","HealthMarkerPath":"C:\\temp\\healthy.ok","ParentExitTimeoutSeconds":30,"HealthTimeoutSeconds":30})");
    auto rename=read_rename_transaction(transaction,error); expect(rename.has_value(), "managed rename transaction shape remains readable");
    if (argc == 3) {
        const auto target=root/L"target.exe", downloaded=root/L"downloaded.exe", backup=root/L"target.bak", marker=root/L"healthy.ok", live_transaction=root/L"live.json";
        write_text(target,"old executable"); std::filesystem::copy_file(argv[2],downloaded);
        std::ofstream tx(live_transaction); tx << "{\"ParentProcessId\":999999,\"TargetExePath\":\"" << target.generic_string() << "\",\"DownloadedExePath\":\"" << downloaded.generic_string() << "\",\"BackupExePath\":\"" << backup.generic_string() << "\",\"HealthMarkerPath\":\"" << marker.generic_string() << "\",\"ParentExitTimeoutSeconds\":1,\"HealthTimeoutSeconds\":3}"; tx.close();
        expect(run(argv[1], L"--transaction \""+live_transaction.wstring()+L"\"")==0, "native stub replaces and health-checks a test host");
        expect(std::filesystem::exists(marker), "test host wrote native health marker"); expect(std::filesystem::exists(target), "native stub left target executable in place"); expect(!std::filesystem::exists(backup), "native stub removed backup after healthy launch");
    }
    std::filesystem::remove_all(root,ignored); return failures ? 1 : 0;
}
