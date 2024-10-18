# 设置 CMake 最低版本要求
cmake_minimum_required(VERSION 3.10)

# 设置项目名称和版本
project(workflow VERSION 1.0 LANGUAGES C CXX)

# 设置 C++ 标准
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# 使用 ccache
set(CMAKE_CXX_COMPILER_LAUNCHER ccache)

# 添加包含目录
include_directories(
    ${PROJECT_SOURCE_DIR}/src/kernel
    # ${PROJECT_SOURCE_DIR}/src/protocol
    # ${PROJECT_SOURCE_DIR}/src/util
    # ${PROJECT_SOURCE_DIR}/src/manager
    # ${PROJECT_SOURCE_DIR}/src/server
    # ${PROJECT_SOURCE_DIR}/src/factory
)

# 查找 pthread 库
find_package(Threads REQUIRED)
# find_package(fmt REQUIRED)
# find_package(spdlog REQUIRED)
# find_package(OpenSSL REQUIRED)
# 添加源文件
file(GLOB SRC ${PROJECT_SOURCE_DIR}/src/kernel/*.cc 
    ${PROJECT_SOURCE_DIR}/src/kernel/*.c
    # ${PROJECT_SOURCE_DIR}/src/protocol/*.cc
    # ${PROJECT_SOURCE_DIR}/src/protocol/*.c
    # ${PROJECT_SOURCE_DIR}/src/util/*.cc
    # ${PROJECT_SOURCE_DIR}/src/util/*.c
    # ${PROJECT_SOURCE_DIR}/src/manager/*.cc
    # ${PROJECT_SOURCE_DIR}/src/manager/*.c
    # ${PROJECT_SOURCE_DIR}/src/server/*.cc
    # ${PROJECT_SOURCE_DIR}/src/server/*.c
    # ${PROJECT_SOURCE_DIR}/src/factory/*.cc
    # ${PROJECT_SOURCE_DIR}/src/factory/*.c
)
set(LIBRARIES
    ${CMAKE_THREAD_LIBS_INIT}
    # fmt::fmt
    # spdlog::spdlog
    # OpenSSL::SSL
    # OpenSSL::Crypto
)

# 设置输出路径
set(EXECUTABLE_OUTPUT_PATH ${PROJECT_SOURCE_DIR}/bin)
set(LIBRARY_OUTPUT_PATH ${PROJECT_SOURCE_DIR}/lib)

# 创建静态库
add_library(workflow STATIC ${SRC})
# add_library(workflow STATIC ${SRC})

# 添加编译选项
target_compile_options(workflow PRIVATE -rdynamic -O0 -ggdb -Wall -Wno-deprecated -Werror -Wno-unused-function )

# 创建可执行文件
# add_executable(test_thrdpool ${PROJECT_SOURCE_DIR}/test/test_thrdpool.cc)
# target_link_libraries(test_thrdpool ${LIBRARIES} workflow)

# add_executable(test_httpparser ${PROJECT_SOURCE_DIR}/test/test_httpparser.cc)
# target_link_libraries(test_httpparser ${LIBRARIES} workflow)

# add_executable(test_http_echo_server ${PROJECT_SOURCE_DIR}/test/http_echo_server.cc)
# target_link_libraries(test_http_echo_server ${LIBRARIES} workflow)