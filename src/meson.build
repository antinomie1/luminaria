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
    'types/screencopy.cpp',
    'types/linux_dmabuf.cpp',
    'types/subcompositor.cpp',
    'types/data_device.cpp',
    'types/drm_syncobj.cpp',
  ),
  xdg_shell_c,         # generated protocol code
  xdg_shell_h,         # generated server header (also on the include path)
  xdg_shell_client_h,  # nested backend acts as a client of the parent
  xdg_decoration_c, xdg_decoration_client_h, # …and asks it to decorate our window
  dmabuf_c, dmabuf_h,  # linux-dmabuf-v1 server glue
  primary_sel_c, primary_sel_h,
  xdg_output_c, xdg_output_h,
  syncobj_c, syncobj_h,
  wlr_screencopy_c, wlr_screencopy_h,
  ext_copy_capture_c, ext_copy_capture_h,
  ext_capture_source_c, ext_capture_source_h,
  include_directories : luminaria_inc,
  dependencies : luminaria_deps,
)

luminaria_dep = declare_dependency(
  link_with : luminaria_lib,
  include_directories : luminaria_inc,
  dependencies : luminaria_deps,
)
