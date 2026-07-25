-- Luminaria — a minimal Wayland compositor library.
--
-- Built as a C++23 *module*: `import luminaria;` is the whole interface. The
-- interface partitions live in include/luminaria/**.cppm (one per former public
-- header) and are marked public so dependent targets can import them.
--
-- xmake rather than Meson because Meson's module dependency scanner still emits
-- MSVC-shaped `.ifc` outputs and cannot drive GCC's modules at all.

set_project("luminaria")
set_version("0.0.1")
set_languages("c11", "c++23")
set_policy("build.c++.modules", true)

add_rules("mode.debug", "mode.release")
set_defaultmode("debug")

-- Partial designated-init of libwayland's C *_interface vtables is idiomatic;
-- unlisted request slots are intentionally null. Silence that one warning, keep
-- everything else fatal.
set_warnings("all", "extra", "pedantic", "error")
add_cxxflags("-Wno-missing-field-initializers")

-- We build ON libwayland-server; the wire protocol is never reimplemented.
local pkgs = {"wayland-server", "wayland-client", "vulkan", "xkbcommon", "xcb",
              "libdrm", "libinput", "libudev", "gbm", "libseat"}
for _, name in ipairs(pkgs) do
    add_requires("pkgconfig::" .. name, {alias = (name:gsub("-", "_"))})
end
local pkg_aliases = {}
for _, name in ipairs(pkgs) do
    table.insert(pkg_aliases, (name:gsub("-", "_")))
end

-- ---------------------------------------------------------------- code generation
--
-- wayland-scanner turns protocol XML into C glue, and glslangValidator compiles
-- the renderer's shaders into SPIR-V arrays that get embedded in the binary —
-- nothing is loaded from disk at runtime. Both run at configure time into a
-- single generated/ directory, and only when the output is older than its input.

-- ---------------------------------------------------------------------- library

target("luminaria")
    set_kind("static")
    -- Interface partitions: public so anything depending on us can import them.
    add_files("include/luminaria.cppm", "include/luminaria/**.cppm", {public = true})
    -- for detail/wayland_fwd.h, which the interface units include in their GMF
    add_includedirs("include", {public = true})
    add_files("src/**.cpp")
    add_packages(pkg_aliases, {public = true})
    on_load(function (target)
        -- os.iorunv is only exposed in script scope, so the generator lives
        -- here rather than next to the tables above.
        import("core.base.option")

        local function tool_path(pkg, var)
            local out = os.iorunv("pkg-config", {"--variable=" .. var, pkg})
            return (out:gsub("%s+$", ""))
        end
        local function stale(src, dst)
            return not os.isfile(dst) or os.mtime(src) > os.mtime(dst)
        end
        local function run(prog, argv, dst)
            os.mkdir(path.directory(dst))
            os.iorunv(prog, argv)
        end

        -- Protocols we generate glue for. `kinds` picks which halves are wanted:
        -- "s" server header, "c" private code, "l" client header (tests act as clients).
        local protocols = {
            {"xdg-shell",                     "stable/xdg-shell/xdg-shell.xml",                             "scl"},
            {"linux-dmabuf-unstable-v1",      "unstable/linux-dmabuf/linux-dmabuf-unstable-v1.xml",         "scl"},
            {"xdg-decoration-unstable-v1",    "unstable/xdg-decoration/xdg-decoration-unstable-v1.xml",     "scl"},
            {"viewporter",                    "stable/viewporter/viewporter.xml",                           "scl"},
            {"fractional-scale-v1",           "staging/fractional-scale/fractional-scale-v1.xml",           "scl"},
            {"xdg-output-unstable-v1",        "unstable/xdg-output/xdg-output-unstable-v1.xml",             "scl"},
            {"primary-selection-unstable-v1", "unstable/primary-selection/primary-selection-unstable-v1.xml", "scl"},
            {"single-pixel-buffer-v1",        "staging/single-pixel-buffer/single-pixel-buffer-v1.xml",     "scl"},
            {"presentation-time",             "stable/presentation-time/presentation-time.xml",             "scl"},
            {"tearing-control-v1",            "staging/tearing-control/tearing-control-v1.xml",             "scl"},
            {"cursor-shape-v1",               "staging/cursor-shape/cursor-shape-v1.xml",                   "scl"},
            -- cursor-shape references tablet-v2 interfaces; the generated code needs them.
            {"tablet-v2",                     "stable/tablet/tablet-v2.xml",                                "sc"},
            {"ext-workspace-v1",              "staging/ext-workspace/ext-workspace-v1.xml",                 "scl"},
            {"linux-drm-syncobj-v1",          "staging/linux-drm-syncobj/linux-drm-syncobj-v1.xml",         "scl"},
            {"xdg-activation-v1",             "staging/xdg-activation/xdg-activation-v1.xml",               "scl"},
        }
        -- Protocols upstream does not ship; the XML lives in protocol/.
        local local_protocols = {
            {"wlr-screencopy-unstable-v1",   "sc"},
            {"ext-image-copy-capture-v1",    "sc"},
            {"ext-image-capture-source-v1",  "sc"},
            {"wlr-layer-shell-unstable-v1",  "scl"},
            {"wlr-foreign-toplevel-management-unstable-v1", "scl"},
        }

        local gendir = path.join(os.projectdir(), "build", "generated")
        local scanner = tool_path("wayland-scanner", "wayland_scanner")
        local protodir = tool_path("wayland-protocols", "pkgdatadir")
        local sources = {}

        local function scan(name, xml, kinds)
            if kinds:find("s") then
                local dst = path.join(gendir, name .. "-protocol.h")
                if stale(xml, dst) then run(scanner, {"server-header", xml, dst}, dst) end
            end
            if kinds:find("l") then
                local dst = path.join(gendir, name .. "-client-protocol.h")
                if stale(xml, dst) then run(scanner, {"client-header", xml, dst}, dst) end
            end
            if kinds:find("c") then
                local dst = path.join(gendir, name .. "-protocol.c")
                if stale(xml, dst) then run(scanner, {"private-code", xml, dst}, dst) end
                table.insert(sources, dst)
            end
        end

        os.mkdir(gendir)
        for _, p in ipairs(protocols) do
            scan(p[1], path.join(protodir, p[2]), p[3])
        end
        for _, p in ipairs(local_protocols) do
            scan(p[1], path.join(os.projectdir(), "protocol", p[1] .. ".xml"), p[2])
        end

        -- Shaders: --vn emits the SPIR-V as a C array, so the pipeline is
        -- compiled into the binary and there is no shader file to ship.
        for _, shader in ipairs({{"quad.vert", "kQuadVertSpv", "quad_vert_spv.h"},
                                 {"quad.frag", "kQuadFragSpv", "quad_frag_spv.h"}}) do
            local src = path.join(os.projectdir(), "src", "render", shader[1])
            local dst = path.join(gendir, shader[3])
            if stale(src, dst) then
                run("glslangValidator", {"-V", "--vn", shader[2], "-o", dst, src}, dst)
            end
        end

        target:add("files", sources)
        target:add("includedirs", gendir, {public = true})
    end)

-- --------------------------------------------------------------------- examples

target("tinyluminaria")
    set_kind("binary")
    add_files("examples/tinyluminaria.cpp")
    add_deps("luminaria")

target("luminaria-drm-demo")
    set_kind("binary")
    add_files("examples/drm_demo.cpp")
    add_deps("luminaria")

target("luminaria-tty")
    set_kind("binary")
    add_files("examples/tty_compositor.cpp")
    add_deps("luminaria")

-- ------------------------------------------------------------------------ tests
--
-- One binary per file, same as before. Several skip themselves with exit 77 when
-- the machine has no GPU / no free VT / no seat, so that counts as a pass.

for _, file in ipairs(os.files("tests/test_*.cpp")) do
    local name = path.basename(file)
    target(name)
        set_kind("binary")
        set_default(false)
        add_files(file)
        add_deps("luminaria")
        add_tests("default")
        -- xmake has no notion of a skipped test, so implement the exit-77
        -- convention here: a test that cannot run (no GPU, no free VT, no seat)
        -- says why on stderr and is not a failure.
        on_test(function (target, opt)
            local code = os.execv(target:targetfile(), {}, {try = true, curdir = os.projectdir()})
            if code == 77 then
                cprint("${color.warning}skipped${clear} %s (see the reason above)", target:name())
                return true
            end
            return code == 0
        end)
end
