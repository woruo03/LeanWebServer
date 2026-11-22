# tests/CMakeLists.txt

# 1. 优先使用系统安装的 GoogleTest
find_package(GTest CONFIG QUIET)
if(NOT TARGET GTest::gtest AND NOT TARGET GTest::GTest)
    find_package(GTest QUIET)
endif()

# 2. 如果未检测到，回退使用 FetchContent 自动下载构建
if(NOT TARGET GTest::gtest AND NOT TARGET GTest::GTest)
    message(
        STATUS
        "GoogleTest not found on system. Fetching via FetchContent..."
    )
    include(FetchContent)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.18.0
    )
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)

    # 隔离第三方依赖，避免 clang-tidy 扫描 FetchContent 源码
    set(_SAVED_CMAKE_CXX_CLANG_TIDY "${CMAKE_CXX_CLANG_TIDY}")
    set(CMAKE_CXX_CLANG_TIDY "")
    FetchContent_MakeAvailable(googletest)
    set(CMAKE_CXX_CLANG_TIDY "${_SAVED_CMAKE_CXX_CLANG_TIDY}")
endif()

# 3. 确定 GTest 目标标识
if(TARGET GTest::gtest_main)
    set(GTEST_MAIN_TARGET GTest::gtest_main)
elseif(TARGET GTest::Main)
    set(GTEST_MAIN_TARGET GTest::Main)
else()
    set(GTEST_MAIN_TARGET gtest_main)
endif()

# 4. 辅助函数：定义单元测试目标并注册到 CTest
include(GoogleTest)

function(lean_add_test target src)
    add_executable(${target} ${src})
    target_link_libraries(
        ${target}
        PRIVATE
            LeanWebServer::core
            LeanWebServer::warnings
            LeanWebServer::sanitizers
            Threads::Threads
            ${GTEST_MAIN_TARGET}
    )
    target_compile_features(${target} PRIVATE cxx_std_17)
    target_compile_definitions(${target} PRIVATE LEAN_WEB_SERVER_TESTING=1)
    gtest_discover_tests(${target})
endfunction()

lean_add_test(tests_base   test_timestamp.cpp)
lean_add_test(tests_logger test_logger.cpp)
lean_add_test(tests_net    test_timer.cpp)
lean_add_test(tests_http   test_integration_timeout.cpp)
lean_add_test(tests_config test_server_config.cpp)
