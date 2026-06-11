#include "AppAliasCore.h"

#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

namespace
{
    constexpr int ExitSuccess = 0;
    constexpr int ExitFailed = 1;
    constexpr int ExitUsage = 2;
    constexpr int ExitNotFound = 3;
    constexpr int ExitForeignAlias = 4;
    constexpr int ExitStubInvalid = 5;
    constexpr int ExitException = 6;

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

    void Usage(std::wostream& output)
    {
        output << L"Usage:\n"
            << L"  appalias create --alias <name.exe> --target <path> [--display-name <name>] [--publisher <name>] [--package-version <x.y.z.w>] [--force]\n"
            << L"  appalias list [--json]\n"
            << L"  appalias remove --alias <name.exe>|--package <name-or-full-name>\n"
            << L"  appalias verify --alias <name.exe>\n"
            << L"\nExit codes: 0 success, 1 failed, 2 usage, 3 not found, 4 foreign alias blocked, 5 stub invalid, 6 exception.\n";
    }

    int ExitCodeForResult(const appalias::OperationResult& result)
    {
        if (result.succeeded)
        {
            return ExitSuccess;
        }

        if (result.message.find(L"foreign") != std::wstring::npos)
        {
            return ExitForeignAlias;
        }

        if (result.message.find(L"not found") != std::wstring::npos)
        {
            return ExitNotFound;
        }

        if (result.message.find(L"stub") != std::wstring::npos)
        {
            return ExitStubInvalid;
        }

        return ExitFailed;
    }

    int PrintResult(const appalias::OperationResult& result)
    {
        std::wostream& diagnostic = result.succeeded ? std::wcout : std::wcerr;
        if (!result.message.empty())
        {
            diagnostic << result.message << L"\n";
        }

        if (!result.succeeded && result.errorCode != S_OK)
        {
            std::wcerr << L"ErrorCode: 0x" << std::hex << std::uppercase << result.errorCode << std::dec << L"\n";
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
            if (!result.record.installedPackagePath.empty())
            {
                std::wcout << L"InstalledPackagePath: " << result.record.installedPackagePath.wstring() << L"\n";
            }
            if (!result.record.stagedMsixPath.empty())
            {
                std::wcout << L"StagedMsixPath: " << result.record.stagedMsixPath.wstring() << L"\n";
            }
            std::wcout << L"StubExists: " << (result.record.stubExists ? L"true" : L"false") << L"\n";
            std::wcout << L"StubIsAppExecLink: " << (result.record.stubIsAppExecLink ? L"true" : L"false") << L"\n";
        }

        return ExitCodeForResult(result);
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
                << L"    \"installedPackagePath\": " << appalias::ToJsonString(record.installedPackagePath.wstring()) << L",\n"
                << L"    \"stagedMsixPath\": " << appalias::ToJsonString(record.stagedMsixPath.wstring()) << L",\n"
                << L"    \"externalLocation\": " << appalias::ToJsonString(record.externalLocation.wstring()) << L",\n"
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
        Usage(args.empty() ? std::wcerr : std::wcout);
        return args.empty() ? ExitUsage : ExitSuccess;
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
            options.packageVersion = ArgValue(args, L"--package-version");
            options.force = HasArg(args, L"--force");

            if (options.alias.empty() || options.targetPath.empty())
            {
                Usage(std::wcerr);
                return ExitUsage;
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
                std::wcout << record.alias << L"\t" << record.packageName << L"\t" << (record.owned ? L"owned" : L"foreign") << L"\n";
            }
            return ExitSuccess;
        }

        if (command == L"remove")
        {
            const std::wstring alias = ArgValue(args, L"--alias");
            const std::wstring package = ArgValue(args, L"--package");
            if (!alias.empty() && !package.empty())
            {
                std::wcerr << L"remove accepts either --alias or --package, not both\n";
                Usage(std::wcerr);
                return ExitUsage;
            }
            if (!alias.empty())
            {
                return PrintResult(appalias::RemoveAliasByAlias(alias));
            }
            if (!package.empty())
            {
                return PrintResult(appalias::RemoveAliasByPackage(package));
            }
            Usage(std::wcerr);
            return ExitUsage;
        }

        if (command == L"verify")
        {
            const std::wstring alias = ArgValue(args, L"--alias");
            if (alias.empty())
            {
                Usage(std::wcerr);
                return ExitUsage;
            }
            return PrintResult(appalias::VerifyAlias(alias));
        }
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << "\n";
        return ExitException;
    }

    Usage(std::wcerr);
    return ExitUsage;
}
