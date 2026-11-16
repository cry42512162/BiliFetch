cmake_minimum_required(VERSION 3.21)

project(BiliFetch VERSION 1.0.0 LANGUAGES CXX)

enable_language(RC)   # 资源编译（图标 / 版本信息，MinGW windres）
# windres 预处理器处理不了带空格的 Qt include 路径与 C++ 宏，RC 只需源文件本身
set(CMAKE_RC_COMPILE_OBJECT "<CMAKE_RC_COMPILER> -O coff <SOURCE> -o <OBJECT>")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets)

qt_add_executable(BiliFetch WIN32
    src/main.cpp
    src/BridgeClient.h
    src/BridgeClient.cpp
    src/LoginDialog.h
    src/LoginDialog.cpp
    src/MainWindow.h
    src/MainWindow.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/app.rc"
)

# 图标/版本资源模板：注入本机绝对路径后供 windres 编译（app.rc.in → build/app.rc）
set(APP_ICON "${CMAKE_SOURCE_DIR}/assets/app.ico")
configure_file(assets/app.rc.in "${CMAKE_CURRENT_BINARY_DIR}/app.rc" @ONLY)

target_link_libraries(BiliFetch PRIVATE Qt6::Widgets)

# bridge 脚本源码目录回退路径（exe 旁没有时使用）
target_compile_definitions(BiliFetch PRIVATE
    BILI_BRIDGE_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

# 运行时把 bridge 脚本放到可执行文件旁，便于打包分发
add_custom_command(TARGET BiliFetch POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/bridge/bili_bridge.py"
        "$<TARGET_FILE_DIR:BiliFetch>/bili_bridge.py"
    COMMENT "Copy bili_bridge.py next to BiliFetch.exe"
)
