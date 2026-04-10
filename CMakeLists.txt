# 设置CMake的最小版本要求
cmake_minimum_required(VERSION 3.10)

# 设置项目名称
project(MyReactorProject)

# 设置C++标准
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

find_package(PkgConfig REQUIRED)
pkg_check_modules(MYSQLCLIENT REQUIRED mysqlclient)

# 添加可执行文件
add_executable(main main.cpp reactor.cpp Logger.cpp)

# 头文件搜索路径
target_include_directories(main PRIVATE ${MYSQLCLIENT_INCLUDE_DIRS})

# 如果需要链接额外的库，可以在这里添加
target_link_libraries(main ${MYSQLCLIENT_LIBRARIES})