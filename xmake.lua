set_languages("cxx17")

add_rules("mode.debug", "mode.release")

add_requires("cpr", "nlohmann_json")
add_requires("ixwebsocket v12.0.1")  -- NapCat 适配器：反向 WebSocket 服务端（收事件/发 API）
if is_arch("arm64") or is_plat("cross") then
    add_requires("sqlite3")
else
    add_requires("sqlite3", {system = true})  -- 记忆系统：BLOB 向量存储 + SQL 元数据预过滤
end

target("MIO")
    set_kind("binary")
    -- **.cpp 递归匹配所有子目录的源文件（旧的 src/*.cpp 漏掉了子目录）
    add_files("src/**.cpp")
    -- 统一以 src/ 为 include 根，代码里写 #include "providers/llm/Llm.h"
    add_includedirs("src")
    add_packages("cpr", "nlohmann_json", "ixwebsocket", "sqlite3")
--
-- If you want to known more usage about xmake, please see https://xmake.io
--
-- ## FAQ
--
-- You can enter the project directory firstly before building project.
--
--   $ cd projectdir
--
-- 1. How to build project?
--
--   $ xmake
--
-- 2. How to configure project?
--
--   $ xmake f -p [macosx|linux|iphoneos ..] -a [x86_64|i386|arm64 ..] -m [debug|release]
--
-- 3. Where is the build output directory?
--
--   The default output directory is `./build` and you can configure the output directory.
--
--   $ xmake f -o outputdir
--   $ xmake
--
-- 4. How to run and debug target after building project?
--
--   $ xmake run [targetname]
--   $ xmake run -d [targetname]
--
-- 5. How to install target to the system directory or other output directory?
--
--   $ xmake install
--   $ xmake install -o installdir
--
-- 6. Add some frequently-used compilation flags in xmake.lua
--
-- @code
--    -- add debug and release modes
--    add_rules("mode.debug", "mode.release")
--
--    -- add macro definition
--    add_defines("NDEBUG", "_GNU_SOURCE=1")
--
--    -- set warning all as error
--    set_warnings("all", "error")
--
--    -- set language: c99, c++11
--    set_languages("c99", "c++11")
--
--    -- set optimization: none, faster, fastest, smallest
--    set_optimize("fastest")
--
--    -- add include search directories
--    add_includedirs("/usr/include", "/usr/local/include")
--
--    -- add link libraries and search directories
--    add_links("tbox")
--    add_linkdirs("/usr/local/lib", "/usr/lib")
--
--    -- add system link libraries
--    add_syslinks("z", "pthread")
--
--    -- add compilation and link flags
--    add_cxflags("-stdnolib", "-fno-strict-aliasing")
--    add_ldflags("-L/usr/local/lib", "-lpthread", {force = true})
--
-- @endcode
--

