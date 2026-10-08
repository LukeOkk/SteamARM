#version 450
// Positions from the vertex index; vertex buffers are not what is tested.
layout(location = 0) out vec4 v_color;
layout(push_constant) uniform PC { vec4 offset; int mode; } pc;
const vec2 P[8] = vec2[](vec2(-0.8, -0.8), vec2(0.0, 0.7), vec2(0.7, -0.6), vec2(-0.6, 0.5),
                         vec2(0.5, 0.6), vec2(0.6, -0.1), vec2(-0.2, -0.3), vec2(0.3, 0.2));
const vec3 C[4] = vec3[](vec3(1, 0, 0), vec3(0, 1, 0), vec3(0, 0, 1), vec3(1, 1, 0));
void main() {
    int i = gl_VertexIndex & 7;
    gl_Position = vec4(P[i] + pc.offset.xy, 0.0, 1.0);
    gl_PointSize = 1.0;
    v_color = vec4(C[gl_VertexIndex & 3], 1.0);
}
