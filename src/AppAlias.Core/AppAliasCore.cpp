#include "AppAliasInternal.h"

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
#include <charconv>
#include <cwctype>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <map>
#include <mutex>
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
    constexpr DWORD ProcessTimeoutMs = 120000;

    void EnsureWinrtApartment();

    struct AppAliasError : std::runtime_error
    {
        appalias::OperationErrorKind kind;
        HRESULT hr;

        AppAliasError(appalias::OperationErrorKind errorKind, const char* message, HRESULT errorCode = S_OK) :
            std::runtime_error(message),
            kind(errorKind),
            hr(errorCode)
        {
        }
    };

    void MarkFailure(appalias::OperationResult& result, appalias::OperationErrorKind kind, std::wstring message, HRESULT hr = S_OK)
    {
        result.succeeded = false;
        result.errorKind = kind;
        result.errorCode = hr;
        result.message = std::move(message);
    }

    wchar_t AsciiLower(wchar_t ch)
    {
        if (ch >= L'A' && ch <= L'Z')
        {
            return static_cast<wchar_t>(ch - L'A' + L'a');
        }
        return ch;
    }

    std::wstring Lower(std::wstring value)
    {
        std::transform(value.begin(), value.end(), value.begin(), AsciiLower);
        return value;
    }

    int CompareCaseInsensitive(std::wstring_view left, std::wstring_view right)
    {
        const size_t count = std::min(left.size(), right.size());
        for (size_t index = 0; index < count; ++index)
        {
            const wchar_t lch = AsciiLower(left[index]);
            const wchar_t rch = AsciiLower(right[index]);
            if (lch != rch)
            {
                return lch < rch ? -1 : 1;
            }
        }

        if (left.size() == right.size())
        {
            return 0;
        }
        return left.size() < right.size() ? -1 : 1;
    }

    bool EqualsCaseInsensitive(std::wstring_view left, std::wstring_view right)
    {
        return CompareCaseInsensitive(left, right) == 0;
    }

    bool StartsWith(std::wstring_view value, std::wstring_view prefix)
    {
        return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
    }

    std::string WideToUtf8Impl(std::wstring_view value)
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
        if (WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size, nullptr, nullptr) != size)
        {
            throw std::runtime_error("WideCharToMultiByte failed");
        }
        return result;
    }

    std::wstring Utf8ToWideImpl(std::string_view value)
    {
        if (value.empty())
        {
            return {};
        }

        const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
        if (size <= 0)
        {
            throw std::runtime_error("MultiByteToWideChar failed");
        }

        std::wstring result(static_cast<size_t>(size), L'\0');
        if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), size) != size)
        {
            throw std::runtime_error("MultiByteToWideChar failed");
        }
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
        if (alias.size() < exe.size() || alias.substr(alias.size() - exe.size()) != exe)
        {
            throw std::invalid_argument("alias must end with .exe");
        }

        alias.resize(alias.size() - exe.size());
        return alias;
    }

    uint32_t Fnv1a32(std::wstring_view value)
    {
        uint32_t hash = 2166136261u;
        std::wstring lowered;
        lowered.reserve(value.size());
        for (const wchar_t ch : value)
        {
            lowered.push_back(AsciiLower(ch));
        }

        const std::string bytes = WideToUtf8Impl(lowered);
        for (const unsigned char byte : bytes)
        {
            hash ^= byte;
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
                result.push_back(AsciiLower(ch));
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
        SetLastError(ERROR_SUCCESS);
        DWORD required = GetEnvironmentVariableW(name, nullptr, 0);
        if (required == 0)
        {
            if (GetLastError() != ERROR_ENVVAR_NOT_FOUND)
            {
                return {};
            }
            return {};
        }

        std::wstring value(required, L'\0');
        for (;;)
        {
            const DWORD copied = GetEnvironmentVariableW(name, value.data(), static_cast<DWORD>(value.size()));
            if (copied == 0)
            {
                return {};
            }
            if (copied < value.size())
            {
                value.resize(copied);
                return value;
            }
            value.resize(copied + 1);
        }
    }

    void EnsureDirectory(const fs::path& path)
    {
        std::error_code ec;
        fs::create_directories(path, ec);
        if (ec)
        {
            throw std::runtime_error("failed to create directory: " + WideToUtf8Impl(path.wstring()));
        }
    }

    bool ExistsNoThrow(const fs::path& path)
    {
        std::error_code ec;
        const bool exists = fs::exists(path, ec);
        if (!ec)
        {
            return exists;
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

        const auto bytes = WideToUtf8Impl(text);
        file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }

    std::wstring ReadUtf8Text(const fs::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file)
        {
            if (ExistsNoThrow(path))
            {
                throw std::runtime_error("failed to open input file: " + WideToUtf8Impl(path.wstring()));
            }
            return {};
        }

        std::stringstream buffer;
        buffer << file.rdbuf();
        if (!file.good() && !file.eof())
        {
            throw std::runtime_error("failed to read input file: " + WideToUtf8Impl(path.wstring()));
        }
        return Utf8ToWideImpl(buffer.str());
    }

    fs::path GetModulePathImpl()
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

    std::wstring QuoteCommandArgumentImpl(std::wstring_view value)
    {
        std::wstring result = L"\"";
        size_t backslashes = 0;
        for (const wchar_t ch : value)
        {
            if (ch == L'\\')
            {
                ++backslashes;
                continue;
            }

            if (ch == L'"')
            {
                result.append((backslashes * 2) + 1, L'\\');
                result.push_back(L'"');
            }
            else
            {
                result.append(backslashes, L'\\');
                result.push_back(ch);
            }
            backslashes = 0;
        }
        result.append(backslashes * 2, L'\\');
        result.push_back(L'"');
        return result;
    }

    int RunProcess(const fs::path& exe, const std::wstring& args)
    {
        std::wstring command = QuoteCommandArgumentImpl(exe.wstring());
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

        if (!CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process))
        {
            return static_cast<int>(GetLastError());
        }

        const DWORD wait = WaitForSingleObject(process.hProcess, ProcessTimeoutMs);
        if (wait == WAIT_TIMEOUT)
        {
            TerminateProcess(process.hProcess, WAIT_TIMEOUT);
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
            return static_cast<int>(WAIT_TIMEOUT);
        }
        if (wait == WAIT_FAILED)
        {
            const DWORD error = GetLastError();
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
            return static_cast<int>(error);
        }

        DWORD exitCode = 1;
        GetExitCodeProcess(process.hProcess, &exitCode);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return static_cast<int>(exitCode);
    }

    fs::path SearchPathTool(const std::wstring& tool)
    {
        DWORD required = SearchPathW(nullptr, tool.c_str(), nullptr, 0, nullptr, nullptr);
        if (required == 0)
        {
            return {};
        }

        std::wstring buffer(required + 1, L'\0');
        for (;;)
        {
            const DWORD written = SearchPathW(nullptr, tool.c_str(), nullptr, static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
            if (written == 0)
            {
                return {};
            }
            if (written < buffer.size())
            {
                buffer.resize(written);
                return fs::path(buffer);
            }
            buffer.resize(written + 1);
        }
    }

    std::vector<int> VersionParts(std::wstring value)
    {
        std::vector<int> parts;
        size_t start = 0;
        while (start <= value.size())
        {
            const size_t dot = value.find(L'.', start);
            const size_t end = dot == std::wstring::npos ? value.size() : dot;
            int part = 0;
            bool ok = end > start;
            for (size_t index = start; index < end; ++index)
            {
                if (value[index] < L'0' || value[index] > L'9')
                {
                    ok = false;
                    break;
                }
                part = (part * 10) + static_cast<int>(value[index] - L'0');
            }
            parts.push_back(ok ? part : -1);
            if (dot == std::wstring::npos)
            {
                break;
            }
            start = dot + 1;
        }
        return parts;
    }

    bool SdkToolPathGreater(const fs::path& left, const fs::path& right)
    {
        const auto leftVersion = VersionParts(left.parent_path().parent_path().filename().wstring());
        const auto rightVersion = VersionParts(right.parent_path().parent_path().filename().wstring());
        if (leftVersion != rightVersion)
        {
            return leftVersion > rightVersion;
        }
        return left.wstring() > right.wstring();
    }

    fs::path FindSdkTool(const std::wstring& tool)
    {
        static std::mutex cacheMutex;
        static std::map<std::wstring, fs::path> cache;

        const std::wstring cacheKey = Lower(tool);
        std::lock_guard<std::mutex> lock(cacheMutex);
        const auto found = cache.find(cacheKey);
        if (found != cache.end())
        {
            return found->second;
        }

        fs::path result;
        const auto fromPath = SearchPathTool(tool);
        if (!fromPath.empty())
        {
            result = fromPath;
        }
        else
        {
            const std::wstring programFilesX86 = GetEnvPath(L"ProgramFiles(x86)");
            if (!programFilesX86.empty())
            {
                const fs::path binRoot = fs::path(programFilesX86) / L"Windows Kits" / L"10" / L"bin";
                if (ExistsNoThrow(binRoot))
                {
                    std::vector<fs::path> candidates;
                    std::error_code ec;
                    fs::recursive_directory_iterator iterator(binRoot, fs::directory_options::skip_permission_denied, ec);
                    const fs::recursive_directory_iterator end;
                    while (!ec && iterator != end)
                    {
                        const auto& entry = *iterator;
                        std::error_code fileEc;
                        if (entry.is_regular_file(fileEc) && !fileEc && Lower(entry.path().filename().wstring()) == cacheKey)
                        {
                            const auto parent = Lower(entry.path().parent_path().filename().wstring());
                            if (parent == L"x64")
                            {
                                candidates.push_back(entry.path());
                            }
                        }

                        iterator.increment(ec);
                    }

                    std::sort(candidates.begin(), candidates.end(), SdkToolPathGreater);
                    if (!candidates.empty())
                    {
                        result = candidates.front();
                    }
                }
            }
        }

        cache[cacheKey] = result;
        return result;
    }

    std::wstring ConfiguredPfxPassword()
    {
        const std::wstring password = GetEnvPath(L"APPALIAS_PFX_PASSWORD");
        if (password.empty())
        {
            throw std::runtime_error("APPALIAS_PFX_PASSWORD must be set when APPALIAS_PFX is used");
        }
        return password;
    }

    std::wstring ConfiguredPublisherSubject()
    {
        const std::wstring publisher = GetEnvPath(L"APPALIAS_PUBLISHER_SUBJECT");
        return publisher.empty() ? DefaultPublisher : publisher;
    }

    bool IsValidPackageVersion(std::wstring_view version)
    {
        if (version.empty())
        {
            return false;
        }

        size_t start = 0;
        int parts = 0;
        while (start <= version.size())
        {
            const size_t dot = version.find(L'.', start);
            const size_t end = dot == std::wstring_view::npos ? version.size() : dot;
            if (end == start || end - start > 5)
            {
                return false;
            }
            if (end - start > 1 && version[start] == L'0')
            {
                return false;
            }

            uint32_t value = 0;
            for (size_t index = start; index < end; ++index)
            {
                if (version[index] < L'0' || version[index] > L'9')
                {
                    return false;
                }
                value = (value * 10) + static_cast<uint32_t>(version[index] - L'0');
                if (value > 65535)
                {
                    return false;
                }
            }

            ++parts;
            if (dot == std::wstring_view::npos)
            {
                break;
            }
            start = dot + 1;
        }

        return parts == 4;
    }

    std::wstring ResolvePackageVersion(std::wstring_view version)
    {
        if (version.empty())
        {
            return DefaultVersion;
        }

        if (!IsValidPackageVersion(version))
        {
            throw std::invalid_argument("package version must be four dot-separated numbers from 0 to 65535");
        }

        return std::wstring(version);
    }

    fs::path ConfiguredPfxPath()
    {
        const std::wstring configured = GetEnvPath(L"APPALIAS_PFX");
        return configured.empty() ? fs::path{} : fs::path(configured);
    }

    std::wstring PathToFileUriImpl(const fs::path& path)
    {
        if (!path.is_absolute())
        {
            throw std::invalid_argument("file uri requires an absolute path");
        }

        std::wstring value = path.wstring();
        std::replace(value.begin(), value.end(), L'\\', L'/');
        std::wstring uri = StartsWith(value, L"//") ? L"file:" : L"file:///";
        const std::string bytes = WideToUtf8Impl(value);
        constexpr wchar_t hex[] = L"0123456789ABCDEF";
        for (const unsigned char byte : bytes)
        {
            const bool unreserved =
                (byte >= 'A' && byte <= 'Z') ||
                (byte >= 'a' && byte <= 'z') ||
                (byte >= '0' && byte <= '9') ||
                byte == '-' ||
                byte == '.' ||
                byte == '_' ||
                byte == '~' ||
                byte == '/' ||
                byte == ':';

            if (unreserved)
            {
                uri.push_back(static_cast<wchar_t>(byte));
            }
            else
            {
                uri.push_back(L'%');
                uri.push_back(hex[(byte >> 4) & 0x0F]);
                uri.push_back(hex[byte & 0x0F]);
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
        const fs::path current = GetModulePathImpl();
        const fs::path proxySource = current.parent_path() / L"AppAlias.Proxy.exe";
        if (!ExistsNoThrow(proxySource))
        {
            throw std::runtime_error("AppAlias.Proxy.exe not found next to CLI");
        }

        EnsureDirectory(destinationRoot);
        fs::copy_file(proxySource, destinationRoot / L"AppAlias.Proxy.exe", fs::copy_options::overwrite_existing);
    }

    CLSID GetPngEncoderClsid()
    {
        static std::once_flag once;
        static CLSID cached{};
        static std::exception_ptr failure;

        std::call_once(once, [] {
            try
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
                        cached = encoders[index].Clsid;
                        return;
                    }
                }

                throw std::runtime_error("png encoder not found");
            }
            catch (...)
            {
                failure = std::current_exception();
            }
        });

        if (failure)
        {
            std::rethrow_exception(failure);
        }

        return cached;
    }

    void EnsureGdiplusStarted()
    {
        static std::once_flag once;
        static ULONG_PTR token = 0;
        static std::exception_ptr failure;

        std::call_once(once, [] {
            try
            {
                Gdiplus::GdiplusStartupInput input{};
                if (Gdiplus::GdiplusStartup(&token, &input, nullptr) != Gdiplus::Ok)
                {
                    throw std::runtime_error("failed to initialize gdiplus");
                }
            }
            catch (...)
            {
                failure = std::current_exception();
            }
        });

        if (failure)
        {
            std::rethrow_exception(failure);
        }
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
        HICON largeIcon = nullptr;
        HICON smallIcon = nullptr;
        const UINT count = ExtractIconExW(target.c_str(), 0, &largeIcon, &smallIcon, 1);
        if (count == 0 || count == static_cast<UINT>(-1))
        {
            throw std::runtime_error("failed to extract target icon");
        }

        if (smallIcon)
        {
            DestroyIcon(smallIcon);
        }

        if (!largeIcon)
        {
            throw std::runtime_error("failed to extract target icon");
        }

        return largeIcon;
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

        const std::wstring args = L"pack /o /d " + QuoteCommandArgumentImpl(packageRoot.wstring()) + L" /p " + QuoteCommandArgumentImpl(packagePath.wstring());
        const int exitCode = RunProcess(makeappx, args);
        if (exitCode != 0)
        {
            if (exitCode == static_cast<int>(WAIT_TIMEOUT))
            {
                throw AppAliasError(appalias::OperationErrorKind::Timeout, "makeappx.exe timed out");
            }
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
            args += L"/s My /sha1 " + QuoteCommandArgumentImpl(certSha1) + L" ";
            if (certStore == L"localmachine" || certStore == L"machine")
            {
                args += L"/sm ";
            }
        }
        else
        {
            const fs::path pfx = ConfiguredPfxPath();
            if (pfx.empty())
            {
                throw std::runtime_error("APPALIAS_CERT_SHA1 must be set for cert-store signing, or APPALIAS_PFX and APPALIAS_PFX_PASSWORD must be set for PFX signing");
            }
            if (!ExistsNoThrow(pfx))
            {
                throw std::runtime_error("configured signing certificate PFX not found: " + WideToUtf8Impl(pfx.wstring()));
            }
            args += L"/f " + QuoteCommandArgumentImpl(pfx.wstring()) + L" /p " + QuoteCommandArgumentImpl(ConfiguredPfxPassword()) + L" ";
        }

        args += QuoteCommandArgumentImpl(appalias::GetPackageMsixPath(identity).wstring());
        const int exitCode = RunProcess(signtool, args);
        if (exitCode != 0)
        {
            if (exitCode == static_cast<int>(WAIT_TIMEOUT))
            {
                throw AppAliasError(appalias::OperationErrorKind::Timeout, "signtool.exe timed out");
            }
            throw std::runtime_error("signtool.exe failed");
        }
    }

    appalias::OperationResult DeployPackage(const appalias::PackageIdentity& identity)
    {
        appalias::OperationResult result{};
        try
        {
            EnsureWinrtApartment();

            winrt::Windows::Management::Deployment::PackageManager packageManager;
            winrt::Windows::Management::Deployment::AddPackageOptions options;

            const auto deployment = packageManager.AddPackageByUriAsync(
                winrt::Windows::Foundation::Uri(PathToFileUriImpl(appalias::GetPackageMsixPath(identity))),
                options).get();

            const HRESULT hr = deployment.ExtendedErrorCode();
            if (FAILED(hr))
            {
                MarkFailure(result, appalias::OperationErrorKind::Failed, deployment.ErrorText().c_str(), hr);
                return result;
            }

            result.succeeded = true;
            result.errorKind = appalias::OperationErrorKind::None;
            result.message = L"alias package registered";
        }
        catch (const winrt::hresult_error& error)
        {
            MarkFailure(result, appalias::OperationErrorKind::Failed, error.message().c_str(), error.code());
        }
        catch (const std::exception& error)
        {
            MarkFailure(result, appalias::OperationErrorKind::Failed, Utf8ToWideImpl(error.what()));
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

    void EnsureWinrtApartment()
    {
        thread_local bool initialized = false;
        if (!initialized)
        {
            winrt::init_apartment();
            initialized = true;
        }
    }

    int HexDigit(wchar_t ch)
    {
        if (ch >= L'0' && ch <= L'9')
        {
            return static_cast<int>(ch - L'0');
        }
        if (ch >= L'a' && ch <= L'f')
        {
            return static_cast<int>(ch - L'a' + 10);
        }
        if (ch >= L'A' && ch <= L'F')
        {
            return static_cast<int>(ch - L'A' + 10);
        }
        return -1;
    }

    uint32_t ReadJsonHex4(const std::wstring& value, size_t offset)
    {
        if (offset + 4 > value.size())
        {
            throw std::runtime_error("incomplete json unicode escape");
        }

        uint32_t codeUnit = 0;
        for (size_t index = 0; index < 4; ++index)
        {
            const int digit = HexDigit(value[offset + index]);
            if (digit < 0)
            {
                throw std::runtime_error("invalid json unicode escape");
            }
            codeUnit = (codeUnit << 4) | static_cast<uint32_t>(digit);
        }
        return codeUnit;
    }

    void AppendJsonCodePoint(std::wstring& output, uint32_t codePoint)
    {
        if (codePoint <= 0xFFFF)
        {
            output.push_back(static_cast<wchar_t>(codePoint));
            return;
        }

        codePoint -= 0x10000;
        output.push_back(static_cast<wchar_t>(0xD800 + ((codePoint >> 10) & 0x3FF)));
        output.push_back(static_cast<wchar_t>(0xDC00 + (codePoint & 0x3FF)));
    }

    std::wstring ParseJsonStringAt(const std::wstring& json, size_t start)
    {
        std::wstring result;
        for (size_t index = start; index < json.size(); ++index)
        {
            const wchar_t ch = json[index];
            if (ch == L'"')
            {
                return result;
            }

            if (ch != L'\\')
            {
                result.push_back(ch);
                continue;
            }

            if (++index >= json.size())
            {
                throw std::runtime_error("incomplete json escape");
            }

            switch (json[index])
            {
            case L'"': result.push_back(L'"'); break;
            case L'\\': result.push_back(L'\\'); break;
            case L'/': result.push_back(L'/'); break;
            case L'b': result.push_back(L'\b'); break;
            case L'f': result.push_back(L'\f'); break;
            case L'n': result.push_back(L'\n'); break;
            case L'r': result.push_back(L'\r'); break;
            case L't': result.push_back(L'\t'); break;
            case L'u':
            {
                uint32_t codeUnit = ReadJsonHex4(json, index + 1);
                index += 4;
                if (codeUnit >= 0xD800 && codeUnit <= 0xDBFF)
                {
                    if (index + 6 >= json.size() || json[index + 1] != L'\\' || json[index + 2] != L'u')
                    {
                        throw std::runtime_error("missing json low surrogate");
                    }

                    const uint32_t low = ReadJsonHex4(json, index + 3);
                    if (low < 0xDC00 || low > 0xDFFF)
                    {
                        throw std::runtime_error("invalid json low surrogate");
                    }

                    index += 6;
                    const uint32_t codePoint = 0x10000 + (((codeUnit - 0xD800) << 10) | (low - 0xDC00));
                    AppendJsonCodePoint(result, codePoint);
                }
                else
                {
                    AppendJsonCodePoint(result, codeUnit);
                }
                break;
            }
            default:
                throw std::runtime_error("invalid json escape");
            }
        }

        throw std::runtime_error("unterminated json string");
    }

    std::wstring JsonStringValueImpl(const std::wstring& json, const std::wstring& key)
    {
        size_t index = 0;
        while (index < json.size())
        {
            while (index < json.size() && json[index] != L'"')
            {
                ++index;
            }

            if (index >= json.size())
            {
                break;
            }

            const size_t keyStart = index + 1;
            const std::wstring memberName = ParseJsonStringAt(json, keyStart);
            index = keyStart;
            bool escaped = false;
            while (index < json.size())
            {
                const wchar_t ch = json[index++];
                if (escaped)
                {
                    escaped = false;
                    continue;
                }
                if (ch == L'\\')
                {
                    escaped = true;
                    continue;
                }
                if (ch == L'"')
                {
                    break;
                }
            }

            while (index < json.size() && iswspace(json[index]))
            {
                ++index;
            }

            if (index >= json.size() || json[index] != L':')
            {
                continue;
            }
            ++index;
            while (index < json.size() && iswspace(json[index]))
            {
                ++index;
            }

            if (memberName == key)
            {
                if (index < json.size() && json[index] == L'"')
                {
                    return ParseJsonStringAt(json, index + 1);
                }
                return {};
            }

            if (index < json.size() && json[index] == L'"')
            {
                ++index;
                bool valueEscaped = false;
                while (index < json.size())
                {
                    const wchar_t ch = json[index++];
                    if (valueEscaped)
                    {
                        valueEscaped = false;
                        continue;
                    }
                    if (ch == L'\\')
                    {
                        valueEscaped = true;
                        continue;
                    }
                    if (ch == L'"')
                    {
                        break;
                    }
                }
            }
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
        record.installedPackagePath = fs::path(package.InstalledLocation().Path().c_str());

        const fs::path configPath = FindConfigForPackage(record.packageName);
        if (record.owned && ExistsNoThrow(configPath))
        {
            const auto config = ReadUtf8Text(configPath);
            record.targetPath = JsonStringValueImpl(config, L"target");
            record.externalLocation = configPath.parent_path();
        }

        const fs::path stub = appalias::GetWindowsAppsAliasPath(alias);
        record.stubExists = ExistsNoThrow(stub);
        record.stubIsAppExecLink = record.stubExists && appalias::IsAppExecLink(stub);
        return record;
    }
}

namespace appalias
{
    std::string WideToUtf8(std::wstring_view value)
    {
        return WideToUtf8Impl(value);
    }

    std::wstring Utf8ToWide(std::string_view value)
    {
        return Utf8ToWideImpl(value);
    }

    std::wstring QuoteCommandArgument(std::wstring_view value)
    {
        return QuoteCommandArgumentImpl(value);
    }

    fs::path GetModulePath()
    {
        return GetModulePathImpl();
    }

    std::wstring PathToFileUri(const fs::path& path)
    {
        return PathToFileUriImpl(path);
    }

    std::wstring JsonStringValue(const std::wstring& json, const std::wstring& key)
    {
        return JsonStringValueImpl(json, key);
    }

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

        for (size_t index = 0; index < value.size(); ++index)
        {
            const wchar_t ch = value[index];
            const bool allowed =
                (ch >= L'a' && ch <= L'z') ||
                (ch >= L'0' && ch <= L'9') ||
                ch == L'.' ||
                ch == L'-' ||
                ch == L'_';

            if (!allowed)
            {
                std::ostringstream message;
                message << "alias contains invalid character U+"
                    << std::uppercase << std::hex << std::setw(4) << std::setfill('0') << static_cast<unsigned int>(ch)
                    << " at position " << std::dec << index;
                throw std::invalid_argument(message.str());
            }
        }

        if (value.find(L"..") != std::wstring::npos || value.front() == L'.')
        {
            throw std::invalid_argument("alias contains invalid dot sequence");
        }

        return value;
    }

    PackageIdentity BuildIdentity(std::wstring_view alias, std::wstring_view displayName, std::wstring_view publisherDisplayName, std::wstring_view packageVersion)
    {
        PackageIdentity identity{};
        identity.alias = NormalizeAlias(alias);
        const std::wstring aliasStem = StripExe(identity.alias);
        identity.packageName = std::wstring(PackagePrefix) + SanitizeIdentityPart(aliasStem) + L"." + Hash8(identity.alias);
        identity.applicationId = DefaultApplicationId;
        identity.publisher = ConfiguredPublisherSubject();
        identity.displayName = displayName.empty() ? aliasStem : std::wstring(displayName);
        identity.publisherDisplayName = publisherDisplayName.empty() ? L"AppAliasGenerator" : std::wstring(publisherDisplayName);
        identity.version = ResolvePackageVersion(packageVersion);
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

        EnsureGdiplusStarted();

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
            throw;
        }

        if (icon)
        {
            DestroyIcon(icon);
        }
    }

    bool TryFindAliasRecord(std::wstring_view alias, AliasRecord& record);

    OperationResult CreateAlias(const AliasCreateOptions& options)
    {
        OperationResult result{};
        try
        {
            const PackageIdentity identity = BuildIdentity(options.alias, options.displayName, options.publisherDisplayName, options.packageVersion);
            const fs::path target = fs::absolute(fs::path(options.targetPath));
            if (!ExistsNoThrow(target))
            {
                throw AppAliasError(appalias::OperationErrorKind::NotFound, "target path not found");
            }

            AliasRecord existing{};
            const bool hasExisting = TryFindAliasRecord(identity.alias, existing);
            const bool stubExists = ExistsNoThrow(GetWindowsAppsAliasPath(identity.alias));
            if (!options.force)
            {
                if (hasExisting || stubExists)
                {
                    throw AppAliasError(appalias::OperationErrorKind::Failed, "alias already exists; pass --force to replace owned alias");
                }
            }
            else
            {
                if (hasExisting && !existing.owned)
                {
                    throw AppAliasError(appalias::OperationErrorKind::ForeignAlias, "refusing to replace foreign package alias");
                }

                if (!hasExisting && stubExists)
                {
                    throw AppAliasError(appalias::OperationErrorKind::StubInvalid, "refusing to replace alias stub without owned package");
                }

                if (hasExisting)
                {
                    const OperationResult removed = RemoveAliasByPackage(existing.packageFullName);
                    if (!removed.succeeded)
                    {
                        result = removed;
                        return result;
                    }
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
            result.record.stagedMsixPath = GetPackageMsixPath(identity);
            result.record.externalLocation = externalRoot;
            result.record.owned = true;
            result.record.stubExists = ExistsNoThrow(GetWindowsAppsAliasPath(identity.alias));
            result.record.stubIsAppExecLink = result.record.stubExists && IsAppExecLink(GetWindowsAppsAliasPath(identity.alias));
        }
        catch (const AppAliasError& error)
        {
            MarkFailure(result, error.kind, Utf8ToWide(error.what()), error.hr);
        }
        catch (const std::exception& error)
        {
            MarkFailure(result, OperationErrorKind::Failed, Utf8ToWide(error.what()));
        }
        return result;
    }

    std::vector<AliasRecord> ListAliases()
    {
        std::vector<AliasRecord> records;
        EnsureWinrtApartment();
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

    bool TryFindAliasRecord(std::wstring_view alias, AliasRecord& record)
    {
        const std::wstring normalized = NormalizeAlias(alias);
        for (const auto& candidate : ListAliases())
        {
            if (Lower(candidate.alias) == normalized)
            {
                record = candidate;
                return true;
            }
        }
        return false;
    }

    OperationResult RemoveAliasByAlias(std::wstring_view alias)
    {
        AliasRecord record{};
        if (TryFindAliasRecord(alias, record))
        {
            return RemoveAliasByPackage(record.packageFullName);
        }

        OperationResult result{};
        MarkFailure(result, OperationErrorKind::NotFound, L"alias not found");
        return result;
    }

    OperationResult RemoveAliasByPackage(std::wstring_view packageNameOrFullName)
    {
        OperationResult result{};
        try
        {
            EnsureWinrtApartment();
            winrt::Windows::Management::Deployment::PackageManager packageManager;

            for (const auto& package : packageManager.FindPackagesForUser(L""))
            {
                const std::wstring name = package.Id().Name().c_str();
                const std::wstring fullName = package.Id().FullName().c_str();
                if (!EqualsCaseInsensitive(name, packageNameOrFullName) && !EqualsCaseInsensitive(fullName, packageNameOrFullName))
                {
                    continue;
                }

                if (!StartsWith(name, PackagePrefix))
                {
                    MarkFailure(result, OperationErrorKind::ForeignAlias, L"refusing to remove foreign package alias");
                    return result;
                }

                const auto deployment = packageManager.RemovePackageAsync(fullName).get();
                const HRESULT hr = deployment.ExtendedErrorCode();
                if (FAILED(hr))
                {
                    MarkFailure(result, OperationErrorKind::Failed, deployment.ErrorText().c_str(), hr);
                    return result;
                }

                std::error_code ec;
                std::wstring cleanupError;
                fs::remove_all(GetStateRoot() / L"External" / name, ec);
                if (ec)
                {
                    cleanupError = L"; external state cleanup failed: " + Utf8ToWide(ec.message());
                    ec.clear();
                }
                fs::remove_all(GetStateRoot() / L"Packages" / name, ec);
                if (ec)
                {
                    cleanupError += L"; package state cleanup failed: " + Utf8ToWide(ec.message());
                    ec.clear();
                }
                fs::remove(GetStateRoot() / L"Msix" / (name + L".msix"), ec);
                if (ec)
                {
                    cleanupError += L"; msix cleanup failed: " + Utf8ToWide(ec.message());
                }

                result.succeeded = true;
                result.errorKind = OperationErrorKind::None;
                result.message = L"alias package removed" + cleanupError;
                return result;
            }

            MarkFailure(result, OperationErrorKind::NotFound, L"package not found");
        }
        catch (const winrt::hresult_error& error)
        {
            MarkFailure(result, OperationErrorKind::Failed, error.message().c_str(), error.code());
        }
        catch (const std::exception& error)
        {
            MarkFailure(result, OperationErrorKind::Failed, Utf8ToWide(error.what()));
        }
        return result;
    }

    OperationResult VerifyAlias(std::wstring_view alias)
    {
        OperationResult result{};
        try
        {
            const std::wstring normalized = NormalizeAlias(alias);
            AliasRecord record{};
            if (TryFindAliasRecord(normalized, record))
            {
                result.record = record;
                result.succeeded = record.stubExists && record.stubIsAppExecLink;
                result.errorKind = result.succeeded ? OperationErrorKind::None : OperationErrorKind::StubInvalid;
                result.message = result.succeeded ? L"alias has AppExecLink stub" : L"alias package found but stub missing or wrong type";
                return result;
            }

            result.record.alias = normalized;
            result.record.stubExists = ExistsNoThrow(GetWindowsAppsAliasPath(normalized));
            result.record.stubIsAppExecLink = result.record.stubExists && IsAppExecLink(GetWindowsAppsAliasPath(normalized));
            MarkFailure(result, result.record.stubExists ? OperationErrorKind::StubInvalid : OperationErrorKind::NotFound, result.record.stubExists ? L"stub exists but package manifest not found" : L"alias not found");
        }
        catch (const std::exception& error)
        {
            MarkFailure(result, OperationErrorKind::Failed, Utf8ToWide(error.what()));
        }
        return result;
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
