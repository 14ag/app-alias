#pragma once

#include "AppAliasCore.h"

#include <string_view>

namespace appalias
{
    struct PackageIdentity
    {
        std::wstring alias;
        std::wstring packageName;
        std::wstring applicationId;
        std::wstring publisher;
        std::wstring displayName;
        std::wstring publisherDisplayName;
        std::wstring version;
    };

    std::wstring NormalizeAlias(std::wstring_view alias);
    PackageIdentity BuildIdentity(std::wstring_view alias, std::wstring_view displayName, std::wstring_view publisherDisplayName, std::wstring_view packageVersion = {});
    std::wstring GenerateManifest(const PackageIdentity& identity);

    std::filesystem::path GetStateRoot();
    std::filesystem::path GetPackageRoot(const PackageIdentity& identity);
    std::filesystem::path GetPackageMsixPath(const PackageIdentity& identity);
    std::filesystem::path GetWindowsAppsAliasPath(std::wstring_view alias);
    void StageVisualAssets(const std::filesystem::path& packageRoot, const std::filesystem::path& targetPath);

    std::string WideToUtf8(std::wstring_view value);
    std::wstring Utf8ToWide(std::string_view value);
    std::wstring QuoteCommandArgument(std::wstring_view value);
    std::filesystem::path GetModulePath();
    std::wstring PathToFileUri(const std::filesystem::path& path);
    std::wstring JsonStringValue(const std::wstring& json, const std::wstring& key);
    std::wstring ToJsonString(std::wstring_view value);
    bool IsAppExecLink(const std::filesystem::path& path);
}
