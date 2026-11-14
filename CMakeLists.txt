cmake_minimum_required(VERSION 3.24)
project(PvzDevTools VERSION 0.1.3 LANGUAGES CXX)

find_package(ZLIB REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(SPNG CONFIG REQUIRED)
find_package(libjpeg-turbo CONFIG REQUIRED)
find_package(ortools 9.15.6755 EXACT CONFIG REQUIRED)
include(CTest)
set(PVZ_TEST_FIXTURE_DIR "" CACHE PATH "Optional pvz_perf Tests directory for converter regression fixtures")
add_subdirectory(resourceCompiler/src)
add_subdirectory(saveConverter/src)
add_executable(pvz-resource-build
    resourceBuilder/main.cpp
    resourceBuilder/NativeCommon.cpp
    resourceBuilder/NativeConfig.cpp
    resourceBuilder/NativeFonts.cpp
    resourceBuilder/NativeResources.cpp
    resourceBuilder/NativeImages.cpp
    resourceBuilder/NativeAtlas.cpp
    resourceBuilder/NativePackAtlas.cpp
    resourceBuilder/NativeDefinitions.cpp
    resourceBuilder/NativeScenes.cpp
    resourceBuilder/NativeUiCompositions.cpp
    resourceBuilder/NativeGenerate.cpp
    resourceBuilder/NativePipeline.cpp
    resourceBuilder/NativeCompact.cpp)
target_compile_features(pvz-resource-build PRIVATE cxx_std_23)
target_compile_options(pvz-resource-build PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
target_link_libraries(pvz-resource-build PRIVATE ZLIB::ZLIB nlohmann_json::nlohmann_json spng::spng_static libjpeg-turbo::jpeg-static ortools::ortools)
if(BUILD_TESTING)
    add_executable(pvz-resource-image-test tests/resource-image-geometry.cpp
        resourceBuilder/NativeCommon.cpp resourceBuilder/NativeImages.cpp resourceBuilder/NativeScenes.cpp)
    target_include_directories(pvz-resource-image-test PRIVATE resourceBuilder)
    target_compile_features(pvz-resource-image-test PRIVATE cxx_std_23)
    target_compile_options(pvz-resource-image-test PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
    target_link_libraries(pvz-resource-image-test PRIVATE ZLIB::ZLIB nlohmann_json::nlohmann_json spng::spng_static libjpeg-turbo::jpeg-static)
    add_test(NAME high-resolution-image-geometry COMMAND pvz-resource-image-test "${CMAKE_CURRENT_BINARY_DIR}/hd-image-test")
    add_executable(pvz-ui-composition-test tests/ui-composition.cpp resourceBuilder/NativeUiCompositions.cpp resourceBuilder/NativeCommon.cpp)
    target_include_directories(pvz-ui-composition-test PRIVATE resourceBuilder)
    target_compile_features(pvz-ui-composition-test PRIVATE cxx_std_23)
    target_compile_options(pvz-ui-composition-test PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
    target_link_libraries(pvz-ui-composition-test PRIVATE ZLIB::ZLIB nlohmann_json::nlohmann_json)
    add_test(NAME compact-ui-composition COMMAND pvz-ui-composition-test)

endif()
if(WIN32)
    add_custom_command(TARGET pvz-resource-build POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:pvz-resource-build> $<TARGET_FILE_DIR:pvz-resource-build>
        COMMAND ${CMAKE_COMMAND} -DEXECUTABLE=$<TARGET_FILE:pvz-resource-build> -DDEPENDENCY_DIRECTORY=$<TARGET_FILE_DIR:ortools::ortools>
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/StageRuntime.cmake
        COMMAND_EXPAND_LISTS)
endif()
