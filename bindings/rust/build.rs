//! Two compilers, one link.
//!
//! 1. cxx-build expands the `#[cxx::bridge]` in src/lib.rs into lib.rs.h and
//!    lib.rs.cc, and compiles the .cc with `cc`. `cc` cannot build C++
//!    modules, which is fine: the glue only includes luminaria_rs.h.
//! 2. xmake builds luminaria and luminaria_rs.cpp (the only module importer),
//!    with lib.rs.h on its include path from step 1.
//! 3. Everything is linked here, with the system libraries from pkg-config.
//!
//! Both halves must use the same C++ standard library, or a `std::unique_ptr`
//! crossing the bridge has two layouts. Default: clang for both, which on
//! Linux means libstdc++ for both. `LUMINARIA_TOOLCHAIN=gcc` switches both.
//!
//! `LUMINARIA_SANITIZE=address` (or `undefined`) instruments both halves and
//! links the runtime. The link must then be done by the same compiler family:
//!
//!   LUMINARIA_SANITIZE=address RUSTFLAGS="-C linker=clang" cargo run --example floating

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

    // ---------------------------------------------------------------- 1. glue
    let mut glue = cxx_build::bridge("src/lib.rs");
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
    // Dependents before dependencies: the shim needs luminaria.desktop and
    // luminaria.gpu, which both need luminaria.
    for lib in [
        "luminaria-rs",
        "luminaria-desktop",
        "luminaria-gpu",
        "luminaria",
    ] {
        println!("cargo:rustc-link-lib=static={lib}");
    }
    for pkg in [
        "wayland-server",
        "wayland-client",
        "xkbcommon",
        "vulkan",
        "libdrm",
        "libinput",
        "libudev",
        "gbm",
        "libseat",
    ] {
        pkg_config::Config::new()
            .probe(pkg)
            .unwrap_or_else(|e| panic!("pkg-config {pkg}: {e}"));
    }

    for path in [
        "luminaria_rs.h",
        "luminaria_rs.cpp",
        "xmake.lua",
        "../../src",
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
