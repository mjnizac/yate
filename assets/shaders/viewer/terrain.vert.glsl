// Displaced grid: the vertex shader reads the compute output directly, so no mesh is ever uploaded
// (spec section 12, viewer).
//
// Every vertex derives its grid position from `gl_VertexIndex` and samples the height buffer at that
// position. Six vertices per quad rather than an index buffer, because an index buffer is a second
// allocation and a second upload to describe something the vertex index already encodes.
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
    // Samples per side of the tile.
    uint samples;
    // Vertices per side of the rendered grid, which may be coarser than `samples`.
    uint gridVertices;
    uint pad0;
    uint pad1;
    FloatBuffer heights;
    FloatBuffer normals;
}
pc;

layout(location = 0) out vec3 vWorldPosition;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out float vHeight;

// Which corner of which quad this vertex is. Two triangles wound counter-clockwise when seen from above,
// which with the pipeline's front face and cull mode is what makes the terrain visible from above and
// culled from below.
const uvec2 kCorners[6] =
    uvec2[6](uvec2(0, 0), uvec2(0, 1), uvec2(1, 0), uvec2(1, 0), uvec2(0, 1), uvec2(1, 1));

void main() {
    uint quadsPerSide = max(pc.gridVertices, 2u) - 1u;
    uint quad         = uint(gl_VertexIndex) / 6u;
    uint corner       = uint(gl_VertexIndex) % 6u;

    uvec2 quadCoord = uvec2(quad % quadsPerSide, quad / quadsPerSide);
    uvec2 gridCoord = quadCoord + kCorners[corner];

    // The rendered grid may be coarser than the data, so a grid vertex maps onto a sample rather than
    // being one. Integer arithmetic, so neighbouring tiles land on exactly the same samples at their
    // shared edge and the seam the compute side works so hard to avoid is not reintroduced here.
    uint  stride = max(pc.samples - 1u, 1u);
    uvec2 sampleCoord =
        min(gridCoord * stride / max(quadsPerSide, 1u), uvec2(pc.samples - 1u));
    uint  index = sampleCoord.y * pc.samples + sampleCoord.x;

    float height = pc.heights.values[index];
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
