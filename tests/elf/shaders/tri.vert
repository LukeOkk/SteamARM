#version 450
layout(location = 0) out vec3 vColor;
void main() {
    vec2 p[3] = vec2[3](vec2(0.0, -0.6), vec2(0.6, 0.6), vec2(-0.6, 0.6));
    vec3 c[3] = vec3[3](vec3(1.0, 0.2, 0.2), vec3(0.2, 1.0, 0.3), vec3(0.3, 0.4, 1.0));
    gl_Position = vec4(p[gl_VertexIndex], 0.0, 1.0);
    vColor = c[gl_VertexIndex];
}
