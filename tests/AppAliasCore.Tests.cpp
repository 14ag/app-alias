#include "AppAliasInternal.h"

#include <Windows.h>

#include <algorithm>
#include <iostream>
#include <array>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
    struct TestContext
    {
        int failures = 0;
    };

    void AssertTrue(TestContext& context, bool condition, const wchar_t* message)
    {
        if (!condition)
        {
            std::wcerr << L"[FAIL] " << message << L"\n";
            ++context.failures;
        }
    }

    void AssertEqual(TestContext& context, const std::wstring& expected, const std::wstring& actual, const wchar_t* message)
    {
        if (expected != actual)
        {
            std::wcerr << L"[FAIL] " << message << L"\n  expected: " << expected << L"\n  actual  : " << actual << L"\n";
            ++context.failures;
        }
    }

    template <typename Fn>
    void AssertThrows(TestContext& context, Fn&& fn, const wchar_t* message)
    {
        try
        {
            fn();
            std::wcerr << L"[FAIL] " << message << L"\n";
            ++context.failures;
        }
        catch (...)
        {
        }
    }

    void TestAliasNormalization(TestContext& context)
    {
        AssertEqual(context, L"code-insiders.exe", appalias::NormalizeAlias(L"code-insiders.exe"), L"valid alias should stay unchanged");
        AssertThrows(context, [] { appalias::NormalizeAlias(L"code-insiders"); }, L"alias without .exe should be rejected");
        AssertThrows(context, [] { appalias::NormalizeAlias(L"..\\code.exe"); }, L"path separators should be rejected");
        AssertThrows(context, [] { appalias::NormalizeAlias(L"bad name.exe"); }, L"spaces should be rejected");
        AssertThrows(context, [] { appalias::NormalizeAlias(L"bad\u2603.exe"); }, L"unicode alias characters should be rejected");
    }

    void TestIdentity(TestContext& context)
    {
        const auto first = appalias::BuildIdentity(L"code-insiders.exe", L"Microsoft Visual Studio Code Insiders", L"Microsoft Corporation");
        const auto second = appalias::BuildIdentity(L"code-insiders.exe", L"Microsoft Visual Studio Code Insiders", L"Microsoft Corporation");

        AssertEqual(context, first.packageName, second.packageName, L"package identity should be deterministic");
        AssertTrue(context, first.packageName.rfind(L"AppAliasGenerator.codeinsiders.", 0) == 0, L"package identity should include sanitized alias");
        AssertEqual(context, L"AliasApp", first.applicationId, L"application id should match manifest app id");
        AssertEqual(context, L"CN=AppAliasGenerator", first.publisher, L"default publisher subject should be fixed");
        AssertEqual(context, L"2.3.4.5", appalias::BuildIdentity(L"code-insiders.exe", L"", L"", L"2.3.4.5").version, L"explicit package version should be used");
        AssertThrows(context, [] { appalias::BuildIdentity(L"code-insiders.exe", L"", L"", L"1.2.3"); }, L"three-part package version should be rejected");
        AssertThrows(context, [] { appalias::BuildIdentity(L"code-insiders.exe", L"", L"", L"1.2.3.70000"); }, L"out-of-range package version should be rejected");
        AssertThrows(context, [] { appalias::BuildIdentity(L"code-insiders.exe", L"", L"", L"1.02.3.4"); }, L"package version should reject leading zero parts");
    }

    void TestManifest(TestContext& context)
    {
        const auto identity = appalias::BuildIdentity(L"code-insiders.exe", L"Microsoft Visual Studio Code Insiders", L"Microsoft Corporation");
        const auto manifest = appalias::GenerateManifest(identity);

        AssertTrue(context, manifest.find(L"<rescap:Capability Name=\"runFullTrust\" />") != std::wstring::npos, L"manifest should declare runFullTrust");
        AssertTrue(context, manifest.find(L"Category=\"windows.appExecutionAlias\"") != std::wstring::npos, L"manifest should declare app execution alias extension");
        AssertTrue(context, manifest.find(L"ProcessorArchitecture=\"x64\"") != std::wstring::npos, L"manifest should match the native proxy architecture");
        AssertTrue(context, manifest.find(L"<uap5:ExecutionAlias Alias=\"code-insiders.exe\" />") != std::wstring::npos, L"manifest should include requested alias");
        AssertTrue(context, manifest.find(L"EntryPoint=\"Windows.FullTrustApplication\"") != std::wstring::npos, L"full package manifest should declare full trust entry point");
        AssertTrue(context, manifest.find(L"unvirtualizedResources") == std::wstring::npos, L"proxy package should not request unneeded restricted capabilities");
        AssertTrue(context, manifest.find(L"uap10:RuntimeBehavior=") == std::wstring::npos, L"full package manifest should not use sparse runtime behavior");
        AssertTrue(context, manifest.find(L"uap10:AllowExternalContent") == std::wstring::npos, L"full package manifest should not allow external content");
    }

    bool ExistsByAttributes(const std::filesystem::path& path)
    {
        return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
    }

    void TestListAliasesHandlesWindowsAppsStubs(TestContext& context)
    {
        try
        {
            const auto records = appalias::ListAliases();
            if (!ExistsByAttributes(appalias::GetWindowsAppsAliasPath(L"winget.exe")))
            {
                std::wcout << L"[SKIP] winget.exe alias not installed\n";
                return;
            }

            const auto found = std::find_if(records.begin(), records.end(), [](const appalias::AliasRecord& record) {
                return record.alias == L"winget.exe";
            });
            AssertTrue(context, found != records.end(), L"list should include DesktopAppInstaller winget alias when installed");
        }
        catch (const std::exception& error)
        {
            std::wcerr << L"[FAIL] list should not throw on WindowsApps stubs: " << error.what() << L"\n";
            ++context.failures;
        }
    }

    bool HasPngSignature(const std::filesystem::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file)
        {
            return false;
        }

        std::array<unsigned char, 8> signature{};
        file.read(reinterpret_cast<char*>(signature.data()), static_cast<std::streamsize>(signature.size()));
        return signature == std::array<unsigned char, 8>{ 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A };
    }

    void TestStageVisualAssetsExtractsTargetIcon(TestContext& context)
    {
        const auto packageRoot = std::filesystem::temp_directory_path() / L"AppAliasCore.IconTest";
        std::error_code ec;
        std::filesystem::remove_all(packageRoot, ec);

        wchar_t systemDirectory[MAX_PATH]{};
        if (GetSystemDirectoryW(systemDirectory, MAX_PATH) == 0)
        {
            std::wcout << L"[SKIP] system directory not found\n";
            return;
        }

        const auto notepad = std::filesystem::path(systemDirectory) / L"notepad.exe";
        if (!std::filesystem::exists(notepad))
        {
            std::wcout << L"[SKIP] notepad.exe not installed\n";
            return;
        }

        appalias::StageVisualAssets(packageRoot, notepad);

        const std::vector<std::filesystem::path> expectedAssets{
            L"StoreLogo.png",
            L"Square44x44Logo.png",
            L"Square150x150Logo.png",
            L"Square44x44Logo.targetsize-16.png",
            L"Square44x44Logo.targetsize-24.png",
            L"Square44x44Logo.targetsize-32.png",
            L"Square44x44Logo.targetsize-48.png",
            L"Square44x44Logo.targetsize-256.png"
        };
        for (const auto& asset : expectedAssets)
        {
            AssertTrue(context, HasPngSignature(packageRoot / L"Assets" / asset), L"visual asset should be target icon png");
        }

        std::filesystem::remove_all(packageRoot, ec);
    }

    void TestJsonStringParsing(TestContext& context)
    {
        const std::wstring json = LR"({"target":"C:\\Users\\Name\n\r\t\"tool\"\u0041","other":"value"})";
        AssertEqual(context, L"C:\\Users\\Name\n\r\t\"tool\"A", appalias::JsonStringValue(json, L"target"), L"json parser should unescape target");

        const std::wstring roundTrip = L"{\"target\":" + appalias::ToJsonString(L"C:\\Path\\\"quoted\"\n") + L"}";
        AssertEqual(context, L"C:\\Path\\\"quoted\"\n", appalias::JsonStringValue(roundTrip, L"target"), L"json parser should read ToJsonString output");
        AssertEqual(context, L"real", appalias::JsonStringValue(L"{\"not\":\"\\\"target\\\"\",\"target\":\"real\"}", L"target"), L"json parser should ignore key text in other values");
        AssertEqual(context, L"\b\f", appalias::JsonStringValue(L"{\"target\":\"\\b\\f\"}", L"target"), L"json parser should support b and f escapes");
        AssertThrows(context, [] { appalias::JsonStringValue(L"{\"target\":\"\\uD800\"}", L"target"); }, L"json parser should reject missing low surrogate");
        AssertThrows(context, [] { appalias::JsonStringValue(L"{\"target\":\"\\q\"}", L"target"); }, L"json parser should reject invalid escape");
        AssertTrue(context, appalias::ToJsonString(L"\b\f\x01").find(L"\\b\\f\\u0001") != std::wstring::npos, L"json writer should escape all control chars");
    }

    void TestPathToFileUriEncoding(TestContext& context)
    {
        const auto uri = appalias::PathToFileUri(L"C:\\tools\\caf\u00E9 #100%?.msix");
        AssertEqual(context, L"file:///C:/tools/caf%C3%A9%20%23100%25%3F.msix", uri, L"file uri should percent-encode special characters");
        AssertEqual(context, L"file://server/share/file.msix", appalias::PathToFileUri(L"\\\\server\\share\\file.msix"), L"file uri should preserve UNC authority");
        AssertThrows(context, [] { appalias::PathToFileUri(L"relative\\file.msix"); }, L"relative file uri input should be rejected");
    }

    void TestCommandQuoting(TestContext& context)
    {
        AssertEqual(context, L"\"C:\\path\\\\\"", appalias::QuoteCommandArgument(L"C:\\path\\"), L"command quoting should escape trailing backslashes");
        AssertEqual(context, L"\"quote\\\"path\"", appalias::QuoteCommandArgument(L"quote\"path"), L"command quoting should escape embedded quote");
    }
}

int wmain()
{
    TestContext context{};
    TestAliasNormalization(context);
    TestIdentity(context);
    TestManifest(context);
    TestJsonStringParsing(context);
    TestPathToFileUriEncoding(context);
    TestCommandQuoting(context);
    TestListAliasesHandlesWindowsAppsStubs(context);
    TestStageVisualAssetsExtractsTargetIcon(context);

    if (context.failures != 0)
    {
        std::wcerr << L"[FAIL] " << context.failures << L" AppAliasCore test failures\n";
        return 1;
    }

    std::wcout << L"[PASS] AppAliasCore tests\n";
    return 0;
}
