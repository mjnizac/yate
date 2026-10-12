// Open water for the pixels no terrain covers.
//
// A single quad at the water level, six vertices from `gl_VertexIndex` exactly as the terrain does it,
// so no buffer exists. Where terrain was drawn the water is shaded by the terrain fragment itself; this
// quad is only what is left, so it has to lose against any terrain fragment without a depth bias.
//
// It is emitted at the depth the buffer is cleared to, and the pipeline tests GREATER_OR_EQUAL with
// depth writes off: a fragment at the cleared value passes only where nothing else was drawn. Terrain
// stores a strictly larger value, so the test fails over it every time, with no tie to break.

#version 460

#extension GL_EXT_scalar_block_layout : require

layout(push_constant, scalar) uniform Push {
    mat4  viewProjection;
    vec3  eye;
    float level;
    vec2  centre;
    vec2  halfExtent;
    uint  colour;
}
pc;

layout(location = 0) out vec3 vWorldPosition;

const vec2 kCorners[6] = vec2[6](vec2(-1.0, -1.0), vec2(-1.0, 1.0), vec2(1.0, -1.0), vec2(1.0, -1.0),
                                 vec2(-1.0, 1.0), vec2(1.0, 1.0));

void main() {
    vec2 corner    = kCorners[gl_VertexIndex % 6];
    vec3 world     = vec3(pc.centre.x + corner.x * pc.halfExtent.x, pc.level,
                          pc.centre.y + corner.y * pc.halfExtent.y);
    vWorldPosition = world;

    vec4 clip = pc.viewProjection * vec4(world, 1.0);
    // The cleared depth. The projection is reversed (near is 1, far is 0) and the viewport is the
    // identity on depth, so the stored value is `z / w` and a stored 0 is `z = 0`.
    gl_Position = vec4(clip.xy, 0.0, clip.w);
}
