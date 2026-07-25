project('luminaria', 'c', 'cpp',
  version : '0.0.1',
  license : 'MIT',
  meson_version : '>=1.3.0',
  default_options : [
    'cpp_std=c++23',
    'warning_level=3',
    'werror=true',
    'default_library=static',
  ],
)

cc = meson.get_compiler('cpp')

# Partial designated-init of libwayland's C *_interface vtables is idiomatic;
# unlisted request slots are intentionally null. Silence that one pedantic warning.
add_project_arguments('-Wno-missing-field-initializers', language : 'cpp')

# --- Hard requirement: a modern C++ stdlib with <expected> (C++23). ---
if not cc.has_header('expected')
  error('luminaria needs a C++23 stdlib with <expected> (gcc>=14 / clang>=18).')
endif

# --- Core dependency: we build ON libwayland-server, we do NOT reimplement the wire. ---
wayland_server = dependency('wayland-server')
vulkan = dependency('vulkan')
xkbcommon = dependency('xkbcommon')
wayland_client = dependency('wayland-client')
xcb = dependency('xcb')
libdrm = dependency('libdrm')
libinput = dependency('libinput')
libudev = dependency('libudev')
gbm = dependency('gbm')
libseat = dependency('libseat')

luminaria_deps = [wayland_server, vulkan, xkbcommon, wayland_client, xcb, libdrm, libinput, libudev, gbm, libseat]

# --- Generate server-side glue for extension protocols (xdg-shell). ---
wl_scanner_dep = dependency('wayland-scanner', native : true)
wl_scanner = find_program(wl_scanner_dep.get_variable('wayland_scanner'))
wl_protocols_dir = dependency('wayland-protocols').get_variable('pkgdatadir')

xdg_xml = wl_protocols_dir / 'stable/xdg-shell/xdg-shell.xml'
xdg_shell_h = custom_target('xdg-shell-protocol.h',
  input : xdg_xml, output : 'xdg-shell-protocol.h',
  command : [wl_scanner, 'server-header', '@INPUT@', '@OUTPUT@'])
xdg_shell_c = custom_target('xdg-shell-protocol.c',
  input : xdg_xml, output : 'xdg-shell-protocol.c',
  command : [wl_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])
# Client header — only tests act as clients; the interface symbols come from the lib.
xdg_shell_client_h = custom_target('xdg-shell-client-protocol.h',
  input : xdg_xml, output : 'xdg-shell-client-protocol.h',
  command : [wl_scanner, 'client-header', '@INPUT@', '@OUTPUT@'])

# --- linux-dmabuf-unstable-v1: GPU (dmabuf) client buffers. ---
dmabuf_xml = wl_protocols_dir / 'unstable/linux-dmabuf/linux-dmabuf-unstable-v1.xml'
dmabuf_h = custom_target('linux-dmabuf-unstable-v1-protocol.h',
  input : dmabuf_xml, output : 'linux-dmabuf-unstable-v1-protocol.h',
  command : [wl_scanner, 'server-header', '@INPUT@', '@OUTPUT@'])
dmabuf_c = custom_target('linux-dmabuf-unstable-v1-protocol.c',
  input : dmabuf_xml, output : 'linux-dmabuf-unstable-v1-protocol.c',
  command : [wl_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])
# Client header — only tests act as dmabuf clients.
dmabuf_client_h = custom_target('linux-dmabuf-unstable-v1-client-protocol.h',
  input : dmabuf_xml, output : 'linux-dmabuf-unstable-v1-client-protocol.h',
  command : [wl_scanner, 'client-header', '@INPUT@', '@OUTPUT@'])

# --- xdg-decoration-unstable-v1: BOTH directions. As a client we ask the parent
#     compositor to decorate our nested window; as a server we answer the same
#     question for our own clients.
xdg_decoration_xml = wl_protocols_dir / 'unstable/xdg-decoration/xdg-decoration-unstable-v1.xml'
xdg_decoration_c = custom_target('xdg-decoration-unstable-v1-protocol.c',
  input : xdg_decoration_xml, output : 'xdg-decoration-unstable-v1-protocol.c',
  command : [wl_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])
xdg_decoration_h = custom_target('xdg-decoration-unstable-v1-protocol.h',
  input : xdg_decoration_xml, output : 'xdg-decoration-unstable-v1-protocol.h',
  command : [wl_scanner, 'server-header', '@INPUT@', '@OUTPUT@'])
xdg_decoration_client_h = custom_target('xdg-decoration-unstable-v1-client-protocol.h',
  input : xdg_decoration_xml, output : 'xdg-decoration-unstable-v1-client-protocol.h',
  command : [wl_scanner, 'client-header', '@INPUT@', '@OUTPUT@'])

# --- viewporter: crop + scale a buffer. The other half of fractional scaling. ---
viewporter_xml = wl_protocols_dir / 'stable/viewporter/viewporter.xml'
viewporter_h = custom_target('viewporter-protocol.h',
  input : viewporter_xml, output : 'viewporter-protocol.h',
  command : [wl_scanner, 'server-header', '@INPUT@', '@OUTPUT@'])
viewporter_c = custom_target('viewporter-protocol.c',
  input : viewporter_xml, output : 'viewporter-protocol.c',
  command : [wl_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])
viewporter_client_h = custom_target('viewporter-client-protocol.h',
  input : viewporter_xml, output : 'viewporter-client-protocol.h',
  command : [wl_scanner, 'client-header', '@INPUT@', '@OUTPUT@'])

# --- fractional-scale-v1: the real output scale, in 120ths. ---
fractional_scale_xml = wl_protocols_dir / 'staging/fractional-scale/fractional-scale-v1.xml'
fractional_scale_h = custom_target('fractional-scale-v1-protocol.h',
  input : fractional_scale_xml, output : 'fractional-scale-v1-protocol.h',
  command : [wl_scanner, 'server-header', '@INPUT@', '@OUTPUT@'])
fractional_scale_c = custom_target('fractional-scale-v1-protocol.c',
  input : fractional_scale_xml, output : 'fractional-scale-v1-protocol.c',
  command : [wl_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])
fractional_scale_client_h = custom_target('fractional-scale-v1-client-protocol.h',
  input : fractional_scale_xml, output : 'fractional-scale-v1-client-protocol.h',
  command : [wl_scanner, 'client-header', '@INPUT@', '@OUTPUT@'])

# --- xdg-output-unstable-v1: logical output layout (grim/slurp read this). ---
xdg_output_xml = wl_protocols_dir / 'unstable/xdg-output/xdg-output-unstable-v1.xml'
xdg_output_h = custom_target('xdg-output-unstable-v1-protocol.h',
  input : xdg_output_xml, output : 'xdg-output-unstable-v1-protocol.h',
  command : [wl_scanner, 'server-header', '@INPUT@', '@OUTPUT@'])
xdg_output_c = custom_target('xdg-output-unstable-v1-protocol.c',
  input : xdg_output_xml, output : 'xdg-output-unstable-v1-protocol.c',
  command : [wl_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])
xdg_output_client_h = custom_target('xdg-output-unstable-v1-client-protocol.h',
  input : xdg_output_xml, output : 'xdg-output-unstable-v1-client-protocol.h',
  command : [wl_scanner, 'client-header', '@INPUT@', '@OUTPUT@'])

# --- primary-selection-unstable-v1: middle-click paste. ---
primary_sel_xml = wl_protocols_dir / 'unstable/primary-selection/primary-selection-unstable-v1.xml'
primary_sel_h = custom_target('primary-selection-unstable-v1-protocol.h',
  input : primary_sel_xml, output : 'primary-selection-unstable-v1-protocol.h',
  command : [wl_scanner, 'server-header', '@INPUT@', '@OUTPUT@'])
primary_sel_c = custom_target('primary-selection-unstable-v1-protocol.c',
  input : primary_sel_xml, output : 'primary-selection-unstable-v1-protocol.c',
  command : [wl_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])
primary_sel_client_h = custom_target('primary-selection-unstable-v1-client-protocol.h',
  input : primary_sel_xml, output : 'primary-selection-unstable-v1-client-protocol.h',
  command : [wl_scanner, 'client-header', '@INPUT@', '@OUTPUT@'])

# --- single-pixel-buffer-v1: a 1x1 solid-colour wl_buffer with no allocation. ---
single_pixel_xml = wl_protocols_dir / 'staging/single-pixel-buffer/single-pixel-buffer-v1.xml'
single_pixel_h = custom_target('single-pixel-buffer-v1-protocol.h',
  input : single_pixel_xml, output : 'single-pixel-buffer-v1-protocol.h',
  command : [wl_scanner, 'server-header', '@INPUT@', '@OUTPUT@'])
single_pixel_c = custom_target('single-pixel-buffer-v1-protocol.c',
  input : single_pixel_xml, output : 'single-pixel-buffer-v1-protocol.c',
  command : [wl_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])
single_pixel_client_h = custom_target('single-pixel-buffer-v1-client-protocol.h',
  input : single_pixel_xml, output : 'single-pixel-buffer-v1-client-protocol.h',
  command : [wl_scanner, 'client-header', '@INPUT@', '@OUTPUT@'])

# --- presentation-time: when a frame actually hit the screen (animation timing). ---
presentation_xml = wl_protocols_dir / 'stable/presentation-time/presentation-time.xml'
presentation_h = custom_target('presentation-time-protocol.h',
  input : presentation_xml, output : 'presentation-time-protocol.h',
  command : [wl_scanner, 'server-header', '@INPUT@', '@OUTPUT@'])
presentation_c = custom_target('presentation-time-protocol.c',
  input : presentation_xml, output : 'presentation-time-protocol.c',
  command : [wl_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])
presentation_client_h = custom_target('presentation-time-client-protocol.h',
  input : presentation_xml, output : 'presentation-time-client-protocol.h',
  command : [wl_scanner, 'client-header', '@INPUT@', '@OUTPUT@'])

# --- tearing-control-v1: a surface asking to tear rather than wait for vblank. ---
tearing_xml = wl_protocols_dir / 'staging/tearing-control/tearing-control-v1.xml'
tearing_h = custom_target('tearing-control-v1-protocol.h',
  input : tearing_xml, output : 'tearing-control-v1-protocol.h',
  command : [wl_scanner, 'server-header', '@INPUT@', '@OUTPUT@'])
tearing_c = custom_target('tearing-control-v1-protocol.c',
  input : tearing_xml, output : 'tearing-control-v1-protocol.c',
  command : [wl_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])
tearing_client_h = custom_target('tearing-control-v1-client-protocol.h',
  input : tearing_xml, output : 'tearing-control-v1-client-protocol.h',
  command : [wl_scanner, 'client-header', '@INPUT@', '@OUTPUT@'])

# --- cursor-shape-v1: named cursors, so clients stop shipping cursor bitmaps. ---
cursor_shape_xml = wl_protocols_dir / 'staging/cursor-shape/cursor-shape-v1.xml'
cursor_shape_h = custom_target('cursor-shape-v1-protocol.h',
  input : cursor_shape_xml, output : 'cursor-shape-v1-protocol.h',
  command : [wl_scanner, 'server-header', '@INPUT@', '@OUTPUT@'])
cursor_shape_c = custom_target('cursor-shape-v1-protocol.c',
  input : cursor_shape_xml, output : 'cursor-shape-v1-protocol.c',
  command : [wl_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])
cursor_shape_client_h = custom_target('cursor-shape-v1-client-protocol.h',
  input : cursor_shape_xml, output : 'cursor-shape-v1-client-protocol.h',
  command : [wl_scanner, 'client-header', '@INPUT@', '@OUTPUT@'])
# cursor-shape references tablet-v2 interfaces; the generated code needs them.
tablet_xml = wl_protocols_dir / 'stable/tablet/tablet-v2.xml'
tablet_h = custom_target('tablet-v2-protocol.h',
  input : tablet_xml, output : 'tablet-v2-protocol.h',
  command : [wl_scanner, 'server-header', '@INPUT@', '@OUTPUT@'])
tablet_c = custom_target('tablet-v2-protocol.c',
  input : tablet_xml, output : 'tablet-v2-protocol.c',
  command : [wl_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])

# --- ext-workspace-v1: workspaces a panel/pager can list and switch. ---
workspace_xml = wl_protocols_dir / 'staging/ext-workspace/ext-workspace-v1.xml'
workspace_h = custom_target('ext-workspace-v1-protocol.h',
  input : workspace_xml, output : 'ext-workspace-v1-protocol.h',
  command : [wl_scanner, 'server-header', '@INPUT@', '@OUTPUT@'])
workspace_c = custom_target('ext-workspace-v1-protocol.c',
  input : workspace_xml, output : 'ext-workspace-v1-protocol.c',
  command : [wl_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])
workspace_client_h = custom_target('ext-workspace-v1-client-protocol.h',
  input : workspace_xml, output : 'ext-workspace-v1-client-protocol.h',
  command : [wl_scanner, 'client-header', '@INPUT@', '@OUTPUT@'])

# --- linux-drm-syncobj-v1: explicit GPU synchronisation. ---
syncobj_xml = wl_protocols_dir / 'staging/linux-drm-syncobj/linux-drm-syncobj-v1.xml'
syncobj_h = custom_target('linux-drm-syncobj-v1-protocol.h',
  input : syncobj_xml, output : 'linux-drm-syncobj-v1-protocol.h',
  command : [wl_scanner, 'server-header', '@INPUT@', '@OUTPUT@'])
syncobj_c = custom_target('linux-drm-syncobj-v1-protocol.c',
  input : syncobj_xml, output : 'linux-drm-syncobj-v1-protocol.c',
  command : [wl_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])
syncobj_client_h = custom_target('linux-drm-syncobj-v1-client-protocol.h',
  input : syncobj_xml, output : 'linux-drm-syncobj-v1-client-protocol.h',
  command : [wl_scanner, 'client-header', '@INPUT@', '@OUTPUT@'])

# --- Screencopy protocols ---
wlr_screencopy_xml = files('protocol/wlr-screencopy-unstable-v1.xml')
wlr_screencopy_h = custom_target('wlr-screencopy-unstable-v1-protocol.h',
  input : wlr_screencopy_xml, output : 'wlr-screencopy-unstable-v1-protocol.h',
  command : [wl_scanner, 'server-header', '@INPUT@', '@OUTPUT@'])
wlr_screencopy_c = custom_target('wlr-screencopy-unstable-v1-protocol.c',
  input : wlr_screencopy_xml, output : 'wlr-screencopy-unstable-v1-protocol.c',
  command : [wl_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])

ext_copy_capture_xml = files('protocol/ext-image-copy-capture-v1.xml')
ext_copy_capture_h = custom_target('ext-image-copy-capture-v1-protocol.h',
  input : ext_copy_capture_xml, output : 'ext-image-copy-capture-v1-protocol.h',
  command : [wl_scanner, 'server-header', '@INPUT@', '@OUTPUT@'])
ext_copy_capture_c = custom_target('ext-image-copy-capture-v1-protocol.c',
  input : ext_copy_capture_xml, output : 'ext-image-copy-capture-v1-protocol.c',
  command : [wl_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])

ext_capture_source_xml = files('protocol/ext-image-capture-source-v1.xml')
ext_capture_source_h = custom_target('ext-image-capture-source-v1-protocol.h',
  input : ext_capture_source_xml, output : 'ext-image-capture-source-v1-protocol.h',
  command : [wl_scanner, 'server-header', '@INPUT@', '@OUTPUT@'])
ext_capture_source_c = custom_target('ext-image-capture-source-v1-protocol.c',
  input : ext_capture_source_xml, output : 'ext-image-capture-source-v1-protocol.c',
  command : [wl_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])

# --- Shaders: the renderer's textured-quad pipeline (scale + output rotation).
#     glslangValidator's --vn emits SPIR-V as a C array, so nothing is loaded at
#     runtime and there is no shader file to ship.
glslang = find_program('glslangValidator', required : false)
if not glslang.found()
  error('luminaria needs glslangValidator (Fedora: glslang) to build its shaders.')
endif
quad_vert_spv = custom_target('quad_vert_spv',
  input : 'src/render/quad.vert', output : 'quad_vert_spv.h',
  command : [glslang, '-V', '--vn', 'kQuadVertSpv', '-o', '@OUTPUT@', '@INPUT@'])
quad_frag_spv = custom_target('quad_frag_spv',
  input : 'src/render/quad.frag', output : 'quad_frag_spv.h',
  command : [glslang, '-V', '--vn', 'kQuadFragSpv', '-o', '@OUTPUT@', '@INPUT@'])

luminaria_inc = include_directories('include')

subdir('src')
subdir('examples')
subdir('tests')
