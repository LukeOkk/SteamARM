#version 450
// AMD FSR 1 EASU: the game's picture (src) upscaled to the target size.
// c0..c3 come from FsrEasuCon(), computed by shim/scaler.c.
#extension GL_GOOGLE_include_directive : require
layout(set = 0, binding = 0) uniform sampler2D src;
layout(push_constant) uniform Con { uvec4 c0, c1, c2, c3; } con;
layout(location = 0) out vec4 outColor;
#define A_GPU 1
#define A_GLSL 1
#include "ffx_a.h"
#define FSR_EASU_F 1
AF4 FsrEasuRF(AF2 p) { return textureGather(src, p, 0); }
AF4 FsrEasuGF(AF2 p) { return textureGather(src, p, 1); }
AF4 FsrEasuBF(AF2 p) { return textureGather(src, p, 2); }
#include "ffx_fsr1.h"
void main()
{
    AF3 c;
    FsrEasuF(c, AU2(gl_FragCoord.xy), con.c0, con.c1, con.c2, con.c3);
    outColor = vec4(c, 1.0);
}
