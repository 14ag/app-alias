#include "AppAliasCore.h"

#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

namespace
{
    std::wstring ArgValue(const std::vector<std::wstring>& args, const std::wstring& name)
    {
        for (size_t index = 0; index + 1 < args.size(); ++index)
        {
            if (args[index] == name)
            {
                return args[index + 1];
            }
        }
        return {};
    }

    bool HasArg(const std::vector<std::wstring>& args, const std::wstring& name)
    {
        return std::find(args.begin(), args.end(), name) != args.end();
    }

    void Usage()
    {
        std::wcout << L"Usage:\n"
            << L"  appalias create --alias <name.exe> --target <path> [--display-name <name>] [--publisher <name>] [--force]\n"
            << L"  appalias list [--json]\n"
            << L"  appalias remove --alias <name.exe>|--package <name-or-full-name>\n"
            << L"  appalias verify --alias <name.exe>\n";
    }

    int PrintResult(const appalias::OperationResult& result)
    {
        if (!result.message.empty())
        {
            std::wcout << result.message << L"\n";
        }

        if (!result.succeeded && result.errorCode != 0)
        {
            std::wcout << L"ErrorCode: 0x" << std::hex << std::uppercase << result.errorCode << std::dec << L"\n";
        }

        if (!result.record.alias.empty())
        {
            std::wcout << L"Alias: " << result.record.alias << L"\n";
            if (!result.record.packageName.empty())
            {
                std::wcout << L"Package: " << result.record.packageName << L"\n";
            }
            if (!result.record.targetPath.empty())
            {
                std::wcout << L"Target: " << result.record.targetPath.wstring() << L"\n";
            }
            std::wcout << L"StubExists: " << (result.record.stubExists ? L"true" : L"false") << L"\n";
            std::wcout << L"StubIsAppExecLink: " << (result.record.stubIsAppExecLink ? L"true" : L"false") << L"\n";
        }

        return result.succeeded ? 0 : 1;
    }

    void PrintJsonList(const std::vector<appalias::AliasRecord>& records)
    {
        std::wcout << L"[\n";
        for (size_t index = 0; index < records.size(); ++index)
        {
            const auto& record = records[index];
            std::wcout << L"  {\n"
                << L"    \"alias\": " << appalias::ToJsonString(record.alias) << L",\n"
                << L"    \"packageName\": " << appalias::ToJsonString(record.packageName) << L",\n"
                << L"    \"packageFamilyName\": " << appalias::ToJsonString(record.packageFamilyName) << L",\n"
                << L"    \"packageFullName\": " << appalias::ToJsonString(record.packageFullName) << L",\n"
                << L"    \"target\": " << appalias::ToJsonString(record.targetPath.wstring()) << L",\n"
                << L"    \"owned\": " << (record.owned ? L"true" : L"false") << L",\n"
                << L"    \"stubExists\": " << (record.stubExists ? L"true" : L"false") << L",\n"
                << L"    \"stubIsAppExecLink\": " << (record.stubIsAppExecLink ? L"true" : L"false") << L"\n"
                << L"  }" << (index + 1 == records.size() ? L"" : L",") << L"\n";
        }
        std::wcout << L"]\n";
    }
}

int wmain(int argc, wchar_t** argv)
{
    std::vector<std::wstring> args(argv + 1, argv + argc);
    if (args.empty() || args[0] == L"--help" || args[0] == L"-h")
    {
        Usage();
        return args.empty() ? 1 : 0;
    }

    const std::wstring command = args[0];
    try
    {
        if (command == L"create")
        {
            appalias::AliasCreateOptions options{};
            options.alias = ArgValue(args, L"--alias");
            options.targetPath = ArgValue(args, L"--target");
            options.displayName = ArgValue(args, L"--display-name");
            options.publisherDisplayName = ArgValue(args, L"--publisher");
            options.force = HasArg(args, L"--force");

            if (options.alias.empty() || options.targetPath.empty())
            {
                Usage();
                return 1;
            }

            return PrintResult(appalias::CreateAlias(options));
        }

        if (command == L"list")
        {
            const auto records = appalias::ListAliases();
            if (HasArg(args, L"--json"))
            {
                PrintJsonList(records);
                return 0;
            }

            for (const auto& record : records)
            {
                std::wcout << record.alias << L"  " << record.packageName << L"  " << (record.owned ? L"owned" : L"foreign") << L"\n";
            }
            return 0;
        }

        if (command == L"remove")
        {
            const std::wstring alias = ArgValue(args, L"--alias");
            const std::wstring package = ArgValue(args, L"--package");
            if (!alias.empty())
            {
                return PrintResult(appalias::RemoveAliasByAlias(alias));
            }
            if (!package.empty())
            {
                return PrintResult(appalias::RemoveAliasByPackage(package));
            }
            Usage();
            return 1;
        }

        if (command == L"verify")
        {
            const std::wstring alias = ArgValue(args, L"--alias");
            if (alias.empty())
            {
                Usage();
                return 1;
            }
            return PrintResult(appalias::VerifyAlias(alias));
        }
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << "\n";
        return 1;
    }

    Usage();
    return 1;
}
