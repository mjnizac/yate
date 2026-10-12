// The open sea: opaque deep water with the same surface reflection the shoreline uses.

#version 460

#extension GL_EXT_scalar_block_layout : require

#include "lib/water.lib.glsl"

layout(push_constant, scalar) uniform Push {
    mat4  viewProjection;
    vec3  eye;
    float level;
    vec2  centre;
    vec2  halfExtent;
    uint  colour;
}
pc;

layout(location = 0) in vec3 vWorldPosition;

layout(location = 0) out vec4 outColor;

void main() {
    vec3 body      = unpackUnorm4x8(pc.colour).rgb;
    vec3 direction = normalize(vWorldPosition - pc.eye);
    outColor       = vec4(WaterSurface(body * 0.55, direction), 1.0);
}
