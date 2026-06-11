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

set(APPALIAS_CORE_SOURCES
    src/AppAlias.Core/AppAliasCore.cpp
)

add_library(AppAlias.Core STATIC ${APPALIAS_CORE_SOURCES})
target_include_directories(AppAlias.Core PUBLIC src/AppAlias.Core)
target_compile_definitions(AppAlias.Core PUBLIC UNICODE _UNICODE NOMINMAX)
target_compile_options(AppAlias.Core PRIVATE /W4 /WX)
target_link_libraries(AppAlias.Core PUBLIC windowsapp bcrypt version gdiplus shell32)

if(DEFINED CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION)
    set(APPALIAS_WINSDK_CPPWINRT "$ENV{ProgramFiles\(x86\)}/Windows Kits/10/Include/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/cppwinrt")
    if(EXISTS "${APPALIAS_WINSDK_CPPWINRT}")
        target_include_directories(AppAlias.Core PUBLIC "${APPALIAS_WINSDK_CPPWINRT}")
    endif()
endif()

add_executable(AppAlias.Proxy src/AppAlias.Proxy/main.cpp)
target_compile_definitions(AppAlias.Proxy PRIVATE UNICODE _UNICODE NOMINMAX)
target_compile_options(AppAlias.Proxy PRIVATE /W4 /WX)
target_link_libraries(AppAlias.Proxy PRIVATE AppAlias.Core shell32)

add_executable(AppAlias.Cli src/AppAlias.Cli/main.cpp)
target_compile_definitions(AppAlias.Cli PRIVATE UNICODE _UNICODE NOMINMAX)
target_compile_options(AppAlias.Cli PRIVATE /W4 /WX)
target_link_libraries(AppAlias.Cli PRIVATE AppAlias.Core shell32)

add_executable(AppAlias.Ui WIN32 src/AppAlias.Ui/main.cpp)
target_compile_definitions(AppAlias.Ui PRIVATE UNICODE _UNICODE NOMINMAX)
target_compile_options(AppAlias.Ui PRIVATE /W4 /WX)
target_link_libraries(AppAlias.Ui PRIVATE AppAlias.Core comctl32 shell32)

enable_testing()
add_executable(AppAliasCore.Tests tests/AppAliasCore.Tests.cpp)
target_compile_definitions(AppAliasCore.Tests PRIVATE UNICODE _UNICODE NOMINMAX)
target_compile_options(AppAliasCore.Tests PRIVATE /W4 /WX)
target_link_libraries(AppAliasCore.Tests PRIVATE AppAlias.Core)
add_test(NAME AppAliasCore.Tests COMMAND AppAliasCore.Tests)
add_test(
    NAME AppAliasProxy.Tests
    COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/AppAliasProxy.Tests.ps1" -ProxyPath "$<TARGET_FILE:AppAlias.Proxy>"
)

install(TARGETS AppAlias.Cli AppAlias.Proxy AppAlias.Ui RUNTIME DESTINATION bin)
