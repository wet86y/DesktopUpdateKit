#include <windows.h>
#include <filesystem>
#include <string>

int wmain(int argc, wchar_t** argv) {
    if (argc == 3 && _wcsicmp(argv[1], L"--update-health") == 0) {
        HANDLE marker = CreateFileW(argv[2], GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (marker == INVALID_HANDLE_VALUE) return 2;
        CloseHandle(marker); return 0;
    }
    return 0;
}
