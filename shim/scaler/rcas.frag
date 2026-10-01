#version 450
// AMD FSR 1 RCAS: sharpens EASU's output (src, same size as the target).
// stops: 0 = the most sharpening, each stop halves it (FsrRcasCon()).
#extension GL_GOOGLE_include_directive : require
layout(set = 0, binding = 0) uniform sampler2D src;
layout(push_constant) uniform Con { float stops; } con;
layout(location = 0) out vec4 outColor;
#define A_GPU 1
#define A_GLSL 1
#include "ffx_a.h"
#define FSR_RCAS_F 1
AF4 FsrRcasLoadF(ASU2 p) { return texelFetch(src, p, 0); }
void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b) {}
#include "ffx_fsr1.h"
void main()
{
    AU4 c = AU4(floatBitsToUint(exp2(-con.stops)), 0u, 0u, 0u);
    AF3 o;
    FsrRcasF(o.r, o.g, o.b, AU2(gl_FragCoord.xy), c);
    outColor = vec4(o, 1.0);
}
