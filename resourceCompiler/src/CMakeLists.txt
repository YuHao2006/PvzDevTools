cmake_minimum_required(VERSION 3.24)
project(reanimc_standalone CXX)

# 独立构建 reanimc（reanim 编译器）。要求 Ninja + MSVC x64（与 openPvZ 主工程一致）。
if(WIN32)
    if(NOT CMAKE_GENERATOR STREQUAL "Ninja")
        message(FATAL_ERROR "reanimc requires the Ninja generator on Windows")
    endif()
    if(NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
        message(FATAL_ERROR "reanimc requires the MSVC x64 compiler on Windows")
    endif()
endif()

add_executable(reanimc
    AtlasPacking.cpp
    main.cpp
    Pak64.cpp
    ParticleDsl.cpp
    ProjectileDsl.cpp
    StringIdGen.cpp
    TrailDsl.cpp)
target_include_directories(reanimc PRIVATE include)
target_compile_features(reanimc PRIVATE cxx_std_23)
target_compile_options(reanimc PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
find_package(ZLIB REQUIRED)
target_link_libraries(reanimc PRIVATE ZLIB::ZLIB)
