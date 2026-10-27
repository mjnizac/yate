// Displaced grid: the vertex shader reads the compute output directly, so no mesh is ever uploaded
// (spec section 12, viewer).
//
// Every vertex derives its grid position from `gl_VertexIndex`, which is the index buffer's value: the
// vertex's row-major position in the grid. The grid is indexed so a vertex shared by up to six
// triangles is shaded once (the post-transform cache) and not six times.
//
// The heights and the normals arrive as buffer device addresses in push constants, exactly as the compute
// kernels pass their buffers around, so a tile the evaluator produced can be drawn without a copy, a
// descriptor set or a layout transition.

#version 460

#extension GL_EXT_buffer_reference : require
#extension GL_EXT_scalar_block_layout : require

layout(buffer_reference, scalar, buffer_reference_align = 4) readonly buffer FloatBuffer {
    float values[];
};

layout(push_constant, scalar) uniform Push {
    mat4 viewProjection;
    // World position of the tile's first sample, in metres.
    vec3 tileOrigin;
    // Metres between samples.
    float resolution;
    // Samples per side of the tile in the low 16 bits; above them, which water bodies are drawn: bit 31
    // lakes, bit 30 rivers, bit 29 the sea at `waterLevel`.
    uint samples;
    // Vertices per side of the rendered grid, which may be coarser than `samples`.
    uint gridVertices;
    // Read by the fragment stage only; declared here so the block is the same one in both.
    float waterLevel;
    uint  waterColour;
    FloatBuffer heights;
    FloatBuffer normals;
    vec3 eye;
    uint look;
}
pc;

layout(location = 0) out vec3 vWorldPosition;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out float vHeight;
// The highest water surface drawn here, lake or river; far below the ground where there is none.
layout(location = 3) out float vWater;

void main() {
    uint samples         = pc.samples & 0xFFFFu;
    uint verticesPerSide = max(pc.gridVertices, 2u);
    uint quadsPerSide    = verticesPerSide - 1u;
    uint vertex          = uint(gl_VertexIndex);
    uvec2 gridCoord      = uvec2(vertex % verticesPerSide, vertex / verticesPerSide);

    // The rendered grid may be coarser than the data, so a grid vertex maps onto a sample rather than
    // being one. Integer arithmetic, so neighbouring tiles land on exactly the same samples at their
    // shared edge and the seam the compute side works so hard to avoid is not reintroduced here.
    uint  stride = max(samples - 1u, 1u);
    uvec2 sampleCoord =
        min(gridCoord * stride / max(quadsPerSide, 1u), uvec2(samples - 1u));
    uint  index = sampleCoord.y * samples + sampleCoord.x;

    // The height buffer carries the lake and the river surfaces as two more planes after the height.
    float height = pc.heights.values[index];
    uint  plane  = samples * samples;
    float lake   = (pc.samples & 0x80000000u) != 0u ? pc.heights.values[plane + index] : -1e30;
    float river  = (pc.samples & 0x40000000u) != 0u ? pc.heights.values[2u * plane + index] : -1e30;
    vWater       = max(lake, river);
    vec3  normal = vec3(pc.normals.values[index * 3u], pc.normals.values[index * 3u + 1u],
                        pc.normals.values[index * 3u + 2u]);

    vec3 world = pc.tileOrigin
                 + vec3(float(sampleCoord.x) * pc.resolution, height,
                        float(sampleCoord.y) * pc.resolution);

    vWorldPosition = world;
    vNormal        = normal;
    vHeight        = height;
    gl_Position    = pc.viewProjection * vec4(world, 1.0);
}
