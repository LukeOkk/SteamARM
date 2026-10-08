#version 450
// Two separate triangles from one: two strips, EndPrimitive between them,
// the second only for even primitives (a varying output count).
layout(triangles) in;
layout(triangle_strip, max_vertices = 6) out;
layout(location = 0) in vec4 g_color[];
layout(location = 0) out vec4 f_color;
void main() {
    for (int i = 0; i < 3; i++) {
        gl_Position = vec4(gl_in[i].gl_Position.xy * 0.5 - vec2(0.45, 0.0), 0.0, 1.0);
        f_color = g_color[i];
        EmitVertex();
    }
    EndPrimitive();
    if ((gl_PrimitiveIDIn & 1) == 0) {
        for (int i = 0; i < 3; i++) {
            gl_Position = vec4(gl_in[i].gl_Position.xy * 0.5 + vec2(0.45, 0.0), 0.0, 1.0);
            f_color = vec4(1.0) - g_color[i];
            EmitVertex();
        }
        EndPrimitive();
    }
}
