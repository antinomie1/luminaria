//! Two compilers, one link, and only the modules the features asked for.
//!
//! 1. cxx-build expands the enabled `#[cxx::bridge]`es (src/ffi.rs, and one per
//!    C++ extension module) into headers and .cc glue, and compiles the glue
//!    with `cc`. `cc` cannot build C++ modules, which is fine: the glue only
//!    includes the plain shim headers.
//! 2. xmake builds luminaria and the shim .cpp files for those same modules,
//!    with the generated headers on its include path from step 1.
//! 3. Everything is linked here, with the system libraries the enabled modules
//!    actually need. A core-only build (`--no-default-features`) never names
//!    Vulkan, libdrm, libinput, libudev, gbm or libseat.
//!
//! Both halves must use the same C++ standard library, or a `std::unique_ptr`
//! crossing the bridge has two layouts. Default: clang for both, which on
//! Linux means libstdc++ for both. `LUMINARIA_TOOLCHAIN=gcc` switches both.
//!
//! `LUMINARIA_SANITIZE=address` (or `undefined`) instruments both halves and
//! links the runtime. The link must then be done by the same compiler family:
//!
//!   LUMINARIA_SANITIZE=address RUSTFLAGS="-C linker=clang" cargo run --example minimal

use std::env;
use std::path::{Path, PathBuf};
use std::process::Command;

fn main() {
    let manifest = PathBuf::from(env::var("CARGO_MANIFEST_DIR").unwrap());
    let out = PathBuf::from(env::var("OUT_DIR").unwrap());
    let toolchain = env::var("LUMINARIA_TOOLCHAIN").unwrap_or_else(|_| "clang".into());
    let cxx = if toolchain == "gcc" { "g++" } else { "clang++" };
    let mode = match env::var("LUMINARIA_XMAKE_MODE") {
        Ok(mode) => mode,
        Err(_) if env::var("PROFILE").as_deref() == Ok("release") => "release".into(),
        Err(_) => "debug".into(),
    };
    let sanitize = env::var("LUMINARIA_SANITIZE").unwrap_or_else(|_| "none".into());
    assert!(
        ["none", "address", "undefined"].contains(&sanitize.as_str()),
        "LUMINARIA_SANITIZE must be none, address or undefined"
    );

    // Which C++ modules the enabled cargo features ask for (ADR 0008). The core
    // module is always there. `shell` implies `backend` and `render` through the
    // feature table, and `gpu` is the historical name for `shell`, so cargo sets
    // all three for a default build.
    let backend = env::var_os("CARGO_FEATURE_BACKEND").is_some();
    let render = env::var_os("CARGO_FEATURE_RENDER").is_some();
    let shell = env::var_os("CARGO_FEATURE_SHELL").is_some();
    let desktop = env::var_os("CARGO_FEATURE_DESKTOP").is_some();
    let text = env::var_os("CARGO_FEATURE_TEXT").is_some();

    // ------------------------------------------------------------- 0. keysyms
    // Every XKB_KEY_* the system's xkbcommon defines, as `Keysym` constants.
    keysyms(&out);

    // ---------------------------------------------------------------- 1. glue
    let mut bridges = vec!["src/ffi.rs"];
    if backend {
        bridges.push("src/ffi_backend.rs");
    }
    if render {
        bridges.push("src/ffi_render.rs");
    }
    if shell {
        bridges.push("src/ffi_shell.rs");
    }
    if desktop {
        bridges.push("src/ffi_desktop.rs");
    }
    if text {
        bridges.push("src/ffi_text.rs");
    }
    let mut glue = cxx_build::bridges(&bridges);
    glue.compiler(cxx).std("c++20");
    if sanitize != "none" {
        glue.flag(format!("-fsanitize={sanitize}"));
        println!("cargo:rustc-link-arg=-fsanitize={sanitize}");
    }
    glue.compile("luminaria-cxxbridge");

    // --------------------------------------------------------------- 2. xmake
    let bridge = out.join("cxxbridge");
    let build_dir = out.join("xmake-build");
    // Keep xmake's own state out of the source tree, and per profile.
    let config_dir = out.join("xmake-config");
    let xmake = |args: &[&str]| {
        let status = Command::new("xmake")
            .args(args)
            .current_dir(&manifest)
            .env("XMAKE_CONFIGDIR", &config_dir)
            .status()
            .unwrap_or_else(|e| panic!("cannot run xmake (is it installed?): {e}"));
        assert!(status.success(), "xmake {} failed", args.join(" "));
    };
    let mut modules = vec!["core"];
    if backend {
        modules.push("backend");
    }
    if render {
        modules.push("render");
    }
    if shell {
        modules.push("shell");
    }
    if desktop {
        modules.push("desktop");
    }
    if text {
        modules.push("text");
    }
    let modules_arg = format!("--modules={}", modules.join(","));
    xmake(&[
        "f",
        "-y",
        "-P",
        ".",
        &format!("--toolchain={toolchain}"),
        &format!("--mode={mode}"),
        "-o",
        build_dir.to_str().expect("OUT_DIR is not UTF-8"),
        &format!("--cxxbridge={}", bridge.display()),
        &format!("--sanitize={sanitize}"),
        &modules_arg,
    ]);
    let jobs = env::var("NUM_JOBS").unwrap_or_else(|_| "0".into());
    let mut build = vec!["build", "-y", "-P", "."];
    let jobs_arg = format!("--jobs={jobs}");
    if jobs != "0" {
        build.push(&jobs_arg);
    }
    build.push("luminaria-rs");
    xmake(&build);

    // ---------------------------------------------------------------- 3. link
    let libdir = find_archive_dir(&build_dir, "libluminaria-rs.a")
        .expect("xmake built luminaria-rs but its archive is nowhere under the build dir");
    println!("cargo:rustc-link-search=native={}", libdir.display());
    // Dependents before dependencies, and only the modules that were built.
    // Dependents before dependencies.
    let mut libs = vec!["luminaria-rs"];
    if shell {
        libs.push("luminaria-shell");
    }
    if desktop {
        libs.push("luminaria-desktop");
    }
    if render {
        libs.push("luminaria-render");
    }
    if backend {
        libs.push("luminaria-backend");
    }
    libs.push("luminaria");
    if text {
        libs.push("luminaria-text");
    }
    for lib in libs {
        println!("cargo:rustc-link-lib=static={lib}");
    }

    // Only what the enabled modules drag in. luminaria itself needs wayland and
    // xkbcommon; luminaria.gpu adds the whole bare-metal stack; luminaria.text
    // adds fcft and its dependencies.
    let mut pkgs = vec!["wayland-server", "wayland-client", "xkbcommon"];
    if backend {
        pkgs.extend(["libdrm", "libinput", "libudev", "libseat"]);
    }
    if render {
        pkgs.extend(["vulkan", "gbm", "libdrm"]);
    }
    if text {
        pkgs.extend(["fontconfig", "pixman-1"]);
    }
    for pkg in pkgs {
        pkg_config::Config::new()
            .probe(pkg)
            .unwrap_or_else(|e| panic!("pkg-config {pkg}: {e}"));
    }
    if text {
        // fcft's .pc `Requires.private: … tllist`, and a host without tllist's
        // .pc file (which nothing links against when fcft is shared) makes
        // pkg-config refuse the whole probe. Link the shared object directly in
        // that case; its own dependencies are DT_NEEDED entries.
        if pkg_config::Config::new().probe("fcft").is_err() {
            println!("cargo:rustc-link-lib=dylib=fcft");
        }
    }

    for path in [
        "luminaria_rs.h",
        "luminaria_rs.cpp",
        "luminaria_rs_backend.h",
        "luminaria_rs_backend.cpp",
        "luminaria_rs_render.h",
        "luminaria_rs_render.cpp",
        "luminaria_rs_shell.h",
        "luminaria_rs_shell.cpp",
        "luminaria_rs_desktop.h",
        "luminaria_rs_desktop.cpp",
        "luminaria_rs_text.h",
        "luminaria_rs_text.cpp",
        "luminaria_rs_internal.h",
        "src/lib.rs",
        "src/ffi.rs",
        "src/ffi_backend.rs",
        "src/ffi_render.rs",
        "src/ffi_shell.rs",
        "src/ffi_desktop.rs",
        "src/ffi_text.rs",
        "xmake.lua",
        // Directories are scanned recursively: any file under src/ (a .cppm,
        // a shader) or protocol/ (the XML luminaria vendors) reruns xmake.
        "../../src",
        "../../protocol",
        "../../xmake.lua",
    ] {
        println!("cargo:rerun-if-changed={path}");
    }
    for var in [
        "LUMINARIA_TOOLCHAIN",
        "LUMINARIA_XMAKE_MODE",
        "LUMINARIA_SANITIZE",
    ] {
        println!("cargo:rerun-if-env-changed={var}");
    }
    for feature in ["backend", "render", "shell", "gpu", "desktop", "text"] {
        println!("cargo:rerun-if-env-changed=CARGO_FEATURE_{}", feature.to_uppercase());
    }
}

/// `<build>/<plat>/<arch>/<mode>/`, without hard-coding the first two.
fn find_archive_dir(dir: &Path, name: &str) -> Option<PathBuf> {
    if dir.join(name).is_file() {
        return Some(dir.to_path_buf());
    }
    std::fs::read_dir(dir)
        .ok()?
        .filter_map(|e| e.ok())
        .filter(|e| e.file_type().map(|t| t.is_dir()).unwrap_or(false))
        .find_map(|e| find_archive_dir(&e.path(), name))
}

/// `util::Keysym::Return`, `Keysym::a`, `Keysym::XF86Switch_VT_1`: one
/// associated constant per `#define XKB_KEY_<name> 0x…` in the system's
/// xkbcommon-keysyms.h, so the table is the one the C++ side compiles against.
fn keysyms(out: &Path) {
    let include = pkg_config::get_variable("xkbcommon", "includedir").unwrap_or_else(|_| "/usr/include".into());
    let header = Path::new(&include).join("xkbcommon/xkbcommon-keysyms.h");
    let text = std::fs::read_to_string(&header)
        .unwrap_or_else(|e| panic!("{}: {e}", header.display()));
    println!("cargo:rerun-if-changed={}", header.display());
    let keywords = [
        "as", "break", "const", "continue", "crate", "else", "enum", "extern", "false", "fn", "for", "if",
        "impl", "in", "let", "loop", "match", "mod", "move", "mut", "pub", "ref", "return", "self", "Self",
        "static", "struct", "super", "trait", "true", "type", "unsafe", "use", "where", "while", "async",
        "await", "dyn", "abstract", "become", "box", "do", "final", "macro", "override", "priv", "typeof",
        "unsized", "virtual", "yield", "try", "gen",
    ];
    let mut rust = String::from("impl Keysym {\n");
    for line in text.lines() {
        let Some(rest) = line.strip_prefix("#define XKB_KEY_") else { continue };
        let mut parts = rest.split_whitespace();
        let (Some(name), Some(value)) = (parts.next(), parts.next()) else { continue };
        let Some(hex) = value.strip_prefix("0x") else { continue };
        let Ok(value) = u32::from_str_radix(hex, 16) else { continue };
        let ident = if name.starts_with(|c: char| c.is_ascii_digit()) {
            format!("_{name}")
        } else if keywords.contains(&name) {
            format!("r#{name}")
        } else {
            name.to_owned()
        };
        rust.push_str(&format!("    pub const {ident}: Keysym = Keysym({value:#x});\n"));
    }
    rust.push_str("}\n");
    std::fs::write(out.join("keysyms.rs"), rust).expect("write keysyms.rs");
}
