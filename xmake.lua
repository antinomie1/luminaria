-- Luminaria — a minimal Wayland compositor library.
--
-- Built as four C++23 named modules: the dependency-light `luminaria` core and
-- opt-in `luminaria.gpu`, `luminaria.desktop`, `luminaria.xwayland` extensions.
-- Every partition lives in src/**/*.cppm — one file per concept, holding both
-- interface and implementation. There is no public include/ tree.
--
-- xmake rather than Meson because Meson's module dependency scanner still emits
-- MSVC-shaped `.ifc` outputs and cannot drive GCC's modules at all.

set_project("luminaria")
set_version("0.0.1")
set_languages("c11", "c++23")
set_policy("build.c++.modules", true)

add_rules("mode.debug", "mode.release")
set_defaultmode("debug")

-- Sanitizers, off by default (both are slow, and switching rebuilds every
-- module unit from scratch):
--
--   xmake f --sanitize=address --toolchain=clang && xmake build -a && xmake test
--   xmake f --sanitize=undefined --toolchain=clang && ...
--
-- Worth knowing what ASan does and does NOT buy here. It catches use-after-free
-- and heap overruns — the dangling-Surface* class, which is why
-- tests/test_dnd_surface_destroy.cpp is worth running under it. It does NOT
-- catch a read or write past the end of an mmap'd region, which is exactly what
-- a client's short buffer stride produces; ASan only shadows heap, stack and
-- globals. That class is covered by tests/test_buffer_bounds.cpp and
-- tests/test_screencopy_bounds.cpp, which fault or trip a canary by
-- construction rather than relying on a sanitizer.
--
-- LeakSanitizer (on by default with ASan) reports wl_proxy allocations made by
-- the in-process test clients, which disconnect without tearing every proxy
-- down. Those are harness artefacts, not library defects, so tests/lsan.supp
-- suppresses libwayland-client and nothing else:
--
--   LSAN_OPTIONS=suppressions=tests/lsan.supp xmake test
option("sanitize")
    set_default("none")
    set_values("none", "address", "undefined")
    set_description("Build with a sanitizer (address|undefined)")
option_end()

if get_config("sanitize") == "address" then
    set_policy("build.sanitizer.address", true)
elseif get_config("sanitize") == "undefined" then
    set_policy("build.sanitizer.undefined", true)
end

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
local base_packages = {"wayland_server", "wayland_client", "xkbcommon"}
local gpu_packages = {"vulkan", "libdrm", "libinput", "libudev", "gbm", "libseat"}

-- ---------------------------------------------------------------- code generation
--
-- wayland-scanner turns protocol XML into C glue, and glslangValidator compiles
-- the renderer's shaders into SPIR-V arrays that get embedded in the binary —
-- nothing is loaded from disk at runtime. Both run at configure time into a
-- single generated/ directory, and only when the output is older than its input.

-- ---------------------------------------------------------------------- library

target("luminaria")
    set_kind("static")
    add_files("src/luminaria.cppm", "src/core/**.cppm", "src/util/**.cppm", {public = true})
    add_files("src/backend/backend.cppm", "src/backend/headless.cppm",
              "src/backend/input_event.cppm", "src/backend/output.cppm",
              "src/backend/wayland.cppm", {public = true})
    add_files("src/protocol/client_buffer.cppm", "src/protocol/compositor.cppm",
              "src/protocol/cursor_shape.cppm", "src/protocol/data_device.cppm",
              "src/protocol/fractional_scale.cppm", "src/protocol/idle_inhibit.cppm",
              "src/protocol/idle_notify.cppm", "src/protocol/layer_shell.cppm",
              "src/protocol/output_global.cppm", "src/protocol/pointer_constraints.cppm",
              "src/protocol/presentation_time.cppm", "src/protocol/relative_pointer.cppm",
              "src/protocol/seat.cppm", "src/protocol/single_pixel_buffer.cppm",
              "src/protocol/subcompositor.cppm", "src/protocol/tearing_control.cppm",
              "src/protocol/text_input.cppm", "src/protocol/viewporter.cppm",
              "src/protocol/xdg_activation.cppm", "src/protocol/xdg_decoration.cppm",
              "src/protocol/xdg_shell.cppm", {public = true})
    add_files("src/render/cursor_theme.cppm", "src/shell/output_layout.cppm", {public = true})
    -- for detail/wayland_fwd.h, included in the units' GMF
    add_includedirs("src", {public = true})
    add_packages(base_packages, {public = true})
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
            {"relative-pointer-unstable-v1",  "unstable/relative-pointer/relative-pointer-unstable-v1.xml", "scl"},
            {"pointer-constraints-unstable-v1", "unstable/pointer-constraints/pointer-constraints-unstable-v1.xml", "scl"},
            {"text-input-unstable-v3",        "unstable/text-input/text-input-unstable-v3.xml",             "scl"},
            {"idle-inhibit-unstable-v1",      "unstable/idle-inhibit/idle-inhibit-unstable-v1.xml",         "scl"},
            {"ext-idle-notify-v1",            "staging/ext-idle-notify/ext-idle-notify-v1.xml",             "scl"},
        }
        -- Protocols upstream does not ship; the XML lives in protocol/.
        local local_protocols = {
            {"wlr-screencopy-unstable-v1",   "sc"},
            {"ext-image-copy-capture-v1",    "scl"},
            {"ext-image-capture-source-v1",  "scl"},
            {"wlr-layer-shell-unstable-v1",  "scl"},
            {"wlr-foreign-toplevel-management-unstable-v1", "scl"},
            {"wlr-data-control-unstable-v1", "scl"},
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

        target:add("files", sources)
        target:add("includedirs", gendir, {public = true})
    end)

target("luminaria-gpu")
    set_kind("static")
    add_deps("luminaria")
    add_files("src/luminaria.gpu.cppm", "src/backend/drm.cppm",
              "src/backend/libinput.cppm", "src/backend/session.cppm",
              "src/protocol/drm_syncobj.cppm", "src/protocol/linux_dmabuf.cppm",
              "src/protocol/screencopy.cppm", "src/render/vulkan.cppm",
              "src/shell/direct_scanout.cppm", "src/shell/frame.cppm", {public = true})
    add_includedirs("src", "build/generated", {public = true})
    add_packages(gpu_packages, {public = true})
    on_load(function (target)
        local function stale(src, dst)
            return not os.isfile(dst) or os.mtime(src) > os.mtime(dst)
        end
        for _, shader in ipairs({{"quad.vert", "kQuadVertSpv", "quad_vert_spv.h"},
                                 {"quad.frag", "kQuadFragSpv", "quad_frag_spv.h"}}) do
            local src = path.join(os.projectdir(), "src", "render", shader[1])
            local dst = path.join(os.projectdir(), "build", "generated", shader[3])
            if stale(src, dst) then
                os.mkdir(path.directory(dst))
                os.iorunv("glslangValidator", {"-V", "--vn", shader[2], "-o", dst, src})
            end
        end
    end)

target("luminaria-desktop")
    set_kind("static")
    add_deps("luminaria")
    add_files("src/luminaria.desktop.cppm", "src/protocol/data_control.cppm",
              "src/protocol/foreign_toplevel.cppm", "src/protocol/workspace.cppm",
              {public = true})

target("luminaria-xwayland")
    set_kind("static")
    add_deps("luminaria")
    add_files("src/xwayland/xwayland.cppm", {public = true})
    add_packages("xcb", {public = true})

-- --------------------------------------------------------------------- examples

target("tinyluminaria")
    set_kind("binary")
    add_files("examples/tinyluminaria.cpp")
    add_deps("luminaria-gpu", "luminaria-desktop")

target("luminaria-drm-demo")
    set_kind("binary")
    add_files("examples/drm_demo.cpp")
    add_deps("luminaria-gpu")

target("luminaria-tty")
    set_kind("binary")
    add_files("examples/tty_compositor.cpp")
    add_deps("luminaria-gpu")

-- ------------------------------------------------------------------------ tests
--
-- One binary per file, same as before. Several skip themselves with exit 77 when
-- the machine has no GPU / no free VT / no seat, so that counts as a pass.

local gpu_tests = {
    test_client_texture = true, test_composite = true, test_cursor_capture = true,
    test_direct_scanout = true, test_dmabuf = true, test_drm = true, test_frame = true,
    test_gpu_scanout = true, test_libinput = true, test_render_output = true,
    test_screencopy_bounds = true, test_session = true, test_syncobj = true,
    test_texture = true, test_texture_cache = true, test_vulkan = true,
    test_wayland_nested = true,
}
local desktop_tests = {
    test_data_control = true, test_foreign_toplevel = true, test_workspace = true,
}

for _, file in ipairs(os.files("tests/test_*.cpp")) do
    local name = path.basename(file)
    target(name)
        set_kind("binary")
        set_default(false)
        add_files(file)
        if gpu_tests[name] then
            add_deps("luminaria-gpu")
        elseif desktop_tests[name] then
            add_deps("luminaria-desktop")
        elseif name == "test_xwayland" then
            add_deps("luminaria-xwayland")
        else
            add_deps("luminaria")
        end
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
