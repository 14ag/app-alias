#include "AppAliasCore.h"

#include <winrt/base.h>

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <map>
#include <set>
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

    struct ParsedArgs
    {
        std::wstring command;
        std::map<std::wstring, std::wstring> values;
        std::set<std::wstring> flags;
        std::wstring error;
    };

    bool StartsWithOptionPrefix(const std::wstring& value)
    {
        return value.size() > 2 && value[0] == L'-' && value[1] == L'-';
    }

    bool IsFlag(const std::wstring& value)
    {
        return value == L"--force" || value == L"--json";
    }

    ParsedArgs ParseArgs(const std::vector<std::wstring>& args)
    {
        ParsedArgs parsed{};
        if (args.empty())
        {
            return parsed;
        }

        parsed.command = args[0];
        for (size_t index = 1; index < args.size(); ++index)
        {
            const std::wstring& item = args[index];
            if (!StartsWithOptionPrefix(item))
            {
                parsed.error = L"unexpected argument: " + item;
                return parsed;
            }

            if (IsFlag(item))
            {
                parsed.flags.insert(item);
                continue;
            }

            if (index + 1 >= args.size() || StartsWithOptionPrefix(args[index + 1]))
            {
                parsed.error = L"missing value for " + item;
                return parsed;
            }

            parsed.values[item] = args[index + 1];
            ++index;
        }

        return parsed;
    }

    std::wstring ArgValue(const ParsedArgs& args, const std::wstring& name)
    {
        const auto found = args.values.find(name);
        return found == args.values.end() ? std::wstring{} : found->second;
    }

    bool HasArg(const ParsedArgs& args, const std::wstring& name)
    {
        return args.flags.find(name) != args.flags.end() || args.values.find(name) != args.values.end();
    }

    std::wstring ToJsonString(std::wstring_view value)
    {
        std::wstring result = L"\"";
        constexpr wchar_t hex[] = L"0123456789ABCDEF";
        for (const wchar_t ch : value)
        {
            switch (ch)
            {
            case L'\\': result += L"\\\\"; break;
            case L'"': result += L"\\\""; break;
            case L'\b': result += L"\\b"; break;
            case L'\f': result += L"\\f"; break;
            case L'\n': result += L"\\n"; break;
            case L'\r': result += L"\\r"; break;
            case L'\t': result += L"\\t"; break;
            default:
                if (ch < 0x20)
                {
                    result += L"\\u00";
                    result.push_back(hex[(ch >> 4) & 0x0F]);
                    result.push_back(hex[ch & 0x0F]);
                }
                else
                {
                    result.push_back(ch);
                }
                break;
            }
        }
        result.push_back(L'"');
        return result;
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

        switch (result.errorKind)
        {
        case appalias::OperationErrorKind::None:
            return ExitFailed;
        case appalias::OperationErrorKind::NotFound:
            return ExitNotFound;
        case appalias::OperationErrorKind::ForeignAlias:
            return ExitForeignAlias;
        case appalias::OperationErrorKind::StubInvalid:
            return ExitStubInvalid;
        case appalias::OperationErrorKind::Timeout:
        case appalias::OperationErrorKind::Failed:
        default:
            return ExitFailed;
        }
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
            std::wcerr << L"ErrorCode: 0x" << std::hex << std::uppercase << static_cast<std::uint32_t>(result.errorCode) << std::dec << L"\n";
        }

        std::wostream& recordOutput = result.succeeded ? std::wcout : std::wcerr;
        if (!result.record.alias.empty())
        {
            recordOutput << L"Alias: " << result.record.alias << L"\n";
            if (!result.record.packageName.empty())
            {
                recordOutput << L"Package: " << result.record.packageName << L"\n";
            }
            if (!result.record.targetPath.empty())
            {
                recordOutput << L"Target: " << result.record.targetPath.wstring() << L"\n";
            }
            if (!result.record.installedPackagePath.empty())
            {
                recordOutput << L"InstalledPackagePath: " << result.record.installedPackagePath.wstring() << L"\n";
            }
            if (!result.record.stagedMsixPath.empty())
            {
                recordOutput << L"StagedMsixPath: " << result.record.stagedMsixPath.wstring() << L"\n";
            }
            recordOutput << L"StubExists: " << (result.record.stubExists ? L"true" : L"false") << L"\n";
            recordOutput << L"StubIsAppExecLink: " << (result.record.stubIsAppExecLink ? L"true" : L"false") << L"\n";
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
                << L"    \"alias\": " << ToJsonString(record.alias) << L",\n"
                << L"    \"packageName\": " << ToJsonString(record.packageName) << L",\n"
                << L"    \"packageFamilyName\": " << ToJsonString(record.packageFamilyName) << L",\n"
                << L"    \"packageFullName\": " << ToJsonString(record.packageFullName) << L",\n"
                << L"    \"target\": " << ToJsonString(record.targetPath.wstring()) << L",\n"
                << L"    \"installedPackagePath\": " << ToJsonString(record.installedPackagePath.wstring()) << L",\n"
                << L"    \"stagedMsixPath\": " << ToJsonString(record.stagedMsixPath.wstring()) << L",\n"
                << L"    \"externalLocation\": " << ToJsonString(record.externalLocation.wstring()) << L",\n"
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
    const std::vector<std::wstring> rawArgs(argv + 1, argv + argc);
    if (rawArgs.empty() || rawArgs[0] == L"--help" || rawArgs[0] == L"-h")
    {
        Usage(rawArgs.empty() ? std::wcerr : std::wcout);
        return rawArgs.empty() ? ExitUsage : ExitSuccess;
    }

    const ParsedArgs args = ParseArgs(rawArgs);
    if (!args.error.empty())
    {
        std::wcerr << args.error << L"\n";
        Usage(std::wcerr);
        return ExitUsage;
    }

    const std::wstring command = args.command;
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
                return ExitSuccess;
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
    catch (const winrt::hresult_error& error)
    {
        std::wcerr << error.message().c_str() << L"\n";
        std::wcerr << L"ErrorCode: 0x" << std::hex << std::uppercase << static_cast<std::uint32_t>(error.code()) << std::dec << L"\n";
        return ExitException;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << "\n";
        return ExitException;
    }

    Usage(std::wcerr);
    return ExitUsage;
}
