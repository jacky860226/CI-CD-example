cmake_minimum_required(VERSION 3.15)
project(MyProject VERSION 1.0 LANGUAGES CXX)

# 設定 C++ 標準
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Ofast")

# 建立 Library Target (核心邏輯)
add_library(memory_check SHARED src/MemoryCheck/mem_simple.cpp)
install(TARGETS memory_check DESTINATION lib)
add_library(core_lib src/Account.cpp src/ScopeTimer.cpp)
# 設定 Include 路徑 (PUBLIC 表示連結此 lib 的人也能看到 include)
target_include_directories(core_lib PUBLIC include)

# 建立 Executable Target (執行檔)
add_executable(app src/main.cpp)

# 連結 Library (app 使用 core_lib)
target_link_libraries(app PRIVATE core_lib)
target_link_libraries(app PRIVATE memory_check)

# find_package(TBB REQUIRED)
# target_link_libraries(app PRIVATE TBB::tbb)
target_link_libraries(app PRIVATE tbb)

# 1. 使用 FetchContent 自動下載 GoogleTest
include(FetchContent)
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.14.0
)
# 為 Windows 使用者防呆（Linux 可忽略，但建議加上）
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)

# 2. 使 GTest 目標可用
FetchContent_MakeAvailable(googletest)

# 3. 定義你的測試執行檔
# gmock_main 會自動提供 main() 進入點，不需自寫 main
add_executable(test_app tests/gtest_core.cpp)
target_link_libraries(test_app PRIVATE core_lib GTest::gmock_main)

# 連結 backtrace library (如果需要)
target_link_libraries(app PRIVATE stdc++_libbacktrace)