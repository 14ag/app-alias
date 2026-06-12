#pragma once

#include <winerror.h>

#include <filesystem>
#include <string>
#include <vector>

namespace appalias
{
    struct AliasCreateOptions
    {
        std::wstring alias;
        std::wstring targetPath;
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

    enum class OperationErrorKind
    {
        None,
        Failed,
        NotFound,
        ForeignAlias,
        StubInvalid,
        Timeout
    };

    struct OperationResult
    {
        bool succeeded = false;
        OperationErrorKind errorKind = OperationErrorKind::None;
        HRESULT errorCode = S_OK;
        std::wstring message;
        AliasRecord record;
    };

    OperationResult CreateAlias(const AliasCreateOptions& options);
    OperationResult RemoveAliasByAlias(std::wstring_view alias);
    OperationResult RemoveAliasByPackage(std::wstring_view packageNameOrFullName);
    OperationResult VerifyAlias(std::wstring_view alias);
    std::vector<AliasRecord> ListAliases();
}
