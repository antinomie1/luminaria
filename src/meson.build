luminaria_lib = static_library('luminaria',
  files(
    'core/display.cpp',
    'core/event_loop.cpp',
    'backend/headless.cpp',
    'backend/wayland.cpp',
    'backend/drm.cpp',
    'backend/libinput.cpp',
    'xwayland/xwayland.cpp',
    'render/vulkan.cpp',
    'types/compositor.cpp',
    'types/xdg_shell.cpp',
    'scene/scene.cpp',
    'types/seat.cpp',
    'types/output_global.cpp',
  ),
  xdg_shell_c,         # generated protocol code
  xdg_shell_h,         # generated server header (also on the include path)
  xdg_shell_client_h,  # nested backend acts as a client of the parent
  include_directories : luminaria_inc,
  dependencies : luminaria_deps,
)

luminaria_dep = declare_dependency(
  link_with : luminaria_lib,
  include_directories : luminaria_inc,
  dependencies : luminaria_deps,
)
