// Compiles the C++ shim and links librpitx.
//
// On a normal cross-compile the buildroot package sets:
//   - the C++ cross compiler via the standard `${TARGET_TRIPLE}_CXX` env var
//     (`cc` crate honours it), or via CXX as a fallback.
//   - LIBRPITX_INCLUDE_DIR / LIBRPITX_LIB_DIR pointing into the staging dir.
//
// For a host build (running cargo by hand on the Pi itself, or testing on
// a Pi-like sysroot), we fall back to the standard system locations.

use std::env;

fn main() {
    println!("cargo:rerun-if-changed=rpitx-shim/shim.cpp");
    println!("cargo:rerun-if-changed=rpitx-shim/shim.h");
    println!("cargo:rerun-if-env-changed=LIBRPITX_INCLUDE_DIR");
    println!("cargo:rerun-if-env-changed=LIBRPITX_LIB_DIR");

    let mut build = cc::Build::new();
    build
        .cpp(true)
        .file("rpitx-shim/shim.cpp")
        .include("rpitx-shim")
        .flag_if_supported("-std=c++11")
        .flag_if_supported("-Wno-write-strings")
        .flag_if_supported("-Wno-unused-variable");

    if let Ok(inc) = env::var("LIBRPITX_INCLUDE_DIR") {
        build.include(inc);
    }
    build.compile("rpitx_shim");

    if let Ok(libdir) = env::var("LIBRPITX_LIB_DIR") {
        println!("cargo:rustc-link-search=native={}", libdir);
    }
    // Link librpitx (static), then its transitive deps.
    println!("cargo:rustc-link-lib=static=rpitx");
    println!("cargo:rustc-link-lib=dylib=stdc++");
    println!("cargo:rustc-link-lib=dylib=m");
    println!("cargo:rustc-link-lib=dylib=rt");
    println!("cargo:rustc-link-lib=dylib=pthread");
}
