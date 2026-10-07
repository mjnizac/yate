// Uniform op-kernel interface shared by every compute shader under ops/.
//
// Inputs and outputs arrive as buffer device addresses in push constants, so no descriptor set
// has to be managed per dispatch. The buffer load/store boilerplate lives here, separate from
// the math, so a future code generator can chain the math functions instead of rewriting them
// (spec section 9, "Future: kernel fusion").

#ifndef ENGINE_LIB_KERNEL_GLSL
#define ENGINE_LIB_KERNEL_GLSL

#extension GL_EXT_buffer_reference : require
#extension GL_EXT_scalar_block_layout : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require

// Workgroup size comes from specialization constants 0..2, so one SPIR-V module serves both
// domains: 8x8x1 for R2 and 4x4x4 for R3. Op variants use constant ids 3 and up.
layout(local_size_x_id = 0, local_size_y_id = 1, local_size_z_id = 2) in;

layout(buffer_reference, scalar, buffer_reference_align = 4) buffer FloatBuffer {
    float values[];
};

// Mirrors engine::vulkan::KernelPushConstants exactly, under scalar block layout.
layout(push_constant, scalar) uniform KernelConstants {
    ivec3    origin;
    uvec3    extent;
    uint     halo;
    // Domain in bits 0-7, channel mask in bits 8-15. Packed to keep the block inside the guaranteed
    // 128-byte range; read through IsR3() and WantsChannel() below, never directly.
    uint     flags;
    uint     seed;
    uint     inputHalos;
    uint64_t inputs[4];
    uint64_t outputs[2];
    uint     params[10];
} pc;

#define ENGINE_DOMAIN_R2 2u
#define ENGINE_DOMAIN_R3 3u

uint Domain() { return pc.flags & 0xFFu; }
uint ChannelMask() { return (pc.flags >> 8u) & 0xFFu; }

bool IsR3() { return Domain() == ENGINE_DOMAIN_R3; }

// Section size including the halo on every axis of the domain. `y` is 1 for R2.
uvec3 PaddedExtent() {
    uint pad = 2u * pc.halo;
    if (IsR3()) {
        return pc.extent + uvec3(pad);
    }
    return uvec3(pc.extent.x + pad, 1u, pc.extent.z + pad);
}

// Local sample this invocation owns, as (x, y, z). For R2 the dispatch is 2D and maps to (x, z).
uvec3 LocalSample() {
    if (IsR3()) {
        return gl_GlobalInvocationID.xyz;
    }
    return uvec3(gl_GlobalInvocationID.x, 0u, gl_GlobalInvocationID.y);
}

bool InsidePaddedSection(uvec3 local) {
    uvec3 n = PaddedExtent();
    return all(lessThan(local, n));
}

// Index of a padded sample in a value buffer: x fastest, then z, then y.
uint SampleIndex(uvec3 local) {
    uvec3 n = PaddedExtent();
    if (IsR3()) {
        return (local.y * n.z + local.z) * n.x + local.x;
    }
    return local.z * n.x + local.x;
}

// Integer world-space sample coordinate. Positions are always derived from the integer section
// origin plus a local offset, which is what makes neighbouring sections agree exactly at their
// borders and keeps precision far from the origin (spec section 10).
ivec3 WorldSample(uvec3 local) {
    return pc.origin - ivec3(int(pc.halo)) + ivec3(local);
}

bool WantsChannel(uint channel) { return (ChannelMask() & (1u << channel)) != 0u; }

// --- Reading an input whose halo differs from the output's ----------------------------------------
//
// Halo propagation gives a producer `consumer halo + consumer radius`, so a neighbourhood op reads
// buffers with a wider border than it writes. A wider border means a different row stride and a
// different origin, so the output's sample index cannot be reused for the input. Pointwise ops have
// radius zero, their input halo equals `pc.halo`, and they can keep using the plain helpers above.

uint InputHalo(uint slot) { return (pc.inputHalos >> (slot * 8u)) & 0xFFu; }

uvec3 PaddedExtentWithHalo(uint halo) {
    uint pad = 2u * halo;
    if (IsR3()) {
        return pc.extent + uvec3(pad);
    }
    return uvec3(pc.extent.x + pad, 1u, pc.extent.z + pad);
}

// Local coordinate of the same world sample inside an input: shifted by how much wider that input's
// border is.
uvec3 InputLocal(uint slot, uvec3 local) {
    uint delta = InputHalo(slot) - pc.halo;
    return local + uvec3(delta, IsR3() ? delta : 0u, delta);
}

uint InputIndex(uint slot, uvec3 local) {
    uvec3 n = PaddedExtentWithHalo(InputHalo(slot));
    uvec3 at = InputLocal(slot, local);
    if (IsR3()) {
        return (at.y * n.z + at.z) * n.x + at.x;
    }
    return at.z * n.x + at.x;
}

// True when `local` plus `offset` is still inside the input's padded extent. A neighbourhood op must
// check this at the border of its own output: halo propagation guarantees the taps it needs exist,
// but a clamped or wrapped read would silently change the result at the edges.
bool InputContains(uint slot, uvec3 local, ivec3 offset) {
    uvec3 n  = PaddedExtentWithHalo(InputHalo(slot));
    ivec3 at = ivec3(InputLocal(slot, local)) + offset;
    return all(greaterThanEqual(at, ivec3(0))) && all(lessThan(at, ivec3(n)));
}

uint InputIndexOffset(uint slot, uvec3 local, ivec3 offset) {
    uvec3 n  = PaddedExtentWithHalo(InputHalo(slot));
    ivec3 at = ivec3(InputLocal(slot, local)) + offset;
    if (IsR3()) {
        return uint((at.y * int(n.z) + at.z) * int(n.x) + at.x);
    }
    return uint(at.z * int(n.x) + at.x);
}

// --- Typed access to the bound values ---------------------------------------------------------

float LoadScalar(uint slot, uint index) {
    return FloatBuffer(pc.inputs[slot]).values[index];
}

void StoreScalar(uint slot, uint index, float value) {
    FloatBuffer(pc.outputs[slot]).values[index] = value;
}

// Components of an Rn->Rm value are stored tightly packed, so sample `index` of an m-component
// value starts at `index * m`.
vec2 LoadVec2(uint slot, uint index) {
    FloatBuffer source = FloatBuffer(pc.inputs[slot]);
    uint        base   = index * 2u;
    return vec2(source.values[base], source.values[base + 1u]);
}

vec3 LoadVec3(uint slot, uint index) {
    FloatBuffer source = FloatBuffer(pc.inputs[slot]);
    uint        base   = index * 3u;
    return vec3(source.values[base], source.values[base + 1u], source.values[base + 2u]);
}

void StoreVec2(uint slot, uint index, vec2 value) {
    FloatBuffer target = FloatBuffer(pc.outputs[slot]);
    uint        base   = index * 2u;
    target.values[base]      = value.x;
    target.values[base + 1u] = value.y;
}

void StoreVec3(uint slot, uint index, vec3 value) {
    FloatBuffer target = FloatBuffer(pc.outputs[slot]);
    uint        base   = index * 3u;
    target.values[base]      = value.x;
    target.values[base + 1u] = value.y;
    target.values[base + 2u] = value.z;
}

// Reinterprets a parameter word, which the engine packs as raw bits.
float ParamFloat(uint word) { return uintBitsToFloat(pc.params[word]); }
uint  ParamUint(uint word) { return pc.params[word]; }
int   ParamInt(uint word) { return int(pc.params[word]); }

#endif // ENGINE_LIB_KERNEL_GLSL
