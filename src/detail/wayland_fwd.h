// The libwayland types that appear in luminaria's own signatures, forward
// declared and nothing more.
//
// These have to reach a module interface unit through a real #include: entities
// declared directly in a global module fragment are rejected (-Wglobal-module),
// and declaring them *after* `export module` would attach them to module
// luminaria and make them a different type from the ::wl_resource the C headers
// declare. So: one small header, included in the global module fragment.
#pragma once

struct wl_client;
struct wl_display;
struct wl_event_loop;
struct wl_event_source;
struct wl_resource;
