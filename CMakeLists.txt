cmake_minimum_required(VERSION 3.21)
if(POLICY CMP0091)
    cmake_policy(SET CMP0091 NEW)
endif()

project(AppAliasGenerator LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT WIN32)
    message(FATAL_ERROR "AppAliasGenerator native targets require Windows")
endif()

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

add_library(AppAlias.Headers INTERFACE)
target_include_directories(AppAlias.Headers INTERFACE src/AppAlias.Core)
target_compile_definitions(AppAlias.Headers INTERFACE UNICODE _UNICODE NOMINMAX)

add_library(AppAlias.Core STATIC src/AppAlias.Core/AppAliasCore.cpp)
target_link_libraries(AppAlias.Core PRIVATE AppAlias.Headers)
target_compile_definitions(AppAlias.Core PRIVATE UNICODE _UNICODE NOMINMAX)
target_compile_options(AppAlias.Core PRIVATE /W4 /WX)
target_link_libraries(AppAlias.Core PRIVATE windowsapp bcrypt version gdiplus shell32)

if(DEFINED CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION)
    set(APPALIAS_WINSDK_CPPWINRT "$ENV{ProgramFiles\(x86\)}/Windows Kits/10/Include/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/cppwinrt")
    if(EXISTS "${APPALIAS_WINSDK_CPPWINRT}")
        target_include_directories(AppAlias.Headers INTERFACE "${APPALIAS_WINSDK_CPPWINRT}")
    endif()
endif()

if(NOT DEFINED CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION OR NOT EXISTS "${APPALIAS_WINSDK_CPPWINRT}")
    find_path(APPALIAS_CPPWINRT_INCLUDE winrt/base.h
        PATHS "$ENV{ProgramFiles\(x86\)}/Windows Kits/10/Include"
        PATH_SUFFIXES cppwinrt
    )
    if(APPALIAS_CPPWINRT_INCLUDE)
        target_include_directories(AppAlias.Headers INTERFACE "${APPALIAS_CPPWINRT_INCLUDE}")
    endif()
endif()

add_executable(AppAlias.Proxy src/AppAlias.Proxy/main.cpp)
target_compile_definitions(AppAlias.Proxy PRIVATE UNICODE _UNICODE NOMINMAX)
target_compile_options(AppAlias.Proxy PRIVATE /W4 /WX)
target_link_libraries(AppAlias.Proxy PRIVATE AppAlias.Core AppAlias.Headers)

add_executable(AppAlias.Cli src/AppAlias.Cli/main.cpp)
target_compile_definitions(AppAlias.Cli PRIVATE UNICODE _UNICODE NOMINMAX)
target_compile_options(AppAlias.Cli PRIVATE /W4 /WX)
target_link_libraries(AppAlias.Cli PRIVATE AppAlias.Core AppAlias.Headers)

add_executable(AppAlias.Ui WIN32 src/AppAlias.Ui/main.cpp)
target_compile_definitions(AppAlias.Ui PRIVATE UNICODE _UNICODE NOMINMAX)
target_compile_options(AppAlias.Ui PRIVATE /W4 /WX)
target_link_libraries(AppAlias.Ui PRIVATE AppAlias.Core AppAlias.Headers comctl32)

enable_testing()
add_executable(AppAliasCore.Tests tests/AppAliasCore.Tests.cpp)
target_compile_definitions(AppAliasCore.Tests PRIVATE UNICODE _UNICODE NOMINMAX)
target_compile_options(AppAliasCore.Tests PRIVATE /W4 /WX)
target_link_libraries(AppAliasCore.Tests PRIVATE AppAlias.Core AppAlias.Headers)
add_test(NAME AppAliasCore.Tests COMMAND AppAliasCore.Tests)
set_tests_properties(AppAliasCore.Tests PROPERTIES TIMEOUT 120)
find_program(APPALIAS_POWERSHELL powershell.exe REQUIRED)
add_test(
    NAME AppAliasProxy.Tests
    COMMAND "${APPALIAS_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/AppAliasProxy.Tests.ps1" -ProxyPath "$<TARGET_FILE:AppAlias.Proxy>"
)
set_tests_properties(AppAliasProxy.Tests PROPERTIES TIMEOUT 120)
add_test(
    NAME AppAliasCli.Tests
    COMMAND "${APPALIAS_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/AppAliasCli.Tests.ps1" -CliPath "$<TARGET_FILE:AppAlias.Cli>"
)
set_tests_properties(AppAliasCli.Tests PROPERTIES TIMEOUT 120)

install(TARGETS AppAlias.Cli AppAlias.Proxy AppAlias.Ui
    RUNTIME DESTINATION bin
    COMPONENT Runtime
)
