-- The C++ half of the Rust binding: luminaria itself plus luminaria_rs.cpp.
--
-- build.rs drives this, not a human: it runs cxx-build first (so the generated
-- lib.rs.h and rust/cxx.h exist), then configures this project with
-- --cxxbridge pointing at them, then links the archives it leaves behind.
-- luminaria is pulled in with includes(), which makes it non-standalone: only
-- its four library targets are declared, and the build modes are ours to set.
set_project("luminaria-rs")
add_rules("mode.debug", "mode.release")
set_defaultmode("release")

option("cxxbridge")
    set_default("")
    set_showmenu(true)
    set_description("cxx-build's $OUT_DIR/cxxbridge (holds rust/cxx.h and lib.rs.h)")
option_end()

includes("../..")

target("luminaria-rs")
    set_kind("static")
    set_languages("c11", "c++23")
    set_policy("build.c++.modules", true)
    -- The same warning set luminaria builds with, minus the vtable idiom.
    set_warnings("all", "extra", "pedantic", "error")
    add_cxxflags("-Wno-missing-field-initializers")
    add_deps("luminaria-gpu")
    add_options("cxxbridge")
    add_files("luminaria_rs.cpp")
    local bridge = get_config("cxxbridge")
    if bridge and bridge ~= "" then
        -- include/ has rust/cxx.h and luminaria/src/lib.rs.h; crate/ maps
        -- luminaria/ onto this directory, for luminaria/luminaria_rs.h.
        add_includedirs(path.join(bridge, "include"), path.join(bridge, "crate"))
    end
