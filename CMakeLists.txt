#
# HexEditor - 基于 SOUI4 的跨平台十六进制编辑器
#
# 依赖的 SOUI4 源码位于 soui4/ 子目录（git submodule 或手动克隆）。
#
# 构建（Windows, VS 2022）:
#   cmake -S . -B build -G "Visual Studio 17 2022" -A x64
#   cmake --build build --config Release
#
# 构建（单配置生成器，如 Ninja / Makefiles）:
#   cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
#   cmake --build build
#
cmake_minimum_required(VERSION 3.16)
project(HexEditor LANGUAGES C CXX)

# ---------------------------------------------------------------------------
# SOUI4 源目录
# ---------------------------------------------------------------------------
if(NOT DEFINED SOUI4_ROOT)
    set(SOUI4_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/soui4)
endif()
if(NOT EXISTS ${SOUI4_ROOT}/CMakeLists.txt)
    message(FATAL_ERROR "未找到 SOUI4 源码 / SOUI4 sources not found: ${SOUI4_ROOT}\n"
            "请先克隆 SOUI4（二选一，内容相同）/ Clone SOUI4 first (either source, same code):\n"
            "  Outside China: git clone --depth 1 https://github.com/soui4/soui.git ${SOUI4_ROOT}\n"
            "  Inside China:  git clone --depth 1 https://gitee.com/setoutsoft/soui4.git ${SOUI4_ROOT}\n"
            "或用 -DSOUI4_ROOT=<path> 指向已有的 SOUI4 源码目录 / Or pass -DSOUI4_ROOT=<path> to an existing SOUI4 tree.")
endif()

# ---------------------------------------------------------------------------
# 通过 add_subdirectory 引入 SOUI4，只构建核心与必要依赖
# 这些 option 必须在 add_subdirectory 之前设置
# ---------------------------------------------------------------------------
# 与 SOUI4 保持一致：内部字符串默认用 wchar_t，且按 Unicode 构建
# （add_definitions 在父作用域设置，可同时被子目录 socki 与本工程继承）
add_definitions(-DUNICODE -D_UNICODE)
set(SOUI_WCHAR_AS_DEFAULT ON CACHE BOOL "" FORCE)
set(SOUI_USE_UNICODE ON CACHE BOOL "" FORCE)

set(SOUI_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(SOUI_BUILD_TOOLS  OFF CACHE BOOL "" FORCE)
set(SOUI_BUILD_GAMES  OFF CACHE BOOL "" FORCE)
set(SOUI_BUILD_HTTPCLIENT OFF CACHE BOOL "" FORCE)
set(SOUI_BUILD_WS OFF CACHE BOOL "" FORCE)
set(SOUI_ENABLE_SPY OFF CACHE BOOL "" FORCE)

add_subdirectory(${SOUI4_ROOT} ${CMAKE_BINARY_DIR}/soui4)

# ---------------------------------------------------------------------------
# 主程序
# ---------------------------------------------------------------------------
set(HEX_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src)

# CONFIGURE_DEPENDS：新增 src/*.cpp / *.h 时自动重新 configure，
# 否则新文件不会进构建（表现为 LNK2019 无法解析的外部符号）。
file(GLOB HEX_HEADERS CONFIGURE_DEPENDS ${HEX_SRC_DIR}/*.h)
file(GLOB HEX_SOURCES CONFIGURE_DEPENDS ${HEX_SRC_DIR}/*.cpp)

source_group("Header Files" FILES ${HEX_HEADERS})
source_group("Source Files" FILES ${HEX_SOURCES})

# 头文件依赖（重要）：Makefile 生成器下，把 .h 放进 add_executable 并不构成编译依赖。
# 改了头（尤其是增删类成员）后只有 include 链上被判定的少数 .cpp 重编，其他 TU 仍用旧的
# sizeof(类) 分配对象 —— 构造函数按新布局写成员就越界写堆，表现为随机 0xC0000374
# （时好时坏，加日志/改代码即可"消失"）。这里强制本工程所有源码依赖所有头文件，
# 改头即全量重编本工程（文件数很少，代价可接受）。
set_source_files_properties(${HEX_SOURCES} PROPERTIES OBJECT_DEPENDS "${HEX_HEADERS}")

if(WIN32)
    set(HEX_APP_OPTION WIN32)
else()
    set(HEX_APP_OPTION "")
endif()

add_executable(HexEditor ${HEX_APP_OPTION} ${HEX_HEADERS} ${HEX_SOURCES})
if(WIN32)
    # 可执行文件图标（资源管理器显示用）
    target_sources(HexEditor PRIVATE ${HEX_SRC_DIR}/HexEditor.rc)
    # 应用清单由 HexEditor.rc 里的 RT_MANIFEST 提供（声明 supportedOS），
    # 所以关掉链接器自己生成清单，避免两份清单互相覆盖。
    target_link_options(HexEditor PRIVATE "/MANIFEST:NO")
endif()

# 与 SOUI4 各动态库输出到同一目录，运行期可直接加载
set(_HEX_OUT_DIR ${CMAKE_BINARY_DIR}/bin)
# 本工程需要 C++17：HexFileModel 用 std::shared_ptr<u8[]> 的下标运算符（C++17 起才提供）。
# MSVC / libstdc++ 在 C++11 模式下宽容放行，libc++（macOS/clang）严格按标准会直接报错。
set_target_properties(HexEditor PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${_HEX_OUT_DIR}
    RUNTIME_OUTPUT_DIRECTORY_RELEASE ${_HEX_OUT_DIR}
    RUNTIME_OUTPUT_DIRECTORY_DEBUG ${_HEX_OUT_DIR}
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO ${_HEX_OUT_DIR}
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

target_include_directories(HexEditor PRIVATE
    ${HEX_SRC_DIR}
    ${SOUI4_ROOT}/SOUI/include
    ${SOUI4_ROOT}/utilities/include
    ${SOUI4_ROOT}/components
    ${SOUI4_ROOT}/controls.extend
)
# 非 Windows：souistd.h 会 #include <windows.h>/<tchar.h>，这些由 swinx 提供。
# soui4 顶层用 include_directories() 加的 swinx/include 只在它自己的目录作用域生效，
# 不会传播到本工程（父作用域）的 target，所以这里必须显式补上，否则应用编译报
# "windows.h: No such file or directory"。Windows 下用系统 SDK，不需要。
if(NOT WIN32)
    target_include_directories(HexEditor PRIVATE ${SOUI4_ROOT}/swinx/include)
    # 非 Windows 的 HashRange 用 OpenSSL EVP 计算 MD5/SHA-1/SHA-256
    #（Windows 用系统 CNG bcrypt）。Debian/Ubuntu: libssl-dev；macOS: brew install openssl
    find_package(OpenSSL REQUIRED)
    target_link_libraries(HexEditor PRIVATE OpenSSL::Crypto)
endif()

target_link_libraries(HexEditor PRIVATE soui4)

if(MSVC)
    target_compile_options(HexEditor PRIVATE /utf-8)
else()
    target_compile_options(HexEditor PRIVATE -fpermissive)
endif()

add_dependencies(HexEditor soui-sys-resource)

# 运行期依赖（渲染器 / 图像解码器等动态库）
foreach(_dep skia render-gdi render-skia imgdecoder-stb imgdecoder-wic imgdecoder-gdip soui-sys-resource)
    if(TARGET ${_dep})
        add_dependencies(HexEditor ${_dep})
    endif()
endforeach()

# ---------------------------------------------------------------------------
# 运行期资源：把 uires / soui-sys-resource 拷到可执行文件目录
# ---------------------------------------------------------------------------
if(WIN32)
    # 系统资源用 DLL 形式发布（非 Windows 用 zip，见 main.cpp）
    add_custom_command(TARGET HexEditor POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:soui-sys-resource>
            $<TARGET_FILE_DIR:HexEditor>/
        COMMENT "copy soui-sys-resource.dll")
endif()

if(NOT WIN32)
    # 非 Windows 平台系统资源用 zip 形式发布
    add_custom_command(TARGET HexEditor POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${SOUI4_ROOT}/soui-sys-resource.zip
            $<TARGET_FILE_DIR:HexEditor>/
        COMMENT "copy soui-sys-resource.zip")
endif()

# 应用资源（uires 目录）：整目录同步。
# 不用 file(GLOB) 生成文件列表——新增资源（如 layout/find.xml）不会被旧
# 列表收录，运行期会因找不到 XML 而出现黑窗。copy_directory 每次构建全量同步，
# 新增/修改资源都无需重新 configure。
set(HEX_UIRES_DIR ${CMAKE_CURRENT_SOURCE_DIR}/uires)
if(EXISTS ${HEX_UIRES_DIR}/uires.idx)
    add_custom_target(hexeditor-uires ALL
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${HEX_UIRES_DIR} ${_HEX_OUT_DIR}/uires
        COMMENT "sync uires")
    add_dependencies(HexEditor hexeditor-uires)
endif()

# ---------------------------------------------------------------------------
# 模型单元测试（不依赖 UI）：cmake --build . --target hexeditor-test 后运行
# ---------------------------------------------------------------------------
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tools/test_model.cpp)
add_executable(hexeditor-test tools/test_model.cpp src/HexFileModel.cpp)
target_include_directories(hexeditor-test PRIVATE ${HEX_SRC_DIR})
if(NOT WIN32)
    target_include_directories(hexeditor-test PRIVATE ${SOUI4_ROOT}/swinx/include)
endif()
if(MSVC)
    target_compile_options(hexeditor-test PRIVATE /utf-8)
endif()
set_target_properties(hexeditor-test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${_HEX_OUT_DIR}
    RUNTIME_OUTPUT_DIRECTORY_RELEASE ${_HEX_OUT_DIR}
    RUNTIME_OUTPUT_DIRECTORY_DEBUG ${_HEX_OUT_DIR}
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON)
else()
    message(STATUS "tools/test_model.cpp not found, skipping the hexeditor-test target")
endif()
