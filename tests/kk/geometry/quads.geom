#version 450
// Points to quads (particles): one strip of four vertices per point.
layout(points) in;
layout(triangle_strip, max_vertices = 4) out;
layout(location = 0) in vec4 g_color[];
layout(location = 0) out vec4 f_color;
void main() {
    vec4 c = gl_in[0].gl_Position;
    for (int i = 0; i < 4; i++) {
        vec2 d = vec2((i & 1) != 0 ? 0.15 : -0.15, (i & 2) != 0 ? 0.1 : -0.1);
        gl_Position = c + vec4(d, 0.0, 0.0);
        f_color = g_color[0] * (0.4 + 0.2 * float(i));
        EmitVertex();
    }
    EndPrimitive();
}
