cmake_minimum_required(VERSION 3.25)

project(rinha_backend_2026_cpp LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

enable_testing()

find_package(ZLIB REQUIRED)
find_package(Threads REQUIRED)

file(GLOB USOCKETS_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/uWebSockets/uSockets/src/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/uWebSockets/uSockets/src/eventing/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/uWebSockets/uSockets/src/crypto/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/uWebSockets/uSockets/src/io_uring/*.c"
)

add_library(usockets STATIC ${USOCKETS_SOURCES})
target_include_directories(usockets
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/uWebSockets/uSockets/src"
)
target_compile_definitions(usockets PUBLIC LIBUS_NO_SSL)
target_link_libraries(usockets PUBLIC Threads::Threads ${CMAKE_DL_LIBS})

add_library(simdjson_singleheader STATIC
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/simdjson/singleheader/simdjson.cpp"
)
target_include_directories(simdjson_singleheader
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/simdjson/singleheader"
)
target_compile_definitions(simdjson_singleheader PUBLIC SIMDJSON_EXCEPTIONS=0)

add_executable(rinha-backend-2026-cpp
    src/main.cpp
    src/request.cpp
    src/vectorize.cpp
    src/refs.cpp
    src/ivf.cpp
    src/classifier.cpp
)
target_include_directories(rinha-backend-2026-cpp
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/uWebSockets/src"
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/uWebSockets/uSockets/src"
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/simdjson/singleheader"
)
target_compile_definitions(rinha-backend-2026-cpp
    PRIVATE
        LIBUS_NO_SSL
        UWS_NO_ZLIB
        SIMDJSON_EXCEPTIONS=0
)
target_link_libraries(rinha-backend-2026-cpp
    PRIVATE
        simdjson_singleheader
        usockets
        ZLIB::ZLIB
        Threads::Threads
        ${CMAKE_DL_LIBS}
)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(rinha-backend-2026-cpp PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
        -fno-rtti
    )
endif()

if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
    target_compile_options(rinha-backend-2026-cpp PRIVATE -mavx2 -mfma -march=x86-64-v3)
endif()

include(CheckIPOSupported)
check_ipo_supported(RESULT RINHA_IPO_SUPPORTED OUTPUT RINHA_IPO_ERROR LANGUAGES CXX)
if(RINHA_IPO_SUPPORTED)
    set_property(TARGET rinha-backend-2026-cpp PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()

add_executable(prepare-refs-cpp
    src/prepare_refs.cpp
    src/refs.cpp
)
target_include_directories(prepare-refs-cpp
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/simdjson/singleheader"
)
target_compile_definitions(prepare-refs-cpp
    PRIVATE
        SIMDJSON_EXCEPTIONS=0
)
target_link_libraries(prepare-refs-cpp
    PRIVATE
        simdjson_singleheader
        ZLIB::ZLIB
)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(prepare-refs-cpp PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
    )
endif()

if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
    target_compile_options(prepare-refs-cpp PRIVATE -march=x86-64-v3)
endif()

add_executable(prepare-ivf-cpp EXCLUDE_FROM_ALL
    tools/prepare_ivf.cpp
    src/ivf.cpp
)
target_include_directories(prepare-ivf-cpp
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
target_link_libraries(prepare-ivf-cpp
    PRIVATE
        ZLIB::ZLIB
)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(prepare-ivf-cpp PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
    )
endif()

if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
    target_compile_options(prepare-ivf-cpp PRIVATE -mavx2 -march=x86-64-v3)
endif()

add_executable(rinha-backend-2026-cpp-tests
    tests/core_tests.cpp
    src/request.cpp
    src/vectorize.cpp
    src/refs.cpp
    src/classifier.cpp
)
target_include_directories(rinha-backend-2026-cpp-tests
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/simdjson/singleheader"
)
target_compile_definitions(rinha-backend-2026-cpp-tests
    PRIVATE
        SIMDJSON_EXCEPTIONS=0
        RINHA_REPO_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/.."
)
target_link_libraries(rinha-backend-2026-cpp-tests
    PRIVATE
        simdjson_singleheader
        ZLIB::ZLIB
)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(rinha-backend-2026-cpp-tests PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
    )
endif()

if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
    target_compile_options(rinha-backend-2026-cpp-tests PRIVATE -mavx2 -mfma -march=x86-64-v3)
endif()

add_test(
    NAME rinha-backend-2026-cpp-tests
    COMMAND rinha-backend-2026-cpp-tests
)

add_executable(benchmark-classifier-cpp EXCLUDE_FROM_ALL
    tools/benchmark_classifier.cpp
    src/refs.cpp
)
target_include_directories(benchmark-classifier-cpp
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/simdjson/singleheader"
)
target_compile_definitions(benchmark-classifier-cpp
    PRIVATE
        SIMDJSON_EXCEPTIONS=0
)
target_link_libraries(benchmark-classifier-cpp
    PRIVATE
        simdjson_singleheader
        ZLIB::ZLIB
)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(benchmark-classifier-cpp PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
    )
endif()

if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
    target_compile_options(benchmark-classifier-cpp PRIVATE -mavx2 -mfma -march=x86-64-v3)
endif()

add_executable(benchmark-request-cpp EXCLUDE_FROM_ALL
    tools/benchmark_request.cpp
    src/request.cpp
    src/vectorize.cpp
    src/refs.cpp
    src/classifier.cpp
)
target_include_directories(benchmark-request-cpp
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/simdjson/singleheader"
)
target_compile_definitions(benchmark-request-cpp
    PRIVATE
        SIMDJSON_EXCEPTIONS=0
)
target_link_libraries(benchmark-request-cpp
    PRIVATE
        simdjson_singleheader
        ZLIB::ZLIB
)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(benchmark-request-cpp PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
    )
endif()

if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
    target_compile_options(benchmark-request-cpp PRIVATE -mavx2 -mfma -march=x86-64-v3)
endif()

add_executable(benchmark-ivf-cpp EXCLUDE_FROM_ALL
    tools/benchmark_ivf.cpp
    src/request.cpp
    src/vectorize.cpp
    src/ivf.cpp
)
target_include_directories(benchmark-ivf-cpp
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/simdjson/singleheader"
)
target_compile_definitions(benchmark-ivf-cpp
    PRIVATE
        SIMDJSON_EXCEPTIONS=0
        RINHA_IVF_STATS=1
)
target_link_libraries(benchmark-ivf-cpp
    PRIVATE
        simdjson_singleheader
        ZLIB::ZLIB
)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(benchmark-ivf-cpp PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
    )
endif()

if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
    target_compile_options(benchmark-ivf-cpp PRIVATE -mavx2 -mfma -march=x86-64-v3)
endif()

add_executable(benchmark-kernel-cpp EXCLUDE_FROM_ALL
    tools/benchmark_kernel.cpp
    src/refs.cpp
)
target_include_directories(benchmark-kernel-cpp
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/simdjson/singleheader"
)
target_compile_definitions(benchmark-kernel-cpp
    PRIVATE
        SIMDJSON_EXCEPTIONS=0
)
target_link_libraries(benchmark-kernel-cpp
    PRIVATE
        simdjson_singleheader
        ZLIB::ZLIB
)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(benchmark-kernel-cpp PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
    )
endif()

if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
    target_compile_options(benchmark-kernel-cpp PRIVATE -mavx2 -mfma -march=x86-64-v3)
endif()
