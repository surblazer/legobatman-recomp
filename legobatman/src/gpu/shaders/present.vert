#version 460
// Present pass: one triangle covering the swapchain image; samples the guest front buffer.
layout(location = 0) out vec2 uv;
void main() {
  float x = float(gl_VertexID & 1) * 4.0 - 1.0;
  float y = float(gl_VertexID & 2) * 2.0 - 1.0;
  // deko3d's origin is upper-left: NDC y = +1 is the top row, which is texture row 0.
  uv = vec2(float(gl_VertexID & 1) * 2.0, 1.0 - float(gl_VertexID & 2));
  gl_Position = vec4(x, y, 0.0, 1.0);
}
