#version 450
// Source 2's Ultra Quality pass: one unmodified sample, set 1/bindings 30,14.
layout(set = 1, binding = 30) uniform texture2D image;
layout(set = 1, binding = 14) uniform sampler linear_sampler;
layout(location = 0) out vec4 color;
void main() { color = texture(sampler2D(image, linear_sampler), gl_FragCoord.xy / 64.0); }
