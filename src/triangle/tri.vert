#version 450
layout(location = 0) out vec3 vColor;
layout(push_constant) uniform PC { float t; float aspect; float cs; float sn; } pc;
void main() {
  vec2 p[3] = vec2[](vec2(0.0, -0.6), vec2(0.6, 0.45), vec2(-0.6, 0.45));
  vec3 c[3] = vec3[](vec3(1.0, 0.2, 0.2), vec3(0.2, 1.0, 0.3), vec3(0.3, 0.4, 1.0));
  vec2 q; vec3 col;
  if (gl_VertexIndex < 3) {
    float a = pc.t; mat2 r = mat2(cos(a), sin(a), -sin(a), cos(a));
    q = r * p[gl_VertexIndex]; q.x *= pc.aspect; col = c[gl_VertexIndex];
  } else {   /* static white marker, top-left corner of the DISPLAY (not the buffer) */
    vec2 m[3] = vec2[](vec2(-0.97, -0.97), vec2(-0.80, -0.97), vec2(-0.97, -0.80));
    q = m[gl_VertexIndex - 3]; col = vec3(1.0);
  }
  /* pre-rotation: display-space -> buffer-space (matches the surface transform) */
  q = vec2(pc.cs * q.x - pc.sn * q.y, pc.sn * q.x + pc.cs * q.y);
  gl_Position = vec4(q, 0.0, 1.0); vColor = col;
}
