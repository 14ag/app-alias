#include "AppAliasCore.h"

#include <algorithm>
#include <Windows.h>

namespace Gdiplus
{
    using std::max;
    using std::min;
}

#pragma warning(push)
#pragma warning(disable: 4458)
#include <gdiplus.h>
#pragma warning(pop)
#include <shellapi.h>
#include <winioctl.h>

#include <winrt/base.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Management.Deployment.h>
#include <winrt/Windows.Storage.h>

#include <array>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace fs = std::filesystem;

namespace
{
    constexpr wchar_t PackagePrefix[] = L"AppAliasGenerator.";
    constexpr wchar_t DefaultPublisher[] = L"CN=AppAliasGenerator";
    constexpr wchar_t DefaultApplicationId[] = L"AliasApp";
    constexpr wchar_t DefaultVersion[] = L"1.0.0.0";
    constexpr DWORD AppExecLinkTag = 0x8000001B;

    std::wstring Lower(std::wstring value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) { return static_cast<wchar_t>(towlower(ch)); });
        return value;
    }

    bool StartsWith(std::wstring_view value, std::wstring_view prefix)
    {
        return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
    }

    std::string WideToUtf8(std::wstring_view value)
    {
        if (value.empty())
        {
            return {};
        }

        const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
        if (size <= 0)
        {
            throw std::runtime_error("WideCharToMultiByte failed");
        }

        std::string result(static_cast<size_t>(size), '\0');
        WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size, nullptr, nullptr);
        return result;
    }

    std::wstring Utf8ToWide(std::string_view value)
    {
        if (value.empty())
        {
            return {};
        }

        const int size = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
        if (size <= 0)
        {
            throw std::runtime_error("MultiByteToWideChar failed");
        }

        std::wstring result(static_cast<size_t>(size), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size);
        return result;
    }

    std::wstring XmlEscape(std::wstring_view value)
    {
        std::wstring result;
        for (const wchar_t ch : value)
        {
            switch (ch)
            {
            case L'&': result += L"&amp;"; break;
            case L'<': result += L"&lt;"; break;
            case L'>': result += L"&gt;"; break;
            case L'"': result += L"&quot;"; break;
            case L'\'': result += L"&apos;"; break;
            default: result.push_back(ch); break;
            }
        }
        return result;
    }

    std::wstring StripExe(std::wstring alias)
    {
        constexpr std::wstring_view exe = L".exe";
        if (alias.size() >= exe.size())
        {
            alias.resize(alias.size() - exe.size());
        }
        return alias;
    }

    uint32_t Fnv1a32(std::wstring_view value)
    {
        uint32_t hash = 2166136261u;
        for (const wchar_t ch : value)
        {
            hash ^= static_cast<uint16_t>(towlower(ch));
            hash *= 16777619u;
        }
        return hash;
    }

    std::wstring Hash8(std::wstring_view value)
    {
        std::wstringstream stream;
        stream << std::uppercase << std::hex << std::setw(8) << std::setfill(L'0') << Fnv1a32(value);
        return stream.str();
    }

    std::wstring SanitizeIdentityPart(std::wstring_view alias)
    {
        std::wstring result;
        for (const wchar_t ch : alias)
        {
            if ((ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z') || (ch >= L'0' && ch <= L'9'))
            {
                result.push_back(static_cast<wchar_t>(towlower(ch)));
            }
        }

        if (result.empty())
        {
            result = L"alias";
        }

        return result;
    }

    std::wstring GetEnvPath(const wchar_t* name)
    {
        std::array<wchar_t, MAX_PATH * 4> buffer{};
        const DWORD size = GetEnvironmentVariableW(name, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (size == 0 || size >= buffer.size())
        {
            return {};
        }
        return { buffer.data(), size };
    }

    void EnsureDirectory(const fs::path& path)
    {
        std::error_code ec;
        fs::create_directories(path, ec);
        if (ec)
        {
            throw std::runtime_error("failed to create directory");
        }
    }

    bool ExistsNoThrow(const fs::path& path)
    {
        std::error_code ec;
        const bool exists = fs::exists(path, ec);
        if (!ec && exists)
        {
            return true;
        }

        const DWORD attributes = GetFileAttributesW(path.c_str());
        return attributes != INVALID_FILE_ATTRIBUTES;
    }

    void WriteUtf8Text(const fs::path& path, std::wstring_view text)
    {
        EnsureDirectory(path.parent_path());
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file)
        {
            throw std::runtime_error("failed to open output file");
        }

        const auto bytes = WideToUtf8(text);
        file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }

    std::wstring ReadUtf8Text(const fs::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file)
        {
            return {};
        }

        std::stringstream buffer;
        buffer << file.rdbuf();
        return Utf8ToWide(buffer.str());
    }

    fs::path GetModulePath()
    {
        std::wstring buffer(MAX_PATH, L'\0');
        DWORD length = 0;
        for (;;)
        {
            length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
            if (length == 0)
            {
                throw std::runtime_error("GetModuleFileNameW failed");
            }

            if (length < buffer.size() - 1)
            {
                buffer.resize(length);
                return fs::path(buffer);
            }

            buffer.resize(buffer.size() * 2);
        }
    }

    std::wstring Quote(std::wstring_view value)
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

    int RunProcess(const fs::path& exe, const std::wstring& args)
    {
        std::wstring command = Quote(exe.wstring());
        if (!args.empty())
        {
            command.push_back(L' ');
            command += args;
        }

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        std::vector<wchar_t> mutableCommand(command.begin(), command.end());
        mutableCommand.push_back(L'\0');

        if (!CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &startup, &process))
        {
            return static_cast<int>(GetLastError());
        }

        WaitForSingleObject(process.hProcess, INFINITE);
        DWORD exitCode = 1;
        GetExitCodeProcess(process.hProcess, &exitCode);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return static_cast<int>(exitCode);
    }

    fs::path SearchPathTool(const std::wstring& tool)
    {
        std::array<wchar_t, MAX_PATH * 4> buffer{};
        const DWORD size = SearchPathW(nullptr, tool.c_str(), nullptr, static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
        if (size > 0 && size < buffer.size())
        {
            return fs::path(buffer.data());
        }
        return {};
    }

    fs::path FindSdkTool(const std::wstring& tool)
    {
        const auto fromPath = SearchPathTool(tool);
        if (!fromPath.empty())
        {
            return fromPath;
        }

        const std::wstring programFilesX86 = GetEnvPath(L"ProgramFiles(x86)");
        if (programFilesX86.empty())
        {
            return {};
        }

        const fs::path binRoot = fs::path(programFilesX86) / L"Windows Kits" / L"10" / L"bin";
        if (!fs::exists(binRoot))
        {
            return {};
        }

        std::vector<fs::path> candidates;
        for (const auto& entry : fs::recursive_directory_iterator(binRoot))
        {
            if (entry.is_regular_file() && Lower(entry.path().filename().wstring()) == Lower(tool))
            {
                const auto parent = Lower(entry.path().parent_path().filename().wstring());
                if (parent == L"x64")
                {
                    candidates.push_back(entry.path());
                }
            }
        }

        std::sort(candidates.begin(), candidates.end(), std::greater<fs::path>());
        if (!candidates.empty())
        {
            return candidates.front();
        }
        return {};
    }

    fs::path DefaultPfxPath()
    {
        return appalias::GetStateRoot() / L"Cert" / L"AppAliasGenerator.pfx";
    }

    std::wstring DefaultPfxPassword()
    {
        const std::wstring password = GetEnvPath(L"APPALIAS_PFX_PASSWORD");
        return password.empty() ? L"AppAliasGenerator" : password;
    }

    std::wstring ConfiguredPublisherSubject()
    {
        const std::wstring publisher = GetEnvPath(L"APPALIAS_PUBLISHER_SUBJECT");
        return publisher.empty() ? DefaultPublisher : publisher;
    }

    fs::path ConfiguredPfxPath()
    {
        const std::wstring configured = GetEnvPath(L"APPALIAS_PFX");
        if (!configured.empty())
        {
            return fs::path(configured);
        }
        return DefaultPfxPath();
    }

    std::wstring PathToFileUri(const fs::path& path)
    {
        std::wstring value = fs::absolute(path).wstring();
        std::replace(value.begin(), value.end(), L'\\', L'/');
        std::wstring uri = L"file:///";
        for (const wchar_t ch : value)
        {
            if (ch == L' ')
            {
                uri += L"%20";
            }
            else if (ch == L'#')
            {
                uri += L"%23";
            }
            else
            {
                uri.push_back(ch);
            }
        }
        return uri;
    }

    std::wstring BuildAliasConfig(const appalias::PackageIdentity& identity, const fs::path& target)
    {
        return L"{\n"
            L"  \"alias\": " + appalias::ToJsonString(identity.alias) + L",\n"
            L"  \"target\": " + appalias::ToJsonString(fs::absolute(target).wstring()) + L",\n"
            L"  \"packageName\": " + appalias::ToJsonString(identity.packageName) + L",\n"
            L"  \"applicationId\": " + appalias::ToJsonString(identity.applicationId) + L"\n"
            L"}\n";
    }

    void CopyProxy(const fs::path& destinationRoot)
    {
        const fs::path current = GetModulePath();
        const fs::path proxySource = current.parent_path() / L"AppAlias.Proxy.exe";
        if (!fs::exists(proxySource))
        {
            throw std::runtime_error("AppAlias.Proxy.exe not found next to CLI");
        }

        EnsureDirectory(destinationRoot);
        fs::copy_file(proxySource, destinationRoot / L"AppAlias.Proxy.exe", fs::copy_options::overwrite_existing);
    }

    CLSID GetPngEncoderClsid()
    {
        UINT count = 0;
        UINT bytes = 0;
        if (Gdiplus::GetImageEncodersSize(&count, &bytes) != Gdiplus::Ok || bytes == 0)
        {
            throw std::runtime_error("failed to enumerate image encoders");
        }

        std::vector<unsigned char> buffer(bytes);
        auto* encoders = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buffer.data());
        if (Gdiplus::GetImageEncoders(count, bytes, encoders) != Gdiplus::Ok)
        {
            throw std::runtime_error("failed to read image encoders");
        }

        for (UINT index = 0; index < count; ++index)
        {
            if (wcscmp(encoders[index].MimeType, L"image/png") == 0)
            {
                return encoders[index].Clsid;
            }
        }

        throw std::runtime_error("png encoder not found");
    }

    void SaveIconAsPng(HICON icon, const fs::path& path, int size)
    {
        EnsureDirectory(path.parent_path());

        const CLSID pngClsid = GetPngEncoderClsid();
        Gdiplus::Bitmap source(icon);
        Gdiplus::Bitmap canvas(size, size, PixelFormat32bppARGB);
        Gdiplus::Graphics graphics(&canvas);
        graphics.Clear(Gdiplus::Color(0, 0, 0, 0));
        graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        graphics.DrawImage(&source, 0, 0, size, size);

        if (canvas.Save(path.c_str(), &pngClsid, nullptr) != Gdiplus::Ok)
        {
            throw std::runtime_error("failed to save icon asset");
        }
    }

    HICON GetTargetIcon(const fs::path& target)
    {
        SHFILEINFOW info{};
        const DWORD_PTR result = SHGetFileInfoW(
            target.c_str(),
            FILE_ATTRIBUTE_NORMAL,
            &info,
            sizeof(info),
            SHGFI_ICON | SHGFI_LARGEICON);

        if (result == 0 || info.hIcon == nullptr)
        {
            throw std::runtime_error("failed to extract target icon");
        }

        return info.hIcon;
    }

    void PackMsix(const appalias::PackageIdentity& identity)
    {
        const fs::path packageRoot = appalias::GetPackageRoot(identity);
        const fs::path packagePath = appalias::GetPackageMsixPath(identity);
        const fs::path makeappx = FindSdkTool(L"makeappx.exe");
        if (makeappx.empty())
        {
            throw std::runtime_error("makeappx.exe not found");
        }

        std::error_code ec;
        fs::remove(packagePath, ec);

        const std::wstring args = L"pack /o /d " + Quote(packageRoot.wstring()) + L" /nv /p " + Quote(packagePath.wstring());
        const int exitCode = RunProcess(makeappx, args);
        if (exitCode != 0)
        {
            throw std::runtime_error("makeappx.exe failed");
        }
    }

    void SignMsix(const appalias::PackageIdentity& identity)
    {
        const fs::path signtool = FindSdkTool(L"signtool.exe");
        if (signtool.empty())
        {
            throw std::runtime_error("signtool.exe not found");
        }

        const std::wstring certSha1 = GetEnvPath(L"APPALIAS_CERT_SHA1");
        const std::wstring certStore = Lower(GetEnvPath(L"APPALIAS_CERT_STORE"));
        std::wstring args = L"sign /fd SHA256 ";
        if (!certSha1.empty())
        {
            args += L"/s My /sha1 " + Quote(certSha1) + L" ";
            if (certStore == L"localmachine" || certStore == L"machine")
            {
                args += L"/sm ";
            }
        }
        else
        {
            const fs::path pfx = ConfiguredPfxPath();
            if (!fs::exists(pfx))
            {
                throw std::runtime_error("signing certificate not found; run scripts\\New-AppAliasCert.ps1");
            }
            args += L"/f " + Quote(pfx.wstring()) + L" /p " + Quote(DefaultPfxPassword()) + L" ";
        }

        args += Quote(appalias::GetPackageMsixPath(identity).wstring());
        const int exitCode = RunProcess(signtool, args);
        if (exitCode != 0)
        {
            throw std::runtime_error("signtool.exe failed");
        }
    }

    appalias::OperationResult DeployPackage(const appalias::PackageIdentity& identity)
    {
        appalias::OperationResult result{};
        try
        {
            winrt::init_apartment();

            winrt::Windows::Management::Deployment::PackageManager packageManager;
            winrt::Windows::Management::Deployment::AddPackageOptions options;

            const auto deployment = packageManager.AddPackageByUriAsync(
                winrt::Windows::Foundation::Uri(PathToFileUri(appalias::GetPackageMsixPath(identity))),
                options).get();

            const HRESULT hr = deployment.ExtendedErrorCode();
            if (FAILED(hr))
            {
                result.succeeded = false;
                result.errorCode = hr;
                result.message = deployment.ErrorText().c_str();
                return result;
            }

            result.succeeded = true;
            result.message = L"alias package registered";
        }
        catch (const winrt::hresult_error& error)
        {
            result.succeeded = false;
            result.errorCode = error.code();
            result.message = error.message().c_str();
        }
        catch (const std::exception& error)
        {
            result.succeeded = false;
            result.message = Utf8ToWide(error.what());
        }
        return result;
    }

    std::vector<std::wstring> FindExecutionAliasesInManifest(const fs::path& manifestPath)
    {
        const std::wstring manifest = ReadUtf8Text(manifestPath);
        std::vector<std::wstring> aliases;
        const std::wregex pattern(LR"alias(<[^>]*ExecutionAlias[^>]*\sAlias\s*=\s*"([^"]+\.exe)")alias", std::regex_constants::icase);

        for (std::wsregex_iterator it(manifest.begin(), manifest.end(), pattern), end; it != end; ++it)
        {
            aliases.push_back((*it)[1].str());
        }
        return aliases;
    }

    fs::path FindConfigForPackage(const std::wstring& packageName)
    {
        return appalias::GetStateRoot() / L"External" / packageName / L"alias.json";
    }

    std::wstring JsonValue(const std::wstring& json, const std::wstring& key)
    {
        const std::wregex pattern(L"\"" + key + LR"json("\s*:\s*"([^"]*)")json");
        std::wsmatch match;
        if (std::regex_search(json, match, pattern))
        {
            std::wstring value = match[1].str();
            std::wstring unescaped;
            unescaped.reserve(value.size());
            for (size_t index = 0; index < value.size(); ++index)
            {
                if (value[index] == L'\\' && index + 1 < value.size())
                {
                    ++index;
                    unescaped.push_back(value[index]);
                }
                else
                {
                    unescaped.push_back(value[index]);
                }
            }
            return unescaped;
        }
        return {};
    }

    appalias::AliasRecord RecordFromPackage(const winrt::Windows::ApplicationModel::Package& package, const std::wstring& alias)
    {
        appalias::AliasRecord record{};
        record.alias = alias;
        record.packageName = package.Id().Name().c_str();
        record.packageFamilyName = package.Id().FamilyName().c_str();
        record.packageFullName = package.Id().FullName().c_str();
        record.owned = StartsWith(record.packageName, PackagePrefix);
        record.packagePath = fs::path(package.InstalledLocation().Path().c_str());

        const fs::path configPath = FindConfigForPackage(record.packageName);
        if (fs::exists(configPath))
        {
            const auto config = ReadUtf8Text(configPath);
            record.targetPath = JsonValue(config, L"target");
            record.externalLocation = configPath.parent_path();
            record.owned = true;
        }

        const fs::path stub = appalias::GetWindowsAppsAliasPath(alias);
        record.stubExists = ExistsNoThrow(stub);
        record.stubIsAppExecLink = record.stubExists && appalias::IsAppExecLink(stub);
        return record;
    }
}

namespace appalias
{
    std::wstring NormalizeAlias(std::wstring_view alias)
    {
        if (alias.empty())
        {
            throw std::invalid_argument("alias is required");
        }

        std::wstring value(alias);
        value = Lower(value);

        if (value.size() < 5 || value.substr(value.size() - 4) != L".exe")
        {
            throw std::invalid_argument("alias must end with .exe");
        }

        for (const wchar_t ch : value)
        {
            const bool allowed =
                (ch >= L'a' && ch <= L'z') ||
                (ch >= L'0' && ch <= L'9') ||
                ch == L'.' ||
                ch == L'-' ||
                ch == L'_';

            if (!allowed)
            {
                throw std::invalid_argument("alias contains invalid characters");
            }
        }

        if (value.find(L"..") != std::wstring::npos || value.front() == L'.')
        {
            throw std::invalid_argument("alias contains invalid dot sequence");
        }

        return value;
    }

    PackageIdentity BuildIdentity(std::wstring_view alias, std::wstring_view displayName, std::wstring_view publisherDisplayName)
    {
        PackageIdentity identity{};
        identity.alias = NormalizeAlias(alias);
        identity.aliasStem = StripExe(identity.alias);
        identity.packageName = std::wstring(PackagePrefix) + SanitizeIdentityPart(identity.aliasStem) + L"." + Hash8(identity.alias);
        identity.applicationId = DefaultApplicationId;
        identity.publisher = ConfiguredPublisherSubject();
        identity.displayName = displayName.empty() ? identity.aliasStem : std::wstring(displayName);
        identity.publisherDisplayName = publisherDisplayName.empty() ? L"AppAliasGenerator" : std::wstring(publisherDisplayName);
        identity.version = DefaultVersion;
        return identity;
    }

    std::wstring GenerateManifest(const PackageIdentity& identity)
    {
        const std::wstring displayName = XmlEscape(identity.displayName);
        const std::wstring publisherDisplayName = XmlEscape(identity.publisherDisplayName);
        const std::wstring packageName = XmlEscape(identity.packageName);
        const std::wstring alias = XmlEscape(identity.alias);

        std::wstringstream manifest;
        manifest << LR"(<?xml version="1.0" encoding="utf-8"?>)" << L"\n"
            << LR"(<Package IgnorableNamespaces="uap uap5 rescap" xmlns="http://schemas.microsoft.com/appx/manifest/foundation/windows10" xmlns:uap="http://schemas.microsoft.com/appx/manifest/uap/windows10" xmlns:uap5="http://schemas.microsoft.com/appx/manifest/uap/windows10/5" xmlns:rescap="http://schemas.microsoft.com/appx/manifest/foundation/windows10/restrictedcapabilities">)" << L"\n"
            << L"  <Identity Name=\"" << packageName << L"\" Publisher=\"" << identity.publisher << L"\" Version=\"" << identity.version << LR"(" ProcessorArchitecture="x64" />)" << L"\n"
            << L"  <Properties>\n"
            << L"    <DisplayName>" << displayName << L"</DisplayName>\n"
            << L"    <PublisherDisplayName>" << publisherDisplayName << L"</PublisherDisplayName>\n"
            << L"    <Logo>Assets\\StoreLogo.png</Logo>\n"
            << L"  </Properties>\n"
            << L"  <Resources>\n"
            << L"    <Resource Language=\"en-us\" />\n"
            << L"  </Resources>\n"
            << L"  <Dependencies>\n"
            << L"    <TargetDeviceFamily Name=\"Windows.Desktop\" MinVersion=\"10.0.19041.0\" MaxVersionTested=\"10.0.26100.0\" />\n"
            << L"  </Dependencies>\n"
            << L"  <Applications>\n"
            << L"    <Application Id=\"" << identity.applicationId << L"\" Executable=\"AppAlias.Proxy.exe\" EntryPoint=\"Windows.FullTrustApplication\">\n"
            << L"      <uap:VisualElements AppListEntry=\"none\" DisplayName=\"" << displayName << L"\" Description=\"" << displayName << L"\" BackgroundColor=\"transparent\" Square150x150Logo=\"Assets\\Square150x150Logo.png\" Square44x44Logo=\"Assets\\Square44x44Logo.png\" />\n"
            << L"      <Extensions>\n"
            << L"        <uap5:Extension Category=\"windows.appExecutionAlias\">\n"
            << L"          <uap5:AppExecutionAlias>\n"
            << L"            <uap5:ExecutionAlias Alias=\"" << alias << L"\" />\n"
            << L"          </uap5:AppExecutionAlias>\n"
            << L"        </uap5:Extension>\n"
            << L"      </Extensions>\n"
            << L"    </Application>\n"
            << L"  </Applications>\n"
            << L"  <Capabilities>\n"
            << L"    <rescap:Capability Name=\"runFullTrust\" />\n"
            << L"  </Capabilities>\n"
            << L"</Package>\n";
        return manifest.str();
    }

    fs::path GetStateRoot()
    {
        const std::wstring localAppData = GetEnvPath(L"LOCALAPPDATA");
        if (localAppData.empty())
        {
            throw std::runtime_error("LOCALAPPDATA not set");
        }
        return fs::path(localAppData) / L"AppAliasGenerator";
    }

    fs::path GetPackageRoot(const PackageIdentity& identity)
    {
        return GetStateRoot() / L"Packages" / identity.packageName;
    }

    fs::path GetExternalRoot(const PackageIdentity& identity)
    {
        return GetStateRoot() / L"External" / identity.packageName;
    }

    fs::path GetPackageMsixPath(const PackageIdentity& identity)
    {
        return GetStateRoot() / L"Msix" / (identity.packageName + L".msix");
    }

    fs::path GetWindowsAppsAliasPath(std::wstring_view alias)
    {
        const std::wstring localAppData = GetEnvPath(L"LOCALAPPDATA");
        if (localAppData.empty())
        {
            return {};
        }
        return fs::path(localAppData) / L"Microsoft" / L"WindowsApps" / std::wstring(alias);
    }

    void StageVisualAssets(const fs::path& packageRoot, const fs::path& targetPath)
    {
        if (!ExistsNoThrow(targetPath))
        {
            throw std::runtime_error("target path not found");
        }

        Gdiplus::GdiplusStartupInput gdiplusInput{};
        ULONG_PTR gdiplusToken = 0;
        if (Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusInput, nullptr) != Gdiplus::Ok)
        {
            throw std::runtime_error("failed to initialize gdiplus");
        }

        HICON icon = nullptr;
        try
        {
            icon = GetTargetIcon(targetPath);
            const fs::path assets = packageRoot / L"Assets";
            SaveIconAsPng(icon, assets / L"StoreLogo.png", 50);
            SaveIconAsPng(icon, assets / L"Square44x44Logo.png", 44);
            SaveIconAsPng(icon, assets / L"Square150x150Logo.png", 150);
            SaveIconAsPng(icon, assets / L"Square44x44Logo.targetsize-16.png", 16);
            SaveIconAsPng(icon, assets / L"Square44x44Logo.targetsize-24.png", 24);
            SaveIconAsPng(icon, assets / L"Square44x44Logo.targetsize-32.png", 32);
            SaveIconAsPng(icon, assets / L"Square44x44Logo.targetsize-48.png", 48);
            SaveIconAsPng(icon, assets / L"Square44x44Logo.targetsize-256.png", 256);
        }
        catch (...)
        {
            if (icon)
            {
                DestroyIcon(icon);
            }
            Gdiplus::GdiplusShutdown(gdiplusToken);
            throw;
        }

        if (icon)
        {
            DestroyIcon(icon);
        }
        Gdiplus::GdiplusShutdown(gdiplusToken);
    }

    OperationResult CreateAlias(const AliasCreateOptions& options)
    {
        OperationResult result{};
        try
        {
            const PackageIdentity identity = BuildIdentity(options.alias, options.displayName, options.publisherDisplayName);
            const fs::path target = fs::absolute(options.targetPath);
            if (!fs::exists(target))
            {
                throw std::runtime_error("target path not found");
            }

            if (!options.force)
            {
                const auto existing = VerifyAlias(identity.alias);
                if (existing.record.stubExists)
                {
                    throw std::runtime_error("alias already exists; pass --force to replace owned alias");
                }
            }
            else
            {
                const auto existing = VerifyAlias(identity.alias);
                if (existing.record.owned)
                {
                    RemoveAliasByAlias(identity.alias);
                }
            }

            const fs::path packageRoot = GetPackageRoot(identity);
            const fs::path externalRoot = GetExternalRoot(identity);
            EnsureDirectory(packageRoot);
            EnsureDirectory(externalRoot);
            EnsureDirectory(GetPackageMsixPath(identity).parent_path());

            CopyProxy(packageRoot);
            StageVisualAssets(packageRoot, target);
            WriteUtf8Text(packageRoot / L"alias.json", BuildAliasConfig(identity, target));
            WriteUtf8Text(externalRoot / L"alias.json", BuildAliasConfig(identity, target));
            WriteUtf8Text(packageRoot / L"AppxManifest.xml", GenerateManifest(identity));
            PackMsix(identity);
            SignMsix(identity);

            result = DeployPackage(identity);
            result.record.alias = identity.alias;
            result.record.packageName = identity.packageName;
            result.record.displayName = identity.displayName;
            result.record.publisherDisplayName = identity.publisherDisplayName;
            result.record.targetPath = target;
            result.record.packagePath = GetPackageMsixPath(identity);
            result.record.externalLocation = externalRoot;
            result.record.owned = true;
            result.record.stubExists = ExistsNoThrow(GetWindowsAppsAliasPath(identity.alias));
            result.record.stubIsAppExecLink = result.record.stubExists && IsAppExecLink(GetWindowsAppsAliasPath(identity.alias));
        }
        catch (const std::exception& error)
        {
            result.succeeded = false;
            result.message = Utf8ToWide(error.what());
        }
        return result;
    }

    std::vector<AliasRecord> ListAliases()
    {
        std::vector<AliasRecord> records;
        winrt::init_apartment();
        winrt::Windows::Management::Deployment::PackageManager packageManager;

        for (const auto& package : packageManager.FindPackagesForUser(L""))
        {
            fs::path location;
            try
            {
                location = fs::path(package.InstalledLocation().Path().c_str());
            }
            catch (...)
            {
                continue;
            }

            const auto aliases = FindExecutionAliasesInManifest(location / L"AppxManifest.xml");
            for (const auto& alias : aliases)
            {
                records.push_back(RecordFromPackage(package, alias));
            }
        }

        std::sort(records.begin(), records.end(), [](const AliasRecord& left, const AliasRecord& right) {
            return Lower(left.alias) < Lower(right.alias);
        });
        return records;
    }

    OperationResult RemoveAliasByAlias(std::wstring_view alias)
    {
        const std::wstring normalized = NormalizeAlias(alias);
        for (const auto& record : ListAliases())
        {
            if (Lower(record.alias) == normalized)
            {
                return RemoveAliasByPackage(record.packageFullName);
            }
        }

        OperationResult result{};
        result.succeeded = false;
        result.message = L"alias not found";
        return result;
    }

    OperationResult RemoveAliasByPackage(std::wstring_view packageNameOrFullName)
    {
        OperationResult result{};
        try
        {
            winrt::init_apartment();
            winrt::Windows::Management::Deployment::PackageManager packageManager;

            for (const auto& package : packageManager.FindPackagesForUser(L""))
            {
                const std::wstring name = package.Id().Name().c_str();
                const std::wstring fullName = package.Id().FullName().c_str();
                if (name != packageNameOrFullName && fullName != packageNameOrFullName)
                {
                    continue;
                }

                if (!StartsWith(name, PackagePrefix))
                {
                    result.succeeded = false;
                    result.message = L"refusing to remove foreign package alias";
                    return result;
                }

                const auto deployment = packageManager.RemovePackageAsync(fullName).get();
                const HRESULT hr = deployment.ExtendedErrorCode();
                if (FAILED(hr))
                {
                    result.succeeded = false;
                    result.errorCode = hr;
                    result.message = deployment.ErrorText().c_str();
                    return result;
                }

                std::error_code ec;
                fs::remove_all(GetStateRoot() / L"External" / name, ec);
                fs::remove_all(GetStateRoot() / L"Packages" / name, ec);
                fs::remove(GetStateRoot() / L"Msix" / (name + L".msix"), ec);

                result.succeeded = true;
                result.message = L"alias package removed";
                return result;
            }

            result.succeeded = false;
            result.message = L"package not found";
        }
        catch (const winrt::hresult_error& error)
        {
            result.succeeded = false;
            result.errorCode = error.code();
            result.message = error.message().c_str();
        }
        catch (const std::exception& error)
        {
            result.succeeded = false;
            result.message = Utf8ToWide(error.what());
        }
        return result;
    }

    OperationResult VerifyAlias(std::wstring_view alias)
    {
        OperationResult result{};
        try
        {
            const std::wstring normalized = NormalizeAlias(alias);
            for (const auto& record : ListAliases())
            {
                if (Lower(record.alias) == normalized)
                {
                    result.record = record;
                    result.succeeded = record.stubExists && record.stubIsAppExecLink;
                    result.message = result.succeeded ? L"alias has AppExecLink stub" : L"alias package found but stub missing or wrong type";
                    return result;
                }
            }

            result.record.alias = normalized;
            result.record.stubExists = ExistsNoThrow(GetWindowsAppsAliasPath(normalized));
            result.record.stubIsAppExecLink = result.record.stubExists && IsAppExecLink(GetWindowsAppsAliasPath(normalized));
            result.succeeded = false;
            result.message = result.record.stubExists ? L"stub exists but package manifest not found" : L"alias not found";
        }
        catch (const std::exception& error)
        {
            result.succeeded = false;
            result.message = Utf8ToWide(error.what());
        }
        return result;
    }

    std::wstring ToJsonString(std::wstring_view value)
    {
        std::wstring result = L"\"";
        for (const wchar_t ch : value)
        {
            switch (ch)
            {
            case L'\\': result += L"\\\\"; break;
            case L'"': result += L"\\\""; break;
            case L'\n': result += L"\\n"; break;
            case L'\r': result += L"\\r"; break;
            case L'\t': result += L"\\t"; break;
            default: result.push_back(ch); break;
            }
        }
        result.push_back(L'"');
        return result;
    }

    bool IsAppExecLink(const fs::path& path)
    {
        HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            return false;
        }

        std::array<std::byte, MAXIMUM_REPARSE_DATA_BUFFER_SIZE> buffer{};
        DWORD bytesReturned = 0;
        const BOOL ok = DeviceIoControl(file, FSCTL_GET_REPARSE_POINT, nullptr, 0, buffer.data(), static_cast<DWORD>(buffer.size()), &bytesReturned, nullptr);
        CloseHandle(file);
        if (!ok || bytesReturned < sizeof(ULONG))
        {
            return false;
        }

        const auto tag = *reinterpret_cast<const ULONG*>(buffer.data());
        return tag == AppExecLinkTag;
    }
}
