//! Calloop event-loop integration example for luminaria.
//!
//! Demonstrates running a luminaria compositor driven by [calloop](https://crates.io/crates/calloop)
//! instead of driving `Display::dispatch()` in a bespoke while-loop.
//!
//! # Integrating with Calloop
//!
//! [`Display`](luminaria::core::Display) implements [`std::os::fd::AsFd`],
//! which exposes the file descriptor of the internal Wayland event loop (`wl_event_loop_get_fd`).
//! You can register this file descriptor directly with calloop using
//! [`calloop::generic::Generic`] and [`calloop::generic::FdWrapper`]:
//!
//! ```no_run
//! use std::os::fd::AsRawFd;
//! use std::time::Duration;
//! use calloop::{generic::{FdWrapper, Generic}, EventLoop, Interest, Mode, PostAction};
//! use luminaria::core::Display;
//!
//! let mut event_loop = EventLoop::<()>::try_new().unwrap();
//! let handle = event_loop.handle();
//! let display = Display::new().unwrap();
//!
//! // Safe: `display` outlives the event loop
//! unsafe {
//!     handle.insert_source(
//!         Generic::new(FdWrapper::new(display.as_raw_fd()), Interest::READ, Mode::Level),
//!         |_readiness, _fd, _shared| {
//!             // 1. Dispatch Wayland requests & internal timers non-blockingly:
//!             display.dispatch(Some(Duration::ZERO)).unwrap();
//!             // 2. Drain protocol signals (Compositor, XdgShell, Output, etc.):
//!             //    while let Some(ev) = compositor.next_event() { ... }
//!             // 3. Re-render/present active outputs when frames are due:
//!             //    renderer.present(...);
//!             Ok(PostAction::Continue)
//!         },
//!     ).unwrap();
//! }
//! ```
//!
//! # Integrating with Tokio
//!
//! **Important**: None of luminaria's protocol, backend, or rendering objects implement
//! [`Send`]. All objects are bound to the specific thread that owns the `wl_display`
//! and the main thread loop.
//!
//! When using Tokio:
//! 1. Use Tokio's single-threaded runtime (`tokio::runtime::Builder::new_current_thread()`)
//!    or run the compositor inside a [`tokio::task::LocalSet`].
//! 2. Wrap the display's descriptor in [`tokio::io::unix::AsyncFd`]:
//!
//! ```no_run
//! # use std::os::fd::AsFd;
//! # use std::time::Duration;
//! # use luminaria::core::Display;
//! # async fn run_tokio(display: Display) -> Result<(), Box<dyn std::error::Error>> {
//! use tokio::io::unix::AsyncFd;
//!
//! let async_fd = AsyncFd::new(display.as_fd())?;
//! loop {
//!     let mut guard = async_fd.readable().await?;
//!     // Dispatch pending requests without blocking the async executor:
//!     display.dispatch(Some(Duration::ZERO))?;
//!     // Drain protocol events here:
//!     // while let Some(ev) = compositor.next_event() { ... }
//!     guard.clear_ready();
//! }
//! # }
//! ```

use std::os::fd::AsRawFd;
use std::time::Duration;

use calloop::generic::{FdWrapper, Generic};
use calloop::timer::{TimeoutAction, Timer};
use calloop::{EventLoop, Interest, Mode, PostAction};

use luminaria::backend::{HeadlessBackend, Output};
use luminaria::core::{Display, Error};
use luminaria::gpu::{SceneOutput, SceneRenderer, VulkanOptions};
use luminaria::protocol::{Compositor, OutputGlobal, XdgShell};
use luminaria::shell::{OutputLayout, SceneItem};
use luminaria::util::{Color, Rect, Transform};

const WIDTH: i32 = 1280;
const HEIGHT: i32 = 800;

struct Screen<'a> {
    output: Output,
    state: SceneOutput<'a>,
    _global: OutputGlobal<'a>,
}

struct AppState<'a> {
    display: &'a Display,
    renderer: &'a SceneRenderer,
    compositor: &'a Compositor<'a>,
    shell: &'a XdgShell<'a>,
    screens: Vec<Screen<'a>>,
    frames_rendered: u64,
}

impl<'a> AppState<'a> {
    fn drain_events(&mut self) {
        while let Some(ev) = self.compositor.next_event() {
            match ev {
                luminaria::protocol::CompositorEvent::NewSurface(id) => {
                    println!("calloop: new client surface {id:?}");
                }
                luminaria::protocol::CompositorEvent::Commit(id) => {
                    println!("calloop: surface {id:?} committed");
                }
                luminaria::protocol::CompositorEvent::Destroy(id) => {
                    println!("calloop: surface {id:?} destroyed");
                }
                _ => {}
            }
        }
        while let Some(_ev) = self.shell.next_event() {}
    }

    fn render_frames(&mut self) -> Result<(), Error> {
        let view = Rect::new(0, 0, WIDTH, HEIGHT);
        let items = vec![
            SceneItem::rect(view, Color::rgb(0.12, 0.14, 0.18)),
            // Compositor-owned image bar at the top (serial 1)
            SceneItem::image(Rect::new(20, 20, 200, 40), 1),
        ];

        let bg = Color::rgb(0.1, 0.1, 0.15);
        for screen in &mut self.screens {
            let outcome = self.renderer.present(
                &mut screen.state,
                &mut screen.output,
                &items,
                bg,
            )?;

            self.frames_rendered += 1;
            if self.frames_rendered <= 3 {
                println!("calloop: frame #{} presented ({outcome:?})", self.frames_rendered);
            }
        }
        Ok(())
    }
}

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let exit_ms: u64 = std::env::var("LUMINARIA_EXIT_MS")
        .ok()
        .and_then(|v| v.parse().ok())
        .unwrap_or(300);

    let display = Display::new()?;
    display.init_shm()?;
    let socket = display.add_socket_auto()?;

    let mut headless = HeadlessBackend::new(&display, 16)?;
    let mut output = headless.add_output(WIDTH, HEIGHT)?;
    headless.start()?;

    let layout = OutputLayout::new()?;
    layout.add(&mut output, 0, 0);

    let global = OutputGlobal::with_properties(
        &display,
        WIDTH,
        HEIGHT,
        "HEADLESS-1",
        (0, 0),
        60000,
        1,
        Transform::Normal,
    )?;

    let renderer = SceneRenderer::new(&VulkanOptions::default())?;
    let mut scene_output = SceneOutput::new();
    renderer.reset(&mut scene_output, &mut output, 0x34325258)?; // DRM_FORMAT_XRGB8888

    // Hand the renderer a 200x40 compositor-owned test bar image (serial 1)
    let mut bar_pixels = vec![0u8; (200 * 40 * 4) as usize];
    for y in 0..40 {
        for x in 0..200 {
            let idx = ((y * 200 + x) * 4) as usize;
            bar_pixels[idx] = 40;       // R
            bar_pixels[idx + 1] = 160;   // G
            bar_pixels[idx + 2] = 220;   // B
            bar_pixels[idx + 3] = 255;   // A
        }
    }
    renderer.set_image(1, 200, 40, &bar_pixels)?;

    let compositor = Compositor::new(&display)?;
    let shell = XdgShell::new(&display)?;

    println!("calloop: compositor initialized (WAYLAND_DISPLAY={socket})");
    println!("calloop: renderer = {}", if renderer.gpu_enabled() { "GPU" } else { "CPU" });

    let screens = vec![Screen {
        output,
        state: scene_output,
        _global: global,
    }];

    let mut state = AppState {
        display: &display,
        renderer: &renderer,
        compositor: &compositor,
        shell: &shell,
        screens,
        frames_rendered: 0,
    };

    // Initial frame render
    state.render_frames()?;

    // Set up Calloop event loop
    let mut event_loop = EventLoop::<AppState<'_>>::try_new()?;
    let handle = event_loop.handle();
    let signal = event_loop.get_signal();

    // Register Display's event loop fd as a Generic source.
    // Safe: `state.display` outlives `event_loop`.
    let display_raw_fd = state.display.as_raw_fd();
    let display_source = unsafe {
        Generic::new(FdWrapper::new(display_raw_fd), Interest::READ, Mode::Level)
    };
    handle.insert_source(
        display_source,
        |_readiness, _fd, state| {
            // Non-blocking dispatch of Wayland events & timers
            state.display.dispatch(Some(Duration::ZERO)).map_err(|e| {
                std::io::Error::new(std::io::ErrorKind::Other, e.to_string())
            })?;

            // Drain protocol events
            state.drain_events();

            // Re-render when any output requests a frame
            let need_redraw = state.screens.iter_mut().any(|s| s.output.take_frame());
            if need_redraw {
                state.render_frames().map_err(|e| {
                    std::io::Error::new(std::io::ErrorKind::Other, e.to_string())
                })?;
            }

            state.display.flush_clients();
            Ok(PostAction::Continue)
        },
    ).map_err(|e| format!("failed to insert display source: {e}"))?;

    // Auto-exit timer for tests / smoke runs
    if exit_ms > 0 {
        let timer_signal = signal.clone();
        handle.insert_source(
            Timer::from_duration(Duration::from_millis(exit_ms)),
            move |_, _, _| {
                println!("calloop: exit timer fired after {exit_ms}ms, exiting");
                timer_signal.stop();
                TimeoutAction::Drop
            },
        ).map_err(|e| format!("failed to insert timer: {e}"))?;
    }

    println!("calloop: running event loop (will auto-terminate in {exit_ms}ms)...");
    event_loop.run(None, &mut state, |_| {})?;
    println!("calloop: event loop terminated cleanly");

    Ok(())
}
