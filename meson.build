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

luminaria_deps = [wayland_server, vulkan, xkbcommon, wayland_client, xcb, libdrm, libinput, libudev]

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

luminaria_inc = include_directories('include')

subdir('src')
subdir('examples')
subdir('tests')
