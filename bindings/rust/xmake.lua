-- The C++ half of the Rust binding: luminaria itself plus the shim .cpp files.
--
-- build.rs drives this, not a human: it runs cxx-build first (so the generated
-- bridge headers and rust/cxx.h exist), then configures this project with
-- --cxxbridge pointing at them and --modules naming the C++ modules the enabled
-- cargo features asked for, then links the archives it leaves behind.
-- luminaria is pulled in with includes(), which makes it non-standalone: only
-- its library targets are declared, and the build modes are ours to set.
set_project("luminaria-rs")
add_rules("mode.debug", "mode.release")
set_defaultmode("release")

option("cxxbridge")
    set_default("")
    set_showmenu(true)
    set_description("cxx-build's $OUT_DIR/cxxbridge (holds rust/cxx.h and the bridge headers)")
option_end()

-- Which C++ modules to build, comma separated: "core", "core,gpu",
-- "core,gpu,desktop", ... The shim target adds the matching luminaria targets
-- and shim .cpp files, so a core-only build never compiles or links the GPU or
-- desktop shims. build.rs derives it from the cargo features (ADR 0003).
option("modules")
    set_default("core")
    set_showmenu(true)
    set_description("C++ modules to build: core,gpu,desktop,text")
option_end()

-- LUMINARIA_SANITIZE=address|undefined, passed down by build.rs. Root scope on
-- purpose, before includes(): it has to reach luminaria's own targets too.
option("sanitize")
    set_default("none")
    set_values("none", "address", "undefined")
    set_showmenu(true)
option_end()
if get_config("sanitize") == "address" then
    set_policy("build.sanitizer.address", true)
elseif get_config("sanitize") == "undefined" then
    set_policy("build.sanitizer.undefined", true)
end

includes("../..")

target("luminaria-rs")
    set_kind("static")
    set_languages("c11", "c++23")
    set_policy("build.c++.modules", true)
    -- Defined in luminaria's xmake.lua: deletes objects older than the module
    -- units they import, which xmake's own incremental build can miss.
    add_rules("luminaria.module_staleness")
    -- The same warning set luminaria builds with, minus the vtable idiom.
    set_warnings("all", "extra", "pedantic", "error")
    add_cxxflags("-Wno-missing-field-initializers")
    add_options("cxxbridge", "modules")

    local enabled = {}
    for module in (get_config("modules") or "core"):gmatch("[^,]+") do
        enabled[module] = true
    end

    add_deps("luminaria")
    add_files(path.join(os.scriptdir(), "luminaria_rs.cpp"))
    if enabled["gpu"] then
        add_deps("luminaria-gpu")
        add_files(path.join(os.scriptdir(), "luminaria_rs_gpu.cpp"))
    end
    if enabled["desktop"] then
        add_deps("luminaria-desktop")
        add_files(path.join(os.scriptdir(), "luminaria_rs_desktop.cpp"))
    end
    if enabled["text"] then
        add_deps("luminaria-text")
        add_files(path.join(os.scriptdir(), "luminaria_rs_text.cpp"))
    end

    -- xmake writes .deps/<target>/<plat>/<arch>/<mode>/<file>.d while scanning
    -- the shim's imports, but on a brand-new build tree it does not create the
    -- directory first: the first build of a fresh clone dies with "save … failed,
    -- cannot open file". Make it at configure time, before any build job runs.
    on_load(function (target)
        local mode_dir = target:targetdir()
        local root = path.directory(path.directory(path.directory(mode_dir)))
        os.mkdir(path.join(root, ".deps", target:name(),
                           path.basename(path.directory(path.directory(mode_dir))),
                           path.basename(path.directory(mode_dir)),
                           path.basename(mode_dir)))
    end)

    local bridge = get_config("cxxbridge")
    if bridge and bridge ~= "" then
        -- include/ has rust/cxx.h and luminaria/src/*.rs.h; crate/ maps
        -- luminaria/ onto this directory, for luminaria/luminaria_rs*.h.
        add_includedirs(path.join(bridge, "include"), path.join(bridge, "crate"))
    end
