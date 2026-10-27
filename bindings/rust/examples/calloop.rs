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
//!             // Dispatch Wayland requests and internal timers without
//!             // blocking, then handle everything they produced:
//!             for event in display.dispatch(Some(Duration::ZERO)).unwrap() {
//!                 // Event::Output(id, OutputEvent::Frame) => renderer.present(...)
//!             }
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
//!     // Dispatch pending requests without blocking the async executor, and
//!     // handle what they produced:
//!     for event in display.dispatch(Some(Duration::ZERO))? { /* ... */ }
//!     guard.clear_ready();
//! }
//! # }
//! ```

use std::os::fd::AsRawFd;
use std::time::Duration;

use calloop::generic::{FdWrapper, Generic};
use calloop::timer::{TimeoutAction, Timer};
use calloop::{EventLoop, Interest, Mode, PostAction};

use luminaria::Event;
use luminaria::backend::{HeadlessBackend, Output, OutputEvent};
use luminaria::core::{Display, Error};
use luminaria::gpu::{SceneRenderer, VulkanOptions};
use luminaria::protocol::{CompositorEvent, CoreGlobals, OutputGlobal};
use luminaria::shell::{OutputLayout, SceneItem};
use luminaria::util::{Color, Rect};

const WIDTH: i32 = 1280;
const HEIGHT: i32 = 800;

struct AppState<'a> {
    display: &'a Display,
    renderer: &'a SceneRenderer,
    output: Output,
    frames_rendered: u64,
}

impl AppState<'_> {
    /// Everything the dispatch produced: client surfaces are logged, a frame
    /// the output asks for is drawn.
    fn handle(&mut self, events: luminaria::Events) -> Result<(), Error> {
        for event in events {
            match event {
                Event::Compositor(CompositorEvent::NewSurface(id)) => println!("calloop: new client surface {id:?}"),
                Event::Compositor(CompositorEvent::Commit(id)) => println!("calloop: surface {id:?} committed"),
                Event::Compositor(CompositorEvent::Destroy(id)) => println!("calloop: surface {id:?} destroyed"),
                Event::Output(_, OutputEvent::Frame) => self.render_frame()?,
                _ => {}
            }
        }
        Ok(())
    }

    fn render_frame(&mut self) -> Result<(), Error> {
        let view = Rect::new(0, 0, WIDTH, HEIGHT);
        let items = [
            SceneItem::rect(view, Color::rgb(0.12, 0.14, 0.18)),
            // Compositor-owned image bar at the top (serial 1)
            SceneItem::image(Rect::new(20, 20, 200, 40), 1),
        ];
        let outcome = self.renderer.present(&mut self.output, &items, Color::rgb(0.1, 0.1, 0.15))?;
        self.frames_rendered += 1;
        if self.frames_rendered <= 3 {
            println!("calloop: frame #{} presented ({outcome:?})", self.frames_rendered);
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
    let _globals = CoreGlobals::new(&display)?;
    let socket = display.add_socket_auto()?;

    let mut headless = HeadlessBackend::new(&display, 16)?;
    let mut output = headless.add_output(WIDTH, HEIGHT)?;
    headless.start()?;

    let layout = OutputLayout::new()?;
    layout.add(&mut output, 0, 0);
    let _global = OutputGlobal::for_output(&display, &output, &layout, "HEADLESS-1")?;

    let renderer = SceneRenderer::new(&VulkanOptions::default())?;
    renderer.reset(&mut output)?;

    // Hand the renderer a 200x40 compositor-owned test bar image (serial 1)
    let bar_pixels: Vec<u8> = std::iter::repeat_n([40u8, 160, 220, 255], 200 * 40).flatten().collect();
    renderer.set_image(1, 200, 40, &bar_pixels)?;

    println!("calloop: compositor initialized (WAYLAND_DISPLAY={socket})");
    println!("calloop: renderer = {}", if renderer.gpu_enabled() { "GPU" } else { "CPU" });

    let mut state = AppState { display: &display, renderer: &renderer, output, frames_rendered: 0 };

    // Initial frame render
    state.render_frame()?;

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
            // Non-blocking dispatch of Wayland events & timers, then what it produced.
            let events = state.display.dispatch(Some(Duration::ZERO)).map_err(std::io::Error::other)?;
            state.handle(events).map_err(std::io::Error::other)?;
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
