#cmake
CMAKE_MINIMUM_REQUIRED(VERSION 3.0) #cmake最低版本需求
PROJECT(WebServer)    #项目名称
set(CMAKE_C_COMPILER gcc)
set(CMAKE_CXX_COMPILER g++)
set(CMAKE_BUILD_TYPE "Debug")  #需要设置为调试模式，调试模式打断点才起效
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11 -pthread")
set(CMAKE_CXX_FLAGS_DEBUG "$ENV{CXXFLAGS} -O0 -Wall -g -ggdb")  #gdb调试
set(CMAKE_CXX_FLAGS_RELEASE "$ENV{CXXFLAGS} -O3 -Wall")
#set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -std=c++11 -g -W -Wall -Wextra -O0")
#set_target_properties(${TARGET} PROPERTIES COMPILE_FLAGS "-pthread" LINK_FLAGS "-pthread")  #强制为编译和链接增加选项-pthread（该方法不适合跨平台使用）
add_subdirectory(src)   #指明本项目包含一个子目录src
add_subdirectory(threadPool)
aux_source_directory(. MAIN_LIST) #把当前目录(.)下的所有源代码和头文件加入变量MAIN_LIST
add_executable(main ${MAIN_LIST}) #生成可执行文件main.exe
target_link_libraries(main srcLib thPoolLib)  #指明可执行文件main需要连接一个名为srcLib和一个名为miraclLib的链接库(被依赖的库要放后面)