# PRO8 plugin: CLAP first, VST3 (and AUv2 on macOS) through clap-wrapper (MIT).
#   macOS:   cmake -S plugin -B plugin/build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build plugin/build
#   Windows: cmake -S plugin -B plugin/build -G "Visual Studio 17 2022" -A x64 && cmake --build plugin/build --config Release
#            (then plugin/make_zip_win.ps1)
cmake_minimum_required(VERSION 3.21)
set(CMAKE_OSX_DEPLOYMENT_TARGET 11.0 CACHE STRING "")
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "")
# Windows: static MSVC runtime, so the plugin needs no Visual C++ Redistributable
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
if(APPLE)
    project(pro8 VERSION 1.0.3 LANGUAGES C CXX OBJC OBJCXX)
else()
    project(pro8 VERSION 1.0.3 LANGUAGES C CXX)
endif()
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(MSVC)
    add_compile_options(/utf-8 /MP)
    add_compile_definitions(_USE_MATH_DEFINES NOMINMAX _CRT_SECURE_NO_WARNINGS)
endif()

add_subdirectory(third_party/clap)
set(CLAP_WRAPPER_DOWNLOAD_DEPENDENCIES TRUE CACHE BOOL "")
set(CLAP_WRAPPER_DONT_ADD_TARGETS FALSE CACHE BOOL "")
add_subdirectory(third_party/clap-wrapper)

# resources copied into every bundle: the panel and the seed banks
set(RES ${CMAKE_BINARY_DIR}/resources)
file(MAKE_DIRECTORY ${RES}/banks)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/../core/ui.html ${RES}/ui.html COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/../LICENSE ${RES}/LICENSE.txt COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/resources/THIRD_PARTY_LICENSES.txt ${RES}/THIRD_PARTY_LICENSES.txt COPYONLY)
file(GLOB SEED_BANKS ${CMAKE_CURRENT_SOURCE_DIR}/resources/banks/*.syx)
foreach(f ${SEED_BANKS})
    get_filename_component(n ${f} NAME)
    configure_file(${f} ${RES}/banks/${n} COPYONLY)
endforeach()

set(CORE_SOURCES ../core/pro8_core.cpp ../dsp/p800_engine.cpp src/pro8_clap.cpp)
if(APPLE)
    add_library(pro8-impl STATIC ${CORE_SOURCES} ../core/platform_mac.mm src/gui_mac.mm)
    target_link_libraries(pro8-impl PUBLIC clap "-framework Cocoa" "-framework WebKit" "-framework UniformTypeIdentifiers")
    target_compile_options(pro8-impl PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc> -O2)
    set(PRO8_FORMATS CLAP VST3 AUV2)
    set(PRO8_COPY TRUE)
elseif(WIN32)
    # WebView2 SDK (headers + static loader) from NuGet; or point WEBVIEW2_DIR at an unpacked Microsoft.Web.WebView2 package
    set(WEBVIEW2_VERSION "1.0.2903.40" CACHE STRING "Microsoft.Web.WebView2 NuGet package version")
    set(WEBVIEW2_DIR "" CACHE PATH "unpacked Microsoft.Web.WebView2 package (contains build/native)")
    if(NOT WEBVIEW2_DIR)
        include(FetchContent)
        FetchContent_Declare(webview2 URL https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2/${WEBVIEW2_VERSION}
                             DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
        FetchContent_MakeAvailable(webview2)
        set(WEBVIEW2_DIR ${webview2_SOURCE_DIR})
    endif()
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64|arm64")
            set(WV2_ARCH arm64)
        else()
            set(WV2_ARCH x64)
        endif()
    else()
        set(WV2_ARCH x86)
    endif()
    # ui.html and the seed banks compiled in (a single-file .clap has no Resources folder)
    set(EMBED_CPP ${CMAKE_BINARY_DIR}/generated/pro8_embedded.cpp)
    file(GLOB SEED_BANK_FILES ${CMAKE_CURRENT_SOURCE_DIR}/resources/banks/*.syx)
    add_custom_command(OUTPUT ${EMBED_CPP}
        COMMAND ${CMAKE_COMMAND} -DUI=${CMAKE_CURRENT_SOURCE_DIR}/../core/ui.html -DBANKS=${CMAKE_CURRENT_SOURCE_DIR}/resources/banks
                -DOUT=${EMBED_CPP} -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed.cmake
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/../core/ui.html ${SEED_BANK_FILES} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed.cmake
        COMMENT "PRO8: embedding ui.html and the seed banks")
    add_library(pro8-impl STATIC ${CORE_SOURCES} ../core/platform_win.cpp src/gui_win.cpp ${EMBED_CPP})
    target_include_directories(pro8-impl PRIVATE src ${WEBVIEW2_DIR}/build/native/include)
    target_link_libraries(pro8-impl PUBLIC clap ${WEBVIEW2_DIR}/build/native/${WV2_ARCH}/WebView2LoaderStatic.lib
                          user32 ole32 oleaut32 uuid shell32 comdlg32 advapi32 version)
    target_compile_options(pro8-impl PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/O2 /W3>)
    set(PRO8_FORMATS CLAP VST3)
    set(PRO8_COPY FALSE)   # Program Files\Common Files needs admin rights: copy by hand / with the installer
else()
    message(FATAL_ERROR "PRO8 builds on macOS and Windows (the panel needs WKWebView or WebView2)")
endif()

make_clapfirst_plugins(
    TARGET_NAME pro8
    IMPL_TARGET pro8-impl
    OUTPUT_NAME "PRO8"
    ENTRY_SOURCE src/pro8_entry.cpp
    BUNDLE_IDENTIFIER "com.mtzvfilm.pro8"
    BUNDLE_VERSION ${PROJECT_VERSION}
    COPY_AFTER_BUILD ${PRO8_COPY}
    PLUGIN_FORMATS ${PRO8_FORMATS}
    WINDOWS_FOLDER_VST3 TRUE
    RESOURCE_DIRECTORY ${RES}
    ASSET_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/out
    AUV2_MANUFACTURER_NAME "mtzvfilm"
    AUV2_MANUFACTURER_CODE "Mtzv"
    AUV2_SUBTYPE_CODE "Pro8"
    AUV2_INSTRUMENT_TYPE "aumu"
)

if(APPLE)
    # clap-wrapper does not copy RESOURCE_DIRECTORY into the AUv2 bundle (yet): do it here, into the built and the installed copy
    add_custom_command(TARGET pro8_auv2 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${RES} $<TARGET_BUNDLE_DIR:pro8_auv2>/Contents/Resources
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${RES} $ENV{HOME}/Library/Audio/Plug-Ins/Components/PRO8.component/Contents/Resources
        COMMENT "PRO8: resources into the AU bundle")
endif()
