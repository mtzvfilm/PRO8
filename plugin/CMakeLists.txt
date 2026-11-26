# PRO8 plugin: CLAP first, VST3 and AUv2 through clap-wrapper (MIT).
#   cmake -S plugin -B plugin/build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build plugin/build
cmake_minimum_required(VERSION 3.21)
set(CMAKE_OSX_DEPLOYMENT_TARGET 11.0 CACHE STRING "")
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "")
project(pro8 VERSION 1.0.1 LANGUAGES C CXX OBJC OBJCXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

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

add_library(pro8-impl STATIC src/pro8_clap.mm ../core/pro8_core.mm ../dsp/p800_engine.cpp)
target_link_libraries(pro8-impl PUBLIC clap "-framework Cocoa" "-framework WebKit")
target_compile_options(pro8-impl PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc> -O2)

make_clapfirst_plugins(
    TARGET_NAME pro8
    IMPL_TARGET pro8-impl
    OUTPUT_NAME "PRO8"
    ENTRY_SOURCE src/pro8_entry.cpp
    BUNDLE_IDENTIFIER "com.mtzvfilm.pro8"
    BUNDLE_VERSION ${PROJECT_VERSION}
    COPY_AFTER_BUILD TRUE
    PLUGIN_FORMATS CLAP VST3 AUV2
    RESOURCE_DIRECTORY ${RES}
    ASSET_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/out
    AUV2_MANUFACTURER_NAME "mtzvfilm"
    AUV2_MANUFACTURER_CODE "Mtzv"
    AUV2_SUBTYPE_CODE "Pro8"
    AUV2_INSTRUMENT_TYPE "aumu"
)

# clap-wrapper does not copy RESOURCE_DIRECTORY into the AUv2 bundle (yet): do it here, into the built and the installed copy
add_custom_command(TARGET pro8_auv2 POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${RES} $<TARGET_BUNDLE_DIR:pro8_auv2>/Contents/Resources
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${RES} $ENV{HOME}/Library/Audio/Plug-Ins/Components/PRO8.component/Contents/Resources
    COMMENT "PRO8: resources into the AU bundle")
