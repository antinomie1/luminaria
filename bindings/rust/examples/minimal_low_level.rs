//! minimal_low_level — 基于 Luminaria 底层 API 组装的最小可用混成器。
//!
//! 与 C++ 原生示例 `examples/minimal.cpp` 对齐：
//! 不依赖高层 `Server` 封装，直接使用底层组件：
//!   Display -> Backend -> Protocols (Compositor, XdgShell, Seat) -> OutputLayout -> SceneRenderer -> Cursor
//!
//! 行为与窗口管理策略：
//! - 每个新窗口最大化到屏幕上，最新的在最上面；
//! - 点击窗口通过 `scene_hit_test` 命中测试进行置顶并获取键盘焦点；
//! - 全局快捷键：
//!     Logo+Return  打开终端 (foot / $TERMINAL)
//!     Logo+Tab     轮换顶层窗口
//!     Logo+q       关闭当前顶层窗口
//!     Logo+Escape  退出混成器
//!
//! 运行方式：
//!   cargo run --example minimal_low_level                                    # 嵌套在当前桌面运行
//!   LUMINARIA_BACKEND=headless cargo run --example minimal_low_level         # 强制无头运行
//!   LUMINARIA_BACKEND=headless LUMINARIA_EXIT_MS=500 cargo run --example minimal_low_level # 冒烟测试

use std::process::Command;
use std::time::{Duration, Instant};

use luminaria::backend::{BackendInputEvent, HeadlessBackend, WaylandBackend};
use luminaria::core::Display;
use luminaria::gpu::{Cursor, SceneOutcome, SceneOutput, SceneRenderer, VulkanOptions};
use luminaria::protocol::{Compositor, OutputGlobal, Seat, Subcompositor, SurfaceId, ToplevelEvent, XdgShell};
use luminaria::shell::{scene_hit_test, OutputLayout, SceneItem};
use luminaria::util::{Color, Rect};

enum BackendKind {
    Nested(WaylandBackend),
    #[allow(dead_code)]
    Headless(HeadlessBackend),
}

fn main() -> Result<(), Box<dyn std::error::Error>> {
    println!("=== Luminaria 纯底层最小混成器 (Minimal Low-Level) ===");

    // 1. 初始化 Display 与 Wayland Socket
    let display = Display::new()?;
    let socket = display.add_socket_auto()?;
    println!("WAYLAND_DISPLAY={socket}");
    display.init_shm()?;

    // 2. 选择并初始化显示后端（优先尝试宿主嵌套，失败则回退至无头）
    let want_backend = std::env::var("LUMINARIA_BACKEND").unwrap_or_else(|_| "auto".into());
    let (mut backend, mut output) = if want_backend != "headless" {
        if let Ok(mut wb) = WaylandBackend::new(&display) {
            let out = wb.add_output(1280, 800)?;
            wb.start()?;
            println!("后端: 宿主 Wayland 嵌套窗口 (1280x800)");
            (BackendKind::Nested(wb), out)
        } else {
            let mut hb = HeadlessBackend::new(&display, 16)?;
            let out = hb.add_output(1280, 800)?;
            hb.start()?;
            println!("后端: 无头 Headless (1280x800)");
            (BackendKind::Headless(hb), out)
        }
    } else {
        let mut hb = HeadlessBackend::new(&display, 16)?;
        let out = hb.add_output(1280, 800)?;
        hb.start()?;
        println!("后端: 强制指定 Headless (1280x800)");
        (BackendKind::Headless(hb), out)
    };

    // 3. 发布核心协议接口
    let _compositor = Compositor::new(&display)?;
    let _subcompositor = Subcompositor::new(&display)?;
    let mut xdg_shell = XdgShell::new(&display)?;
    xdg_shell.set_bounds(1280, 800);

    let mut seat = Seat::new(&display, "seat0")?;
    seat.set_capabilities(true, true, false);

    // 如果是嵌套后端，继承宿主的键盘映射
    if let BackendKind::Nested(ref nb) = backend {
        let keymap = nb.keymap();
        if !keymap.is_empty() {
            seat.set_keymap(&keymap);
        }
    }

    let _output_global = OutputGlobal::new(&display, 1280, 800, "minimal-1")?;

    // 4. 多输出排版
    let mut layout = OutputLayout::new()?;
    layout.add(&mut output, 0, 0);
    let usable = Rect::new(0, 0, 1280, 800);

    // 5. 图形呈现引擎与光标
    let mut renderer = SceneRenderer::new(&VulkanOptions::default())?;
    let mut scene_output = SceneOutput::new();
    renderer.reset(&mut scene_output, &mut output, 0x34325241 /* DRM_FORMAT_ARGB8888 */)?;

    let mut cursor = Cursor::new(&mut renderer, &layout, &mut seat)?;
    let mut cursor_pos = (usable.width as f64 / 2.0, usable.height as f64 / 2.0);
    cursor.move_to(cursor_pos.0, cursor_pos.1, usable);

    // 6. 窗口栈与状态管理（对齐 minimal.cpp）
    let mut stack: Vec<SurfaceId> = Vec::new();
    output.schedule_frame();

    let exit_after = std::env::var("LUMINARIA_EXIT_MS")
        .ok()
        .and_then(|s| s.parse::<u64>().ok())
        .map(Duration::from_millis);
    let start_time = Instant::now();

    println!("混成器已就绪，进入主循环...");

    loop {
        // 驱动 Wayland 事件循环与客户端连接
        display.dispatch(Some(Duration::from_millis(16)))?;
        display.flush_clients();

        // --------------------------------------------------------------------
        // A. 处理 XDG 窗口生命周期事件 (对齐 minimal.cpp 中的 toplevel 信号处理)
        // --------------------------------------------------------------------
        while let Some(event) = xdg_shell.next_event() {
            match event {
                ToplevelEvent::Created(surface) => {
                    // 新建窗口：预设最大化尺寸
                    xdg_shell.set_maximized(surface, true);
                    xdg_shell.configure(surface, usable.width, usable.height);
                }
                ToplevelEvent::Map(surface) => {
                    // 窗口上屏：置顶并赋予键盘焦点
                    stack.retain(|&s| s != surface);
                    stack.push(surface);
                    xdg_shell.set_activated(surface, true);
                    seat.set_keyboard_focus(Some(surface));
                    output.schedule_frame();
                }
                ToplevelEvent::Unmap(surface) | ToplevelEvent::Destroy(surface) => {
                    // 窗口隐藏或销毁：从栈中移出，焦点转移至新的顶层窗口
                    stack.retain(|&s| s != surface);
                    if let Some(&top) = stack.last() {
                        xdg_shell.set_activated(top, true);
                        seat.set_keyboard_focus(Some(top));
                    } else {
                        seat.set_keyboard_focus(None);
                    }
                    output.schedule_frame();
                }
            }
        }

        // --------------------------------------------------------------------
        // B. 处理外设输入事件 (以嵌套窗口宿主事件为例)
        // --------------------------------------------------------------------
        if let BackendKind::Nested(ref mut nb) = backend {
            while let Some(input_event) = nb.next_input_event() {
                match input_event {
                    BackendInputEvent::Motion { x, y } => {
                        cursor_pos = (x, y);
                        cursor.move_to(x, y, usable);
                        seat.pointer_motion(x, y);
                    }
                    BackendInputEvent::Button { button, pressed } => {
                        seat.pointer_button(button, pressed);

                        // 按下鼠标按键：命中测试并置顶窗口（Click-to-Focus / Click-to-Raise）
                        if pressed {
                            let scene: Vec<SceneItem> = stack
                                .iter()
                                .map(|&s| SceneItem::surface(s, usable, usable.x, usable.y))
                                .collect();
                            let hit = scene_hit_test(&scene, cursor_pos.0, cursor_pos.1);

                            if let Some(surface) = hit.surface {
                                if stack.last() != Some(&surface) {
                                    stack.retain(|&s| s != surface);
                                    stack.push(surface);
                                    xdg_shell.set_activated(surface, true);
                                    seat.set_keyboard_focus(Some(surface));
                                    output.schedule_frame();
                                }
                            }
                        }
                    }
                    BackendInputEvent::Key { key, pressed } => {
                        seat.notify_key(key, pressed);

                        // 常用快捷键判定 (evdev keycode: KEY_Q=16, KEY_TAB=15, KEY_ESC=1, KEY_ENTER=28)
                        if pressed {
                            match key {
                                16 => {
                                    // Logo+q: 关闭顶层窗口
                                    if let Some(&top) = stack.last() {
                                        xdg_shell.close(top);
                                    }
                                }
                                15 => {
                                    // Logo+Tab: 轮换窗口
                                    if stack.len() > 1 {
                                        stack.rotate_left(1);
                                        if let Some(&top) = stack.last() {
                                            xdg_shell.set_activated(top, true);
                                            seat.set_keyboard_focus(Some(top));
                                        }
                                        output.schedule_frame();
                                    }
                                }
                                28 => {
                                    // Logo+Return: 启动终端
                                    let term = std::env::var("TERMINAL").unwrap_or_else(|_| "foot".into());
                                    let _ = Command::new(term).spawn();
                                }
                                1 => {
                                    // Logo+Escape: 退出
                                    println!("收到退出按键，正在终止...");
                                    return Ok(());
                                }
                                _ => {}
                            }
                        }
                    }
                }
            }
        }

        // --------------------------------------------------------------------
        // C. 响应屏幕刷新节拍进行呈现 (对齐 minimal.cpp 中的 output.frame 处理)
        // --------------------------------------------------------------------
        if output.take_frame() {
            // 构造即时模式场景项（底色背景 + 铺满输出的窗口列表）
            let mut scene = Vec::with_capacity(stack.len() + 1);
            scene.push(SceneItem::rect(usable, Color::rgb(0.12, 0.13, 0.16)));

            for &s in &stack {
                scene.push(SceneItem::surface(s, usable, usable.x, usable.y));
            }

            // 提交给渲染器呈现并释放客户端帧回调
            let outcome = renderer.present(
                &mut scene_output,
                &mut output,
                &scene,
                Color::rgb(0.12, 0.13, 0.16),
            )?;

            if outcome != SceneOutcome::Unchanged {
                output.schedule_frame();
            }
        }

        // 冒烟测试自动退出检查
        if let Some(limit) = exit_after {
            if start_time.elapsed() >= limit {
                println!("已达到 LUMINARIA_EXIT_MS 时限，正常退出。");
                break;
            }
        }
    }

    Ok(())
}
