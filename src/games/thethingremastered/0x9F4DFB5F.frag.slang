#version 450
#include "./shared.h"

layout(set = 2, binding = 1, std140) uniform ThingColorGrade {
  float uColorGradeBlend;
} _48;

layout(set = 1, binding = 0) uniform sampler2D uTex_tImage;
layout(set = 1, binding = 1) uniform sampler3D uTex_tLUT;

layout(location = 0) out vec4 outFragment0;
layout(location = 0) in vec2 out_texcoord;

void main() {
  vec4 _28 = texelFetch(uTex_tImage, ivec2(gl_FragCoord.xy), 0);

  vec3 scene = max(_28.xyz, vec3(0.0));
  float lut_scale = 1.0 / max(1.0, renodx::math::Max(scene));
  vec3 graded = textureLod(uTex_tLUT, (scene * lut_scale).xzy, 0.0).xyz / lut_scale;

  float blend = _48.uColorGradeBlend * RENODX_COLOR_GRADE_STRENGTH;
  outFragment0 = vec4(mix(scene, graded, vec3(blend)), 1.0);
}
