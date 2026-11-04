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
    // The fragment stage's per-frame block; never dereferenced here.
    uvec2 globals;
    uint look;
}
pc;

layout(location = 0) out vec3 vWorldPosition;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out float vHeight;
// The highest water surface drawn here, lake or river; far below the ground where there is none.
layout(location = 3) out float vWater;
// The shown output (bit 26: a fourth plane of the normals slot), smooth for a ramp and flat for ids, so
// a boundary between two ids never shows the ids between them.
layout(location = 4) out float vShown;
layout(location = 5) flat out float vShownId;

void main() {
    uint samples         = pc.samples & 0xFFFFu;
    uint verticesPerSide = max(pc.gridVertices, 2u);
    uint quadsPerSide    = verticesPerSide - 1u;
    uint vertex          = uint(gl_VertexIndex);
    // The index buffer is for the grid with a skirt: one more vertex each side, standing on the edge
    // vertex and dropped below it, so where two rings of detail meet (one twice the other's spacing) the
    // gap between their edges is closed by a wall instead of showing the background.
    uint  skirtSide = verticesPerSide + 2u;
    ivec2 outer     = ivec2(int(vertex % skirtSide), int(vertex / skirtSide)) - ivec2(1);
    bool  skirt     = any(lessThan(outer, ivec2(0))) || any(greaterThanEqual(outer, ivec2(int(verticesPerSide))));
    uvec2 gridCoord = uvec2(clamp(outer, ivec2(0), ivec2(int(verticesPerSide) - 1)));

    // The rendered grid may be coarser than the data, so a grid vertex maps onto a sample rather than
    // being one. Integer arithmetic, so neighbouring tiles land on exactly the same samples at their
    // shared edge and the seam the compute side works so hard to avoid is not reintroduced here.
    uint  stride = max(samples - 1u, 1u);
    uvec2 sampleCoord =
        min(gridCoord * stride / max(quadsPerSide, 1u), uvec2(samples - 1u));
    uint  index = sampleCoord.y * samples + sampleCoord.x;

    // The height buffer carries the lake, river and sea surfaces as three more planes after the height.
    // The sea plane is read only when the script gave one (bit 28); otherwise the fragment stage draws
    // the sea at the global level.
    float height = pc.heights.values[index];
    uint  plane  = samples * samples;
    float lake   = (pc.samples & 0x80000000u) != 0u ? pc.heights.values[plane + index] : -1e30;
    float river  = (pc.samples & 0x40000000u) != 0u ? pc.heights.values[2u * plane + index] : -1e30;
    float sea    = (pc.samples & 0x30000000u) == 0x30000000u ? pc.heights.values[3u * plane + index] : -1e30;
    vWater       = max(max(lake, river), sea);
    vShown       = (pc.samples & 0x04000000u) != 0u ? pc.normals.values[3u * plane + index] : 0.0;
    vShownId     = vShown;
    vec3  normal = vec3(pc.normals.values[index * 3u], pc.normals.values[index * 3u + 1u],
                        pc.normals.values[index * 3u + 2u]);

    vec3 world = pc.tileOrigin
                 + vec3(float(sampleCoord.x) * pc.resolution, height,
                        float(sampleCoord.y) * pc.resolution);

    // Eight samples deep: more than a coarser ring's chord can stray from the finer edge it meets.
    // Only a tile with a coarser ring outside it has one (bit 27); elsewhere the skirt's vertices stay on
    // the edge, and their triangles have no area.
    if (skirt && (pc.samples & 0x08000000u) != 0u) {
        world.y -= 8.0 * pc.resolution;
    }

    vWorldPosition = world;
    vNormal        = normal;
    vHeight        = height;
    gl_Position    = pc.viewProjection * vec4(world, 1.0);
}
