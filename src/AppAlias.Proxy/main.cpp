#include <Windows.h>
#include <shellapi.h>

#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace
{
    std::wstring Utf8ToWide(const std::string& value)
    {
        if (value.empty())
        {
            return {};
        }

        const int size = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
        std::wstring result(static_cast<size_t>(size), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size);
        return result;
    }

    fs::path ModulePath()
    {
        std::wstring buffer(MAX_PATH, L'\0');
        DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0)
        {
            return {};
        }
        buffer.resize(length);
        return fs::path(buffer);
    }

    std::wstring ReadTarget()
    {
        std::ifstream file(ModulePath().parent_path() / L"alias.json", std::ios::binary);
        if (!file)
        {
            return {};
        }

        std::stringstream buffer;
        buffer << file.rdbuf();
        const std::wstring json = Utf8ToWide(buffer.str());
        std::wregex pattern(LR"json("target"\s*:\s*"([^"]+)")json");
        std::wsmatch match;
        if (!std::regex_search(json, match, pattern))
        {
            return {};
        }

        std::wstring target = match[1].str();
        for (size_t index = 0; index + 1 < target.size(); ++index)
        {
            if (target[index] == L'\\' && target[index + 1] == L'\\')
            {
                target.erase(index, 1);
            }
        }
        return target;
    }

    std::wstring Quote(const std::wstring& value)
    {
        std::wstring result = L"\"";
        for (const wchar_t ch : value)
        {
            if (ch == L'"')
            {
                result += L"\\\"";
            }
            else
            {
                result.push_back(ch);
            }
        }
        result.push_back(L'"');
        return result;
    }

    std::wstring BuildForwardedArgs()
    {
        int count = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &count);
        if (!argv)
        {
            return {};
        }

        std::wstring args;
        for (int index = 1; index < count; ++index)
        {
            if (!args.empty())
            {
                args.push_back(L' ');
            }
            args += Quote(argv[index]);
        }

        LocalFree(argv);
        return args;
    }

    bool EndsWithInsensitive(const std::wstring& value, const std::wstring& suffix)
    {
        if (value.size() < suffix.size())
        {
            return false;
        }

        for (size_t index = 0; index < suffix.size(); ++index)
        {
            if (towlower(value[value.size() - suffix.size() + index]) != towlower(suffix[index]))
            {
                return false;
            }
        }
        return true;
    }
}

int wmain()
{
    const std::wstring target = ReadTarget();
    if (target.empty())
    {
        fwprintf(stderr, L"alias target not configured\n");
        return 2;
    }

    const std::wstring args = BuildForwardedArgs();
    std::wstring application;
    std::wstring command;

    if (EndsWithInsensitive(target, L".cmd") || EndsWithInsensitive(target, L".bat"))
    {
        wchar_t systemDirectory[MAX_PATH]{};
        if (GetSystemDirectoryW(systemDirectory, MAX_PATH) == 0)
        {
            application = L"cmd.exe";
        }
        else
        {
            application = (fs::path(systemDirectory) / L"cmd.exe").wstring();
        }
        command = L"cmd.exe /d /c \"\"" + target + L"\"";
        if (!args.empty())
        {
            command.push_back(L' ');
            command += args;
        }
        command += L"\"";
    }
    else
    {
        application = target;
        command = Quote(target);
        if (!args.empty())
        {
            command.push_back(L' ');
            command += args;
        }
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');

    if (!CreateProcessW(application.c_str(), mutableCommand.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &startup, &process))
    {
        fwprintf(stderr, L"failed to launch alias target: %lu\n", GetLastError());
        return 3;
    }

    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exitCode = 1;
    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return static_cast<int>(exitCode);
}
