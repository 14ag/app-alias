#include "AppAliasCore.h"

#include <Windows.h>
#include <shellapi.h>

#include <exception>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace
{
    wchar_t AsciiLower(wchar_t ch)
    {
        if (ch >= L'A' && ch <= L'Z')
        {
            return static_cast<wchar_t>(ch - L'A' + L'a');
        }
        return ch;
    }

    std::wstring ReadTarget()
    {
        std::ifstream file(appalias::GetModulePath().parent_path() / L"alias.json", std::ios::binary);
        if (!file)
        {
            return {};
        }

        std::stringstream buffer;
        buffer << file.rdbuf();
        return appalias::JsonStringValue(appalias::Utf8ToWide(buffer.str()), L"target");
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
            args += appalias::QuoteCommandArgument(argv[index]);
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
            if (AsciiLower(value[value.size() - suffix.size() + index]) != AsciiLower(suffix[index]))
            {
                return false;
            }
        }
        return true;
    }
}

int wmain()
{
    try
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
                fwprintf(stderr, L"failed to locate system directory: %lu\n", GetLastError());
                return 3;
            }

            application = (fs::path(systemDirectory) / L"cmd.exe").wstring();
            command = appalias::QuoteCommandArgument(application) + L" /d /c \"\"" + target + L"\"";
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
            command = appalias::QuoteCommandArgument(target);
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
    catch (const std::exception& error)
    {
        fwprintf(stderr, L"alias proxy error: %s\n", appalias::Utf8ToWide(error.what()).c_str());
        return 3;
    }
}
