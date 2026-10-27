//! The backend bridge: `import luminaria.backend;` on the C++ side.
//!
//! Cargo feature `backend`. Everything here needs libdrm, libinput, libudev, or
//! libseat — the libraries `luminaria.backend` brings with it. A consumer that
//! leaves the feature off never compiles this bridge, never builds
//! `luminaria_rs_backend.cpp` and never links them.

#[cxx::bridge(namespace = "lum")]
pub(crate) mod ffi {
    unsafe extern "C++" {
        include!("luminaria/luminaria_rs.h");
        include!("luminaria/luminaria_rs_backend.h");
        include!("luminaria/src/ffi.rs.h");

        // Core types this bridge's signatures mention.
        type DisplayShim = crate::ffi::DisplayShim;
        type OutputShim = crate::ffi::OutputShim;
        type SeatShim = crate::ffi::SeatShim;

        // Session (libseat)
        type SessionShim;
        fn session_new(display: &DisplayShim, error: &mut String) -> UniquePtr<SessionShim>;
        fn switch_to(self: &SessionShim, vt: i32, error: &mut String) -> bool;
        fn switch_vt_for(self: &SessionShim, keysym: u32) -> bool;
        fn active(self: &SessionShim) -> bool;
        fn seat_name(self: &SessionShim) -> String;

        // DRM / KMS
        type DrmBackendShim;
        fn drm_backend_new(
            display: &DisplayShim,
            device: &str,
            session: &SessionShim,
            error: &mut String,
        ) -> UniquePtr<DrmBackendShim>;
        fn drm_backend_new_standalone(
            display: &DisplayShim,
            device: &str,
            error: &mut String,
        ) -> UniquePtr<DrmBackendShim>;
        fn start(self: Pin<&mut DrmBackendShim>, error: &mut String) -> bool;
        fn fd(self: &DrmBackendShim) -> i32;
        fn scan_connectors(self: Pin<&mut DrmBackendShim>);

        // Libinput
        type LibinputBackendShim;
        fn libinput_backend_new(
            display: &DisplayShim,
            session: &SessionShim,
            error: &mut String,
        ) -> UniquePtr<LibinputBackendShim>;
        fn libinput_backend_new_standalone(
            display: &DisplayShim,
            error: &mut String,
        ) -> UniquePtr<LibinputBackendShim>;
        fn libinput_backend_new_path(
            display: &DisplayShim,
            session: &SessionShim,
            error: &mut String,
        ) -> UniquePtr<LibinputBackendShim>;
        fn libinput_backend_new_path_standalone(
            display: &DisplayShim,
            error: &mut String,
        ) -> UniquePtr<LibinputBackendShim>;
        fn start(self: Pin<&mut LibinputBackendShim>, error: &mut String) -> bool;
        fn add_device(self: Pin<&mut LibinputBackendShim>, path: &str, error: &mut String) -> bool;
        fn keymap(self: &LibinputBackendShim) -> String;
        fn follow_seat(self: &LibinputBackendShim, seat: &SeatShim, touch: bool);
        fn set_keymap(self: Pin<&mut LibinputBackendShim>, xkb_text: &str) -> bool;
        fn capabilities(self: &LibinputBackendShim) -> u32;

        // Headless
        #[allow(dead_code)]
        type HeadlessBackendShim;
        #[allow(dead_code)]
        fn headless_backend_new(
            display: &DisplayShim,
            interval_ms: u32,
            error: &mut String,
        ) -> UniquePtr<HeadlessBackendShim>;
        #[allow(dead_code)]
        fn add_output(
            self: Pin<&mut HeadlessBackendShim>,
            width: i32,
            height: i32,
        ) -> UniquePtr<OutputShim>;
        #[allow(dead_code)]
        fn start(self: Pin<&mut HeadlessBackendShim>, error: &mut String) -> bool;

        // Wayland (nested)
        #[allow(dead_code)]
        type WaylandBackendShim;
        #[allow(dead_code)]
        fn wayland_backend_new(
            display: &DisplayShim,
            error: &mut String,
        ) -> UniquePtr<WaylandBackendShim>;
        #[allow(dead_code)]
        fn add_output(
            self: Pin<&mut WaylandBackendShim>,
            width: i32,
            height: i32,
        ) -> UniquePtr<OutputShim>;
        #[allow(dead_code)]
        fn start(self: Pin<&mut WaylandBackendShim>, error: &mut String) -> bool;
        #[allow(dead_code)]
        fn keymap(self: &WaylandBackendShim) -> String;
        #[allow(dead_code)]
        fn capabilities(self: &WaylandBackendShim) -> u32;
        #[allow(dead_code)]
        fn follow_seat(self: &WaylandBackendShim, seat: &SeatShim, touch: bool);
    }
}
