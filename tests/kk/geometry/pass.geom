#version 450
// Pass-through: the triangle as it came, its colour tinted.
layout(triangles) in;
layout(triangle_strip, max_vertices = 3) out;
layout(location = 0) in vec4 g_color[];
layout(location = 0) out vec4 f_color;
void main() {
    for (int i = 0; i < 3; i++) {
        gl_Position = gl_in[i].gl_Position;
        f_color = g_color[i] * vec4(0.5, 0.75, 1.0, 1.0);
        EmitVertex();
    }
    EndPrimitive();
}
