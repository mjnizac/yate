// Graph construction and the compiler: mapping checks, source locations, constant folding, CSE,
// dead-node removal, halo propagation, dispatch counts, buffer reuse and peak VRAM
// (spec section 14, "Compiler tests").
//
// Needs no GPU: it compiles graphs and inspects the result.

#include "test_support.hpp"

#include <engine/log.hpp>
#include <engine/memory/general.hpp>
#include <engine/terrain/compiler.hpp>
#include <engine/terrain/graph.hpp>
#include <engine/terrain/kernels.hpp>

#include <string_view>

using namespace engine;
using namespace engine::terrain;

namespace {

constexpr SectionExtent kTile{.x = 256, .y = 1, .z = 256};

[[nodiscard]] SourceLocation At(u32_t line) {
    return SourceLocation{.file = "terrain.lua", .line = line};
}

[[nodiscard]] CompileOptions Options(b8_t canonicalize = true) {
    return CompileOptions{.extent = kTile, .resolution = 1.0f, .canonicalize = canonicalize};
}

/// A plain scalar noise node, the starting point of most of these graphs.
[[nodiscard]] Result<Value> AddHeight(Graph& graph, u32_t line, f32_t frequency = 0.004f) {
    Graph::NoiseParams params;
    params.frequency = frequency;
    params.octaves   = 4;
    return graph.AddNoise(params, At(line));
}

void TestMappingErrors() {
    test::Section("mapping errors are reported at the script line");
    Graph graph;

    Result<Value> height = AddHeight(graph, 10);
    REQUIRE_OK_VOID(height);

    // Normals wants the gradient, not the value. The message must name both mappings and point at
    // the line that made the mistake.
    const Result<Value> wrong = graph.AddNormals(*height, 1.0f, At(42));
    CHECK(!wrong.has_value());
    if (!wrong) {
        CHECK_EQ(std::string_view{wrong.error().Format().data()},
                 std::string_view{
                     "terrain.lua:42: [validation] Normals expects R2->R2 input, got R2->R1"});
    }

    // A slope mask has the same requirement.
    const Result<Value> badMask = graph.AddSlopeMask(*height, 0.2f, 0.6f, At(43));
    CHECK(!badMask.has_value());
    if (!badMask) {
        CHECK_EQ(badMask.error().line, u32_t{43});
        CHECK(badMask.error().stage == ErrorStage::Validation);
    }

    // Blend needs a and b to agree, and the mask to be scalar over the same domain.
    Result<Value> gradient = graph.Channel(*height, 1);
    REQUIRE_OK_VOID(gradient);
    const Result<Value> badBlend = graph.AddBlend(*height, *gradient, *height, At(44));
    CHECK(!badBlend.has_value());
    if (!badBlend) {
        CHECK_EQ(badBlend.error().line, u32_t{44});
    }

    // A channel the node does not expose.
    const Result<Value> badChannel = graph.Channel(*height, 2);
    CHECK(!badChannel.has_value());

    // Out-of-range parameters, including the two the plan tightened to strictly positive.
    Graph::NoiseParams params;
    params.persistence           = 0.0f;
    const Result<Value> noGain   = graph.AddNoise(params, At(50));
    CHECK(!noGain.has_value());
    if (!noGain) {
        CHECK_EQ(std::string_view{noGain.error().Format().data()},
                 std::string_view{"terrain.lua:50: [validation] Noise persistence must be "
                                  "strictly greater than zero, got 0"});
    }

    params.persistence              = 0.5f;
    params.lacunarity               = 0.0f;
    const Result<Value> noLacunarity = graph.AddNoise(params, At(51));
    CHECK(!noLacunarity.has_value());
    if (!noLacunarity) {
        CHECK_EQ(std::string_view{noLacunarity.error().Format().data()},
                 std::string_view{"terrain.lua:51: [validation] Noise lacunarity must be "
                                  "strictly greater than zero, got 0"});
    }

    params.lacunarity            = 2.0f;
    params.frequency             = -1.0f;
    const Result<Value> noFreq   = graph.AddNoise(params, At(52));
    CHECK(!noFreq.has_value());
    params.frequency             = 0.004f;
    params.octaves               = 0;
    const Result<Value> noOctave = graph.AddNoise(params, At(53));
    CHECK(!noOctave.has_value());
}

void TestBroadcast() {
    test::Section("broadcasting and mapping propagation");
    Graph graph;

    Result<Value> height = AddHeight(graph, 1);
    REQUIRE_OK_VOID(height);
    Result<Value> gradient = graph.Channel(*height, 1);
    REQUIRE_OK_VOID(gradient);

    CHECK((height->mapping == Mapping{Domain::R2, 1}));
    CHECK((gradient->mapping == Mapping{Domain::R2, 2}));

    // Scalar over vector broadcasts to the vector mapping, either way round.
    Result<Value> scaled = graph.AddArith(ArithOp::Multiply, *gradient, *height, At(2));
    REQUIRE_OK_VOID(scaled);
    CHECK((scaled->mapping == Mapping{Domain::R2, 2}));

    Result<Value> flipped = graph.AddArith(ArithOp::Multiply, *height, *gradient, At(3));
    REQUIRE_OK_VOID(flipped);
    CHECK((flipped->mapping == Mapping{Domain::R2, 2}));

    // Two vectors of different width cannot combine.
    Result<Value> normals = graph.AddNormals(*gradient, 1.0f, At(4));
    REQUIRE_OK_VOID(normals);
    CHECK((normals->mapping == Mapping{Domain::R2, 3}));
    const Result<Value> mismatch = graph.AddArith(ArithOp::Add, *gradient, *normals, At(5));
    CHECK(!mismatch.has_value());

    // Different domains cannot combine either.
    Graph::NoiseParams volume;
    volume.domain = Domain::R3;
    Result<Value> density = graph.AddNoise(volume, At(6));
    REQUIRE_OK_VOID(density);
    const Result<Value> crossDomain = graph.AddArith(ArithOp::Add, *height, *density, At(7));
    CHECK(!crossDomain.has_value());

    // Extraction and assembly.
    Result<Value> x = graph.AddExtract(*gradient, 0, At(8));
    REQUIRE_OK_VOID(x);
    CHECK((x->mapping == Mapping{Domain::R2, 1}));
    const Value   pair[2] = {*x, *height};
    Result<Value> combined = graph.AddCombine(pair, 2, At(9));
    REQUIRE_OK_VOID(combined);
    CHECK((combined->mapping == Mapping{Domain::R2, 2}));
    CHECK(!graph.AddExtract(*height, 1, At(10)).has_value());
}

void TestConstantFolding() {
    test::Section("constant folding");
    Graph graph;

    Result<Value> two   = graph.AddConst(Mapping{Domain::R2, 1}, {2.0f, 0, 0, 0}, At(1));
    Result<Value> three = graph.AddConst(Mapping{Domain::R2, 1}, {3.0f, 0, 0, 0}, At(2));
    REQUIRE_OK_VOID(two);
    REQUIRE_OK_VOID(three);
    Result<Value> sum = graph.AddArith(ArithOp::Add, *two, *three, At(3));
    REQUIRE_OK_VOID(sum);

    // The sum of two constants is itself a constant, which the stats report.
    Result<Value> height = AddHeight(graph, 4);
    REQUIRE_OK_VOID(height);
    Result<Value> scaled = graph.AddArith(ArithOp::Multiply, *height, *sum, At(5));
    REQUIRE_OK_VOID(scaled);
    REQUIRE_OK_VOID(graph.RequestOutput("height", *scaled, -10.0f, 10.0f));

    Result<CompiledGraph> compiled = Compile(graph, Options());
    REQUIRE_OK_VOID(compiled);
    // Const, Const and Arith(Const, Const) all fold.
    CHECK_EQ(compiled->stats.nodesFolded, u32_t{3});
    // Folding does not remove the dispatches in this version; it marks what *could* be removed.
    // What must hold is that the graph still produces the requested output.
    CHECK_EQ(compiled->outputs.size(), usize_t{1});
}

void TestCommonSubexpressionElimination() {
    test::Section("common-subexpression elimination");
    Graph graph;

    // Two identical noise nodes must run once.
    Result<Value> a = AddHeight(graph, 1);
    Result<Value> b = AddHeight(graph, 2);
    REQUIRE_OK_VOID(a);
    REQUIRE_OK_VOID(b);
    CHECK(a->node != b->node);

    Result<Value> sum = graph.AddArith(ArithOp::Add, *a, *b, At(3));
    REQUIRE_OK_VOID(sum);
    REQUIRE_OK_VOID(graph.RequestOutput("height", *sum, -2.0f, 2.0f));

    Result<CompiledGraph> compiled = Compile(graph, Options());
    REQUIRE_OK_VOID(compiled);
    CHECK_EQ(compiled->stats.nodesIn, u32_t{3});
    CHECK_EQ(compiled->stats.nodesEliminated, u32_t{1});
    CHECK_EQ(compiled->stats.dispatchCount, u32_t{2});

    // A different frequency is a different node, so nothing is merged.
    Graph         distinct;
    Result<Value> c = AddHeight(distinct, 1, 0.004f);
    Result<Value> d = AddHeight(distinct, 2, 0.008f);
    REQUIRE_OK_VOID(c);
    REQUIRE_OK_VOID(d);
    Result<Value> other = distinct.AddArith(ArithOp::Add, *c, *d, At(3));
    REQUIRE_OK_VOID(other);
    REQUIRE_OK_VOID(distinct.RequestOutput("height", *other, -2.0f, 2.0f));
    Result<CompiledGraph> compiledDistinct = Compile(distinct, Options());
    REQUIRE_OK_VOID(compiledDistinct);
    CHECK_EQ(compiledDistinct->stats.nodesEliminated, u32_t{0});
    CHECK_EQ(compiledDistinct->stats.dispatchCount, u32_t{3});

    // With canonicalization off, the duplicate survives: that is what makes the flag useful for
    // telling a compiler bug apart from an op bug.
    Result<CompiledGraph> raw = Compile(graph, Options(false));
    REQUIRE_OK_VOID(raw);
    CHECK_EQ(raw->stats.nodesEliminated, u32_t{0});
    CHECK_EQ(raw->stats.dispatchCount, u32_t{3});
}

void TestDeadNodeRemoval() {
    test::Section("dead-node removal");
    Graph graph;

    Result<Value> height = AddHeight(graph, 1);
    REQUIRE_OK_VOID(height);
    Result<Value> gradient = graph.Channel(*height, 1);
    REQUIRE_OK_VOID(gradient);

    // Built but never requested: a mask and a curve that reach no output.
    Result<Value> mask = graph.AddSlopeMask(*gradient, 0.1f, 0.5f, At(2));
    REQUIRE_OK_VOID(mask);
    Result<Value> unusedCurve = graph.AddCurve(CurveOp::Power, *mask, 2.0f, 0.0f, At(3));
    REQUIRE_OK_VOID(unusedCurve);

    REQUIRE_OK_VOID(graph.RequestOutput("height", *height, -1.0f, 1.0f));

    Result<CompiledGraph> compiled = Compile(graph, Options());
    REQUIRE_OK_VOID(compiled);
    CHECK_EQ(compiled->stats.nodesIn, u32_t{3});
    CHECK_EQ(compiled->stats.nodesDead, u32_t{2});
    CHECK_EQ(compiled->stats.dispatchCount, u32_t{1});

    // The gradient channel nothing asks for is not written either.
    CHECK_EQ(compiled->dispatches.size(), usize_t{1});
    CHECK_EQ(compiled->dispatches[0].channelMask, u8_t{0x1});
    // One channel written means one buffer, not two.
    CHECK_EQ(compiled->buffers.size(), usize_t{1});
}

void TestChannelMask() {
    test::Section("channel masks");
    Graph graph;

    Result<Value> height = AddHeight(graph, 1);
    REQUIRE_OK_VOID(height);
    Result<Value> gradient = graph.Channel(*height, 1);
    REQUIRE_OK_VOID(gradient);
    Result<Value> normals = graph.AddNormals(*gradient, 1.0f, At(2));
    REQUIRE_OK_VOID(normals);

    REQUIRE_OK_VOID(graph.RequestOutput("height", *height, -1.0f, 1.0f));
    REQUIRE_OK_VOID(graph.RequestOutput("normals", *normals, -1.0f, 1.0f));

    Result<CompiledGraph> compiled = Compile(graph, Options());
    REQUIRE_OK_VOID(compiled);
    CHECK_EQ(compiled->stats.dispatchCount, u32_t{2});
    // Both channels of the noise node are needed now.
    CHECK_EQ(compiled->dispatches[0].channelMask, u8_t{0x3});
    CHECK_EQ(compiled->dispatches[1].channelMask, u8_t{0x1});
    CHECK_EQ(compiled->buffers.size(), usize_t{3});

    // Peak equals the three live slots: R2->R1, R2->R2 and R2->R3 on a 256 tile, each rounded up
    // to its power-of-two size class.
    const u64_t scalar = ClassOf(Mapping{Domain::R2, 1}, kTile, 0).slotSize;
    const u64_t vector = ClassOf(Mapping{Domain::R2, 2}, kTile, 0).slotSize;
    const u64_t normal = ClassOf(Mapping{Domain::R2, 3}, kTile, 0).slotSize;
    CHECK_EQ(compiled->stats.peakSectionBytes, scalar + vector + normal);
}

void TestHaloPropagation() {
    test::Section("halo propagation");
    // Every op in this version is pointwise, so a correct implementation must produce zero halo
    // everywhere. That is the property to pin down now: a neighbourhood op added later should make
    // this test fail loudly rather than quietly change behaviour.
    Graph graph;

    Result<Value> height = AddHeight(graph, 1);
    REQUIRE_OK_VOID(height);
    Result<Value> gradient = graph.Channel(*height, 1);
    REQUIRE_OK_VOID(gradient);
    Result<Value> mask = graph.AddSlopeMask(*gradient, 0.1f, 0.5f, At(2));
    REQUIRE_OK_VOID(mask);
    Result<Value> blended = graph.AddBlend(*height, *height, *mask, At(3));
    REQUIRE_OK_VOID(blended);
    REQUIRE_OK_VOID(graph.RequestOutput("height", *blended, -1.0f, 1.0f));

    Result<CompiledGraph> compiled = Compile(graph, Options());
    REQUIRE_OK_VOID(compiled);
    CHECK_EQ(compiled->stats.maxHalo, u32_t{0});
    for (const Dispatch& dispatch : compiled->dispatches) {
        CHECK_EQ(dispatch.halo, u32_t{0});
        CHECK(dispatch.nodeClass == NodeClass::Pointwise);
        CHECK(OpInfoOf(dispatch.kind).nodeClass == NodeClass::Pointwise);
    }
    for (const PlannedBuffer& buffer : compiled->buffers) {
        CHECK_EQ(buffer.halo, u32_t{0});
        CHECK(buffer.bytes == ValueSize(buffer.mapping, kTile, 0));
        CHECK(buffer.sizeClass.slotSize >= buffer.bytes);
    }
}

void TestBufferReuse() {
    test::Section("buffer reuse and peak VRAM");
    Graph graph;

    // A chain of same-mapping curves: each link can hand its buffer to the next, so a correct
    // planner needs two buffers no matter how long the chain is.
    Result<Value> value = AddHeight(graph, 1);
    REQUIRE_OK_VOID(value);
    Value current = *value;
    for (u32_t i = 0; i < 6; ++i) {
        Result<Value> next =
            graph.AddCurve(CurveOp::Power, current, 1.5f + static_cast<f32_t>(i), 0.0f, At(10 + i));
        REQUIRE_OK_VOID(next);
        current = *next;
    }
    REQUIRE_OK_VOID(graph.RequestOutput("height", current, -1.0f, 1.0f));

    Result<CompiledGraph> compiled = Compile(graph, Options());
    REQUIRE_OK_VOID(compiled);
    CHECK_EQ(compiled->stats.dispatchCount, u32_t{7});

    const u64_t scalar = ClassOf(Mapping{Domain::R2, 1}, kTile, 0).slotSize;
    CHECK_EQ(compiled->buffers.size(), usize_t{2});
    CHECK_EQ(compiled->stats.peakSectionBytes, 2 * scalar);

    // Consecutive links must not share a buffer: a kernel cannot read and write the same slot.
    for (usize_t i = 1; i < compiled->dispatches.size(); ++i) {
        const Dispatch& dispatch = compiled->dispatches[i];
        CHECK(dispatch.inputBuffers[0] != dispatch.outputBuffers[0]);
    }
    // But the chain does alternate, which is what proves the buffers are being recycled.
    CHECK_EQ(compiled->dispatches[1].outputBuffers[0], compiled->dispatches[3].outputBuffers[0]);
}

void TestOutputValidation() {
    test::Section("output validation");
    Graph graph;

    // No outputs at all.
    Result<CompiledGraph> empty = Compile(graph, Options());
    CHECK(!empty.has_value());

    Result<Value> height = AddHeight(graph, 1);
    REQUIRE_OK_VOID(height);

    // An empty range on a scalar output.
    CHECK(!graph.RequestOutput("height", *height, 1.0f, 1.0f).has_value());
    REQUIRE_OK_VOID(graph.RequestOutput("height", *height, -1.0f, 1.0f));
    // The same name twice.
    CHECK(!graph.RequestOutput("height", *height, -1.0f, 1.0f).has_value());
    // An unnamed output.
    CHECK(!graph.RequestOutput("", *height, -1.0f, 1.0f).has_value());

    // A four-component value has no PNG form, so it must be rejected at compile time.
    Graph         wide;
    Result<Value> quad =
        wide.AddConst(Mapping{Domain::R2, 4}, {1.0f, 2.0f, 3.0f, 4.0f}, At(1));
    REQUIRE_OK_VOID(quad);
    REQUIRE_OK_VOID(wide.RequestOutput("quad", *quad, 0.0f, 1.0f));
    Result<CompiledGraph> compiledWide = Compile(wide, Options());
    CHECK(!compiledWide.has_value());
}

} // namespace

int main() {
    REQUIRE_OK(log::Init(log::Config{}));
    REQUIRE_OK(memory::Init());

    TestMappingErrors();
    TestBroadcast();
    TestConstantFolding();
    TestCommonSubexpressionElimination();
    TestDeadNodeRemoval();
    TestChannelMask();
    TestHaloPropagation();
    TestBufferReuse();
    TestOutputValidation();

    memory::Shutdown();
    log::Shutdown();
    return test::Summary("test_compiler");
}
