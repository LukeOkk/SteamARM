#version 450
// Draw a 49x49 scene (77% of 64): blue left, red right.
layout(location = 0) out vec4 color;
void main() { color = gl_FragCoord.x < 24.5 ? vec4(0,0,1,1) : vec4(1,0,0,1); }
