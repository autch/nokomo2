cmake_minimum_required(VERSION 3.20)
project(nokomo2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT WIN32)
    message(WARNING "nokomo2 は Windows 専用です(クロスコンパイル時のみ続行)")
endif()

add_library(nokomo2 STATIC
    lib/strconv.cpp
    lib/host_objects.cpp
    lib/module.cpp
    lib/kmp_adapter.cpp
    lib/registry.cpp
    lib/decode.cpp
    lib/wav_writer.cpp
)
target_include_directories(nokomo2
    PUBLIC include
    PRIVATE kpisdk lib
)
target_compile_definitions(nokomo2 PRIVATE UNICODE _UNICODE NOMINMAX)

if(MSVC)
    target_compile_options(nokomo2 PRIVATE /utf-8 /W4)
else()
    # clang (--target=x86_64-w64-mingw32 等): __uuidof / __declspec(uuid) に必要
    target_compile_options(nokomo2 PRIVATE -fms-extensions)
endif()

add_executable(nkmconv tools/nkmconv.cpp)
target_link_libraries(nkmconv PRIVATE nokomo2)
target_include_directories(nkmconv PRIVATE kpisdk lib)
target_compile_definitions(nkmconv PRIVATE UNICODE _UNICODE NOMINMAX)
if(MSVC)
    target_compile_options(nkmconv PRIVATE /utf-8 /W4)
else()
    target_compile_options(nkmconv PRIVATE -fms-extensions)
    target_link_options(nkmconv PRIVATE -municode)
endif()
