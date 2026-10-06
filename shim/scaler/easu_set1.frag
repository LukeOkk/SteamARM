#version 450
// AMD FSR 1 EASU again (easu.frag), laid out as KosmicKrisp's FSR-1 hook
// recognises a game's EASU pass (patches/kosmickrisp-14, -16, -17: the
// input sampled at set 1 binding 30, the constants in a uniform buffer at
// set 1 binding 1 with con0 first; the shader known by EASU's own constants):
// the shim's spatial pass for DLSS 1 (shim/spatial.c), drawn by MetalFX's
// spatial scaler on KosmicKrisp and by this shader on MoltenVK.
// c0..c3 come from FsrEasuCon() (shim/spatial.c).
#extension GL_GOOGLE_include_directive : require
layout(set = 1, binding = 30) uniform sampler2D src;
layout(set = 1, binding = 1, std140) uniform Con { uvec4 c0, c1, c2, c3; } con;
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
