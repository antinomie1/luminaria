//! `Display::as_ptr()`: a global registered on luminaria's own `wl_display`
//! through libwayland-server's C API sits beside luminaria's globals, a
//! libwayland client sees and binds it, and the teardown order CLAUDE.md asks
//! of protocol globals holds for it too — the global dies before the Display,
//! and the client's resource, still connected, dies inside the Display's drop.
//!
//! Both halves are hand-written `extern "C"`, the shape `wayland-sys` gives
//! you; the crate already links libwayland-server and libwayland-client.

use std::ffi::{CStr, CString, c_char, c_int, c_void};
use std::ptr;
use std::sync::Arc;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::mpsc;
use std::time::{Duration, Instant};

use luminaria::core::Display;
use luminaria::protocol::{Compositor, Seat};

#[repr(C)]
struct WlInterface {
    name: *const c_char,
    version: c_int,
    method_count: c_int,
    methods: *const c_void,
    event_count: c_int,
    events: *const c_void,
}

struct Interface(WlInterface);
// SAFETY: immutable, and its pointers are 'static.
unsafe impl Sync for Interface {}

/// The trivial extra protocol: one interface, no requests, no events.
static EXTRA: Interface = Interface(WlInterface {
    name: c"lum_test_extra".as_ptr(),
    version: 1,
    method_count: 0,
    methods: ptr::null(),
    event_count: 0,
    events: ptr::null(),
});

type BindFn = unsafe extern "C" fn(client: *mut c_void, data: *mut c_void, version: u32, id: u32);
type ResourceDestroyFn = unsafe extern "C" fn(resource: *mut c_void);

// libwayland-server
unsafe extern "C" {
    fn wl_global_create(
        display: *mut c_void,
        interface: *const WlInterface,
        version: c_int,
        data: *mut c_void,
        bind: BindFn,
    ) -> *mut c_void;
    fn wl_global_destroy(global: *mut c_void);
    fn wl_resource_create(
        client: *mut c_void,
        interface: *const WlInterface,
        version: c_int,
        id: u32,
    ) -> *mut c_void;
    fn wl_resource_set_implementation(
        resource: *mut c_void,
        implementation: *const c_void,
        data: *mut c_void,
        destroy: Option<ResourceDestroyFn>,
    );
    fn wl_resource_get_user_data(resource: *mut c_void) -> *mut c_void;
}

// libwayland-client
unsafe extern "C" {
    static wl_registry_interface: WlInterface;
    fn wl_display_connect(name: *const c_char) -> *mut c_void;
    fn wl_display_roundtrip(display: *mut c_void) -> c_int;
    fn wl_display_disconnect(display: *mut c_void);
    fn wl_proxy_marshal_flags(
        proxy: *mut c_void,
        opcode: u32,
        interface: *const WlInterface,
        version: u32,
        flags: u32,
        ...
    ) -> *mut c_void;
    fn wl_proxy_add_listener(
        proxy: *mut c_void,
        implementation: *const c_void,
        data: *mut c_void,
    ) -> c_int;
    fn wl_proxy_get_version(proxy: *mut c_void) -> u32;
}

// ---------------------------------------------------------------- server half

/// What the global's callbacks may touch. The global's user data points here.
struct ExtraState {
    binds: AtomicUsize,
    /// Counted by resources' destroy handlers. Each resource holds its own
    /// `Arc`, because it outlives this state: a still-connected client's
    /// resources are destroyed inside the Display's drop.
    destroyed: Arc<AtomicUsize>,
}

/// Owns the global. Declared after the `Display`, so it drops first.
struct ExtraGlobal {
    global: *mut c_void,
    state: Box<ExtraState>,
}

unsafe extern "C" fn extra_bind(client: *mut c_void, data: *mut c_void, version: u32, id: u32) {
    // SAFETY: `data` is the ExtraState the global was created with, and the
    // global is destroyed before that state is.
    let state = unsafe { &*(data as *const ExtraState) };
    state.binds.fetch_add(1, Ordering::SeqCst);
    let resource = unsafe { wl_resource_create(client, &EXTRA.0, version as c_int, id) };
    assert!(!resource.is_null(), "wl_resource_create");
    let counter = Arc::into_raw(state.destroyed.clone()) as *mut c_void;
    // No requests, so no vtable to fill; libwayland only reads it for opcodes.
    unsafe { wl_resource_set_implementation(resource, ptr::null(), counter, Some(extra_destroy)) };
}

unsafe extern "C" fn extra_destroy(resource: *mut c_void) {
    // SAFETY: the user data is the Arc leaked in `extra_bind`, taken back once.
    let counter =
        unsafe { Arc::from_raw(wl_resource_get_user_data(resource) as *const AtomicUsize) };
    counter.fetch_add(1, Ordering::SeqCst);
}

impl ExtraGlobal {
    fn new(display: &Display, destroyed: Arc<AtomicUsize>) -> Self {
        let state = Box::new(ExtraState {
            binds: AtomicUsize::new(0),
            destroyed,
        });
        let data = &*state as *const ExtraState as *mut c_void;
        // SAFETY: the display outlives this global (see the declaration order
        // in the test), and so does `state`, which the Box keeps in place.
        let global = unsafe { wl_global_create(display.as_ptr(), &EXTRA.0, 1, data, extra_bind) };
        assert!(!global.is_null(), "wl_global_create");
        Self { global, state }
    }
}

impl Drop for ExtraGlobal {
    fn drop(&mut self) {
        // Before `state` drops, so no bind can reach freed memory.
        unsafe { wl_global_destroy(self.global) };
    }
}

// ---------------------------------------------------------------- client half

#[repr(C)]
struct RegistryListener {
    global: unsafe extern "C" fn(*mut c_void, *mut c_void, u32, *const c_char, u32),
    global_remove: unsafe extern "C" fn(*mut c_void, *mut c_void, u32),
}

/// (name, interface, version) for every global the registry announced.
type Globals = Vec<(u32, String, u32)>;

unsafe extern "C" fn on_global(
    data: *mut c_void,
    _registry: *mut c_void,
    name: u32,
    interface: *const c_char,
    version: u32,
) {
    let globals = unsafe { &mut *(data as *mut Globals) };
    let interface = unsafe { CStr::from_ptr(interface) }
        .to_string_lossy()
        .into_owned();
    globals.push((name, interface, version));
}

unsafe extern "C" fn on_global_remove(_data: *mut c_void, _registry: *mut c_void, _name: u32) {}

static REGISTRY_LISTENER: RegistryListener = RegistryListener {
    global: on_global,
    global_remove: on_global_remove,
};

/// Connect, list the globals, bind the extra one, and report; then stay
/// connected until told to go, so the server tears down under a live client.
fn client(socket: String, report: mpsc::Sender<Globals>, go: mpsc::Receiver<()>) {
    let name = CString::new(socket).unwrap();
    unsafe {
        let display = wl_display_connect(name.as_ptr());
        assert!(!display.is_null(), "wl_display_connect");
        // wl_display_get_registry, which is an inline function in the header.
        let registry = wl_proxy_marshal_flags(
            display,
            1,
            &raw const wl_registry_interface,
            wl_proxy_get_version(display),
            0,
            ptr::null_mut::<c_void>(),
        );
        let mut globals: Globals = Vec::new();
        wl_proxy_add_listener(
            registry,
            &REGISTRY_LISTENER as *const RegistryListener as *const c_void,
            &mut globals as *mut Globals as *mut c_void,
        );
        assert!(wl_display_roundtrip(display) >= 0);

        let extra = globals
            .iter()
            .find(|(_, interface, _)| interface == "lum_test_extra")
            .map(|&(name, _, _)| name);
        if let Some(name) = extra {
            // wl_registry_bind, likewise inline.
            let bound = wl_proxy_marshal_flags(
                registry,
                0,
                &EXTRA.0,
                1,
                0,
                name,
                EXTRA.0.name,
                1u32,
                ptr::null_mut::<c_void>(),
            );
            assert!(!bound.is_null());
            assert!(wl_display_roundtrip(display) >= 0);
        }
        report.send(globals).unwrap();
        let _ = go.recv();
        wl_display_disconnect(display);
    }
}

#[test]
fn extra_global_through_raw_display() {
    let runtime = std::env::temp_dir().join("luminaria_test_raw_display_runtime");
    let _ = std::fs::create_dir_all(&runtime);
    unsafe {
        std::env::set_var("XDG_RUNTIME_DIR", &runtime);
    }

    let destroyed = Arc::new(AtomicUsize::new(0));
    let display = Display::new().expect("display");
    assert!(!display.as_ptr().is_null());
    let socket = display.add_socket_auto().expect("socket");
    let compositor = Compositor::new(&display).expect("wl_compositor");
    let seat = Seat::new(&display, "seat0").expect("wl_seat");
    // After the Display, so it is destroyed before it.
    let extra = ExtraGlobal::new(&display, destroyed.clone());

    let (report_tx, report_rx) = mpsc::channel();
    let (go_tx, go_rx) = mpsc::channel();
    let client = std::thread::spawn(move || client(socket, report_tx, go_rx));

    let deadline = Instant::now() + Duration::from_secs(10);
    let globals = loop {
        display
            .dispatch(Some(Duration::from_millis(10)))
            .expect("dispatch");
        display.flush_clients();
        if let Ok(globals) = report_rx.try_recv() {
            break globals;
        }
        assert!(Instant::now() < deadline, "client never reported");
    };

    // The raw global coexists with luminaria's own.
    let has = |name: &str| globals.iter().any(|(_, interface, _)| interface == name);
    assert!(
        has("lum_test_extra"),
        "client did not see the extra global: {globals:?}"
    );
    assert!(has("wl_compositor"), "{globals:?}");
    assert!(has("wl_seat"), "{globals:?}");
    assert_eq!(extra.state.binds.load(Ordering::SeqCst), 1);
    assert_eq!(destroyed.load(Ordering::SeqCst), 0);

    // Teardown with the client still connected and its resource still bound:
    // the global goes first, then luminaria's, then the Display — which
    // destroys the client and, with it, the resource whose handler must not
    // touch the global's (freed) state.
    drop(extra);
    display
        .dispatch(Some(Duration::ZERO))
        .expect("dispatch after global destroy");
    drop(seat);
    drop(compositor);
    drop(display);
    assert_eq!(
        destroyed.load(Ordering::SeqCst),
        1,
        "resource destroy handler did not run"
    );

    go_tx.send(()).unwrap();
    client.join().unwrap();
}
