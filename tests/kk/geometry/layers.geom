#version 450
// Layered rendering the way one-pass cube/cascade shadows do it: one
// invocation per layer, gl_Layer chosen by the invocation.
layout(triangles, invocations = 2) in;
layout(triangle_strip, max_vertices = 3) out;
layout(location = 0) in vec4 g_color[];
layout(location = 0) out vec4 f_color;
void main() {
    for (int i = 0; i < 3; i++) {
        gl_Layer = gl_InvocationID;
        gl_Position = gl_in[i].gl_Position + vec4(0.2 * float(gl_InvocationID), 0.0, 0.0, 0.0);
        f_color = gl_InvocationID == 0 ? g_color[i] : g_color[i].bgra;
        EmitVertex();
    }
    EndPrimitive();
}
