// Extras of the OpenGL stand-in for libdeko3d (dk_gl_shim.cpp), used by deko3d_backend.cpp on Linux.
#pragma once
#include "deko3d.h"

// Stands in for the DKSH loader: a shader from translated GLSL text instead of a DKSH binary.
void dkShaderInitializeGlsl(DkShader* obj, DkStage stage, const char* text);
// Writes the image (RGBA8 contents) as a PPM file; debug only.
void dkglDumpImage(const DkImage* image, const char* path);
