// Stand-in for libnx's <switch.h> when the deko3d backend is compiled on Linux against the OpenGL
// stand-in for deko3d (dk_gl_shim.cpp): the backend needs only the default window.
#pragma once
static inline void* nwindowGetDefault() { return nullptr; }
