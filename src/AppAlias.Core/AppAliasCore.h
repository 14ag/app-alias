#pragma once

#include <Windows.h>

#include <filesystem>
#include <string>
#include <vector>

namespace appalias
{
    struct PackageIdentity
    {
        std::wstring alias;
        std::wstring aliasStem;
        std::wstring packageName;
        std::wstring applicationId;
        std::wstring publisher;
        std::wstring displayName;
        std::wstring publisherDisplayName;
        std::wstring version;
    };

    struct AliasCreateOptions
    {
        std::wstring alias;
        std::filesystem::path targetPath;
        std::wstring displayName;
        std::wstring publisherDisplayName;
        std::wstring packageVersion;
        bool force = false;
    };

    struct AliasRecord
    {
        std::wstring alias;
        std::wstring packageName;
        std::wstring packageFamilyName;
        std::wstring packageFullName;
        std::wstring displayName;
        std::wstring publisherDisplayName;
        std::filesystem::path targetPath;
        std::filesystem::path installedPackagePath;
        std::filesystem::path stagedMsixPath;
        std::filesystem::path externalLocation;
        bool owned = false;
        bool stubExists = false;
        bool stubIsAppExecLink = false;
    };

    struct OperationResult
    {
        bool succeeded = false;
        HRESULT errorCode = S_OK;
        std::wstring message;
        AliasRecord record;
    };

    std::wstring NormalizeAlias(std::wstring_view alias);
    PackageIdentity BuildIdentity(std::wstring_view alias, std::wstring_view displayName, std::wstring_view publisherDisplayName, std::wstring_view packageVersion = {});
    std::wstring GenerateManifest(const PackageIdentity& identity);

    std::filesystem::path GetStateRoot();
    std::filesystem::path GetPackageRoot(const PackageIdentity& identity);
    std::filesystem::path GetPackageMsixPath(const PackageIdentity& identity);
    std::filesystem::path GetWindowsAppsAliasPath(std::wstring_view alias);
    void StageVisualAssets(const std::filesystem::path& packageRoot, const std::filesystem::path& targetPath);

    OperationResult CreateAlias(const AliasCreateOptions& options);
    OperationResult RemoveAliasByAlias(std::wstring_view alias);
    OperationResult RemoveAliasByPackage(std::wstring_view packageNameOrFullName);
    OperationResult VerifyAlias(std::wstring_view alias);
    std::vector<AliasRecord> ListAliases();

    std::string WideToUtf8(std::wstring_view value);
    std::wstring Utf8ToWide(std::string_view value);
    std::wstring QuoteCommandArgument(std::wstring_view value);
    std::filesystem::path GetModulePath();
    std::wstring PathToFileUri(const std::filesystem::path& path);
    std::wstring JsonStringValue(const std::wstring& json, const std::wstring& key);
    std::wstring ToJsonString(std::wstring_view value);
    bool IsAppExecLink(const std::filesystem::path& path);
}
