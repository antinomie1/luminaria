test('signal', executable('test_signal', 'test_signal.cpp', dependencies : luminaria_dep))
test('core',   executable('test_core',   'test_core.cpp',   dependencies : luminaria_dep))
test('box',      executable('test_box',      'test_box.cpp',      dependencies : luminaria_dep))
test('scene',    executable('test_scene',    'test_scene.cpp',    dependencies : luminaria_dep))
test('headless', executable('test_headless', 'test_headless.cpp', dependencies : luminaria_dep))
test('drm',      executable('test_drm',      'test_drm.cpp',      dependencies : luminaria_dep))
test('libinput', executable('test_libinput', 'test_libinput.cpp', dependencies : luminaria_dep))
test('vulkan',   executable('test_vulkan',   'test_vulkan.cpp',   dependencies : luminaria_dep))
test('composite', executable('test_composite', 'test_composite.cpp', dependencies : luminaria_dep))
test('render-output', executable('test_render_output', 'test_render_output.cpp', dependencies : luminaria_dep))
test('texture',   executable('test_texture',   'test_texture.cpp',   dependencies : luminaria_dep))

wayland_client = dependency('wayland-client')
threads = dependency('threads')
test('compositor', executable('test_compositor', 'test_compositor.cpp',
  dependencies : [luminaria_dep, wayland_client, threads]))
test('xdg', executable('test_xdg', 'test_xdg.cpp', xdg_shell_client_h,
  dependencies : [luminaria_dep, wayland_client, threads]))
test('seat', executable('test_seat', 'test_seat.cpp',
  dependencies : [luminaria_dep, wayland_client, threads]))
test('wayland-nested', executable('test_wayland_nested', 'test_wayland_nested.cpp',
  dependencies : luminaria_dep))
test('xwayland', executable('test_xwayland', 'test_xwayland.cpp',
  dependencies : luminaria_dep), timeout : 20)
test('client-texture', executable('test_client_texture', 'test_client_texture.cpp',
  dependencies : [luminaria_dep, wayland_client, threads]))
