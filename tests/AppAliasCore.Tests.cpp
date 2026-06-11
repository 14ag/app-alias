#include "AppAliasCore.h"

#include <iostream>
#include <array>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace
{
    int failures = 0;

    void AssertTrue(bool condition, const wchar_t* message)
    {
        if (!condition)
        {
            std::wcerr << L"[FAIL] " << message << L"\n";
            ++failures;
        }
    }

    void AssertEqual(const std::wstring& expected, const std::wstring& actual, const wchar_t* message)
    {
        if (expected != actual)
        {
            std::wcerr << L"[FAIL] " << message << L"\n  expected: " << expected << L"\n  actual  : " << actual << L"\n";
            ++failures;
        }
    }

    template <typename Fn>
    void AssertThrows(Fn&& fn, const wchar_t* message)
    {
        try
        {
            fn();
            std::wcerr << L"[FAIL] " << message << L"\n";
            ++failures;
        }
        catch (const std::exception&)
        {
        }
    }

    void TestAliasNormalization()
    {
        AssertEqual(L"code-insiders.exe", appalias::NormalizeAlias(L"code-insiders.exe"), L"valid alias should stay unchanged");
        AssertThrows([] { appalias::NormalizeAlias(L"code-insiders"); }, L"alias without .exe should be rejected");
        AssertThrows([] { appalias::NormalizeAlias(L"..\\code.exe"); }, L"path separators should be rejected");
        AssertThrows([] { appalias::NormalizeAlias(L"bad name.exe"); }, L"spaces should be rejected");
    }

    void TestIdentity()
    {
        const auto first = appalias::BuildIdentity(L"code-insiders.exe", L"Microsoft Visual Studio Code Insiders", L"Microsoft Corporation");
        const auto second = appalias::BuildIdentity(L"code-insiders.exe", L"Microsoft Visual Studio Code Insiders", L"Microsoft Corporation");

        AssertEqual(first.packageName, second.packageName, L"package identity should be deterministic");
        AssertTrue(first.packageName.rfind(L"AppAliasGenerator.codeinsiders.", 0) == 0, L"package identity should include sanitized alias");
        AssertEqual(L"AliasApp", first.applicationId, L"application id should match manifest app id");
        AssertEqual(L"CN=AppAliasGenerator", first.publisher, L"default publisher subject should be fixed");
    }

    void TestManifest()
    {
        const auto identity = appalias::BuildIdentity(L"code-insiders.exe", L"Microsoft Visual Studio Code Insiders", L"Microsoft Corporation");
        const auto manifest = appalias::GenerateManifest(identity);

        AssertTrue(manifest.find(L"<rescap:Capability Name=\"runFullTrust\" />") != std::wstring::npos, L"manifest should declare runFullTrust");
        AssertTrue(manifest.find(L"Category=\"windows.appExecutionAlias\"") != std::wstring::npos, L"manifest should declare app execution alias extension");
        AssertTrue(manifest.find(L"ProcessorArchitecture=\"x64\"") != std::wstring::npos, L"manifest should match the native proxy architecture");
        AssertTrue(manifest.find(L"<uap5:ExecutionAlias Alias=\"code-insiders.exe\" />") != std::wstring::npos, L"manifest should include requested alias");
        AssertTrue(manifest.find(L"EntryPoint=\"Windows.FullTrustApplication\"") != std::wstring::npos, L"full package manifest should declare full trust entry point");
        AssertTrue(manifest.find(L"unvirtualizedResources") == std::wstring::npos, L"proxy package should not request unneeded restricted capabilities");
        AssertTrue(manifest.find(L"uap10:RuntimeBehavior=") == std::wstring::npos, L"full package manifest should not use sparse runtime behavior");
        AssertTrue(manifest.find(L"uap10:AllowExternalContent") == std::wstring::npos, L"full package manifest should not allow external content");
    }

    void TestListAliasesHandlesWindowsAppsStubs()
    {
        try
        {
            const auto records = appalias::ListAliases();
            const auto found = std::find_if(records.begin(), records.end(), [](const appalias::AliasRecord& record) {
                return record.alias == L"winget.exe";
            });
            AssertTrue(found != records.end(), L"list should include DesktopAppInstaller winget alias when installed");
        }
        catch (const std::exception& error)
        {
            std::wcerr << L"[FAIL] list should not throw on WindowsApps stubs: " << error.what() << L"\n";
            ++failures;
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

    void TestStageVisualAssetsExtractsTargetIcon()
    {
        const auto packageRoot = std::filesystem::temp_directory_path() / L"AppAliasCore.IconTest";
        std::error_code ec;
        std::filesystem::remove_all(packageRoot, ec);

        appalias::StageVisualAssets(packageRoot, L"C:\\Windows\\System32\\notepad.exe");

        AssertTrue(HasPngSignature(packageRoot / L"Assets" / L"StoreLogo.png"), L"store logo should be target icon png");
        AssertTrue(HasPngSignature(packageRoot / L"Assets" / L"Square44x44Logo.png"), L"square 44 logo should be target icon png");
        AssertTrue(HasPngSignature(packageRoot / L"Assets" / L"Square150x150Logo.png"), L"square 150 logo should be target icon png");

        std::filesystem::remove_all(packageRoot, ec);
    }
}

int wmain()
{
    TestAliasNormalization();
    TestIdentity();
    TestManifest();
    TestListAliasesHandlesWindowsAppsStubs();
    TestStageVisualAssetsExtractsTargetIcon();

    if (failures != 0)
    {
        std::wcerr << L"[FAIL] " << failures << L" AppAliasCore test failures\n";
        return 1;
    }

    std::wcout << L"[PASS] AppAliasCore tests\n";
    return 0;
}
