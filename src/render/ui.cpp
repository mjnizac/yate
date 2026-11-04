// The viewer's panels: script parameters, errors and what the frame cost.
//
// Dear ImGui, confined to this translation unit the way Sol2 is confined to `lua.cpp`: no ImGui type
// appears in a header, so adding or replacing the UI library touches one file.
//
// The backend manages its own descriptor sets, its own font atlas and its own staging, which is the
// reason it was chosen over hand-rolling panels: the engine has no descriptor sets, no samplers and no
// image upload path, and the UI is the only thing that would have needed all three.

#include <engine/render/ui.hpp>

#include <engine/log.hpp>
#include <engine/memory/general.hpp>
#include <engine/terrain/export.hpp>
#include <engine/vulkan/context.hpp>
#include <engine/vulkan/window.hpp>

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <new>

namespace engine::render {
namespace {

/// Enough for the panels: ImGui allocates one set per texture it draws, and the viewer draws one.
constexpr u32_t kDescriptorCount = 8;

/// Ids the legend of a shown output lists at most.
constexpr i32_t kMaxLegendIds = 32;

/// The colour of an id, as `IdColour` in `viewer/terrain.frag.glsl` paints it: hues a golden angle apart.
[[nodiscard]] std::array<f32_t, 3> IdColour(i32_t id) {
    const f32_t hue = static_cast<f32_t>(id) * 0.618034f - std::floor(static_cast<f32_t>(id) * 0.618034f);
    std::array<f32_t, 3> colour{};
    for (usize_t c = 0; c < colour.size(); ++c) {
        const f32_t shifted = hue + std::array<f32_t, 3>{0.0f, 2.0f / 3.0f, 1.0f / 3.0f}[c];
        colour[c] = std::clamp(std::abs((shifted - std::floor(shifted)) * 6.0f - 3.0f) - 1.0f, 0.0f, 1.0f) * 0.6f + 0.25f;
    }
    return colour;
}

/// Width of the docked sidebar in pixels, at most half the window.
constexpr f32_t kSidebarWidth = 380.0f;

/// The look: the dark theme with rounded controls, roomier spacing and one blue accent, so the panel reads
/// as a settings pane and not as a debug overlay.
void ApplyStyle() {
    ImGui::StyleColorsDark();
    ImGuiStyle& style      = ImGui::GetStyle();
    style.WindowRounding   = 0.0f;
    style.FrameRounding    = 4.0f;
    style.GrabRounding     = 4.0f;
    style.TabRounding      = 4.0f;
    style.PopupRounding    = 4.0f;
    style.WindowPadding    = ImVec2(12.0f, 10.0f);
    style.FramePadding     = ImVec2(8.0f, 4.0f);
    style.ItemSpacing      = ImVec2(8.0f, 6.0f);
    style.WindowBorderSize = 0.0f;
    ImVec4* colours                 = style.Colors;
    colours[ImGuiCol_WindowBg]      = ImVec4(0.09f, 0.10f, 0.12f, 0.94f);
    colours[ImGuiCol_TitleBgActive] = ImVec4(0.12f, 0.14f, 0.17f, 1.00f);
    colours[ImGuiCol_TitleBg]       = colours[ImGuiCol_TitleBgActive];
    colours[ImGuiCol_FrameBg]       = ImVec4(0.16f, 0.18f, 0.22f, 1.00f);
    colours[ImGuiCol_Tab]           = ImVec4(0.14f, 0.16f, 0.20f, 1.00f);
    colours[ImGuiCol_TabActive]     = ImVec4(0.20f, 0.42f, 0.70f, 1.00f);
    colours[ImGuiCol_TabHovered]    = ImVec4(0.26f, 0.50f, 0.80f, 1.00f);
    colours[ImGuiCol_Header]        = ImVec4(0.20f, 0.42f, 0.70f, 0.55f);
    colours[ImGuiCol_SliderGrab]    = ImVec4(0.30f, 0.56f, 0.90f, 1.00f);
    colours[ImGuiCol_CheckMark]     = colours[ImGuiCol_SliderGrab];
    colours[ImGuiCol_Button]        = ImVec4(0.20f, 0.42f, 0.70f, 0.70f);
    // The swapchain is sRGB and ImGui's colours are sRGB too: written as they are they come out brighter,
    // which made the panel a washed-out grey. Linearised, they come out as chosen.
    for (ImVec4& colour : style.Colors) {
        colour = ImVec4(std::pow(colour.x, 2.2f), std::pow(colour.y, 2.2f), std::pow(colour.z, 2.2f), colour.w);
    }
}

/// A section heading inside a tab.
void Section(const char* title) {
    ImGui::Spacing();
    ImGui::SeparatorText(title);
}

/// Half the side of a map made from the panel when there was none, in metres round the eye.
constexpr f32_t kNewMapHalf = 4096.0f;

/// World Machine's World Dimensions: the map as an origin and a size, with a square lock, what it comes
/// to in samples, and a button that puts it all on screen. Only which tiles are drawn changes: no reload.
void MapSection(Ui::Panels& panels) {
    Section("Map");
    if (ImGui::Checkbox("Bounded to a map", &panels.bounded) && panels.bounded
        && !(panels.bounds[0] < panels.bounds[2] && panels.bounds[1] < panels.bounds[3])) {
        panels.bounds = {panels.eye[0] - kNewMapHalf, panels.eye[2] - kNewMapHalf, panels.eye[0] + kNewMapHalf,
                         panels.eye[2] + kNewMapHalf};
    }
    if (!panels.bounded) {
        ImGui::TextDisabled("an endless ring round the camera");
        return;
    }
    std::array<f32_t, 2> centre{(panels.bounds[0] + panels.bounds[2]) * 0.5f, (panels.bounds[1] + panels.bounds[3]) * 0.5f};
    std::array<f32_t, 2> size{panels.bounds[2] - panels.bounds[0], panels.bounds[3] - panels.bounds[1]};
    const f32_t          step = std::max(std::max(size[0], size[1]) * 0.002f, 1.0f);
    b8_t edited = ImGui::DragFloat2("Centre x, z (m)", centre.data(), step, 0.0f, 0.0f, "%.0f");
    edited      = ImGui::Checkbox("Square", &panels.squareMap) || edited;
    if (panels.squareMap) {
        edited  = ImGui::DragFloat("Side (m)", &size[0], step, 1.0f, 1e7f, "%.0f", ImGuiSliderFlags_AlwaysClamp) || edited;
        size[1] = size[0];
    } else {
        edited = ImGui::DragFloat2("Size x, z (m)", size.data(), step, 1.0f, 1e7f, "%.0f", ImGuiSliderFlags_AlwaysClamp) || edited;
    }
    // Written back only when edited: a round trip through the centre and the size every frame would let
    // the bounds drift by a rounding at a time.
    if (edited) {
        panels.bounds = {centre[0] - size[0] * 0.5f, centre[1] - size[1] * 0.5f, centre[0] + size[0] * 0.5f,
                         centre[1] + size[1] * 0.5f};
    }
    // World Machine's "Total dimensions: W x H, m/pixel", at the finest spacing.
    ImGui::Text("%.2f x %.2f km, %.0f x %.0f samples at %g m", static_cast<f64_t>(size[0]) / 1000.0,
                static_cast<f64_t>(size[1]) / 1000.0, std::floor(static_cast<f64_t>(size[0] / panels.resolution)) + 1.0,
                std::floor(static_cast<f64_t>(size[1] / panels.resolution)) + 1.0, static_cast<f64_t>(panels.resolution));
    panels.frameMap = ImGui::Button("Frame map");
}

/// World Machine's Scene Setup: the map, the spacing the script is run at, its heights and its outputs.
[[nodiscard]] b8_t WorldTab(Ui::Panels& panels) {
    b8_t changed = false;
    MapSection(panels);
    Section("Resolution");
    // Powers of two only: a level's lattice is then a subset of the finer one's, as for the levels. The
    // finest spacing reaches the script (it sees the spacing), so a change reloads.
    i32_t exponent = static_cast<i32_t>(std::lround(std::log2(std::max(panels.resolution, 0.125f))));
    std::array<char, 32> spacing{};
    std::snprintf(spacing.data(), spacing.size(), "%g m", std::exp2(static_cast<f64_t>(exponent)));
    if (ImGui::SliderInt("Finest", &exponent, -3, 14, spacing.data(), ImGuiSliderFlags_AlwaysClamp)) {
        panels.resolution = static_cast<f32_t>(std::exp2(static_cast<f64_t>(exponent)));
        changed           = true;
    }
    ImGui::Text("drawn at level %u: %g m per sample", panels.level, static_cast<f64_t>(panels.spacing));

    Section("Heights");
    ImGui::Text("%.0f .. %.0f m, as the script declares", static_cast<f64_t>(panels.heightRange[0]),
                static_cast<f64_t>(panels.heightRange[1]));

    Section("Outputs");
    for (u32_t i = 0; i < panels.outputCount; ++i) {
        ImGui::BulletText("%s  %g .. %g", panels.outputs[i], static_cast<f64_t>(panels.outputRanges[i][0]),
                          static_cast<f64_t>(panels.outputRanges[i][1]));
    }
    return changed;
}

/// The camera and how the ring of tiles is laid round it: none of it reaches the script.
void ViewTab(Ui::Panels& panels) {
    Section("Camera");
    ImGui::TextUnformatted(panels.cameraMode);
    ImGui::Text("eye %.0f, %.0f, %.0f", static_cast<f64_t>(panels.eye[0]), static_cast<f64_t>(panels.eye[1]),
                static_cast<f64_t>(panels.eye[2]));
    ImGui::Checkbox("Plan view (top-down, no perspective)", &panels.plan);
    ImGui::SliderFloat("Map scale", &panels.mapScale, 0.25f, 16.0f, "x%.2f",
                       ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp);

    Section("Sun");
    ImGui::SliderFloat("Azimuth", &panels.sun[0], 0.0f, 360.0f, "%.0f deg");
    ImGui::SliderFloat("Elevation", &panels.sun[1], 1.0f, 90.0f, "%.0f deg");

    Section("Tiles");
    // Rings of detail beyond the finest, each a level coarser and twice as far: no reload.
    ImGui::SliderInt("Detail rings", &panels.maxLevel, 0, 8, "%d", ImGuiSliderFlags_AlwaysClamp);
    ImGui::SliderInt("Tiles each way", &panels.ringRadius, 1, 16, "%d", ImGuiSliderFlags_AlwaysClamp);
}

/// The ids' colours of a shown output, as many as its range holds, up to `kMaxLegendIds`.
void IdLegend(const Ui::Panels& panels) {
    const i32_t first = static_cast<i32_t>(std::ceil(panels.shownRange[0]));
    const i32_t last  = std::min(static_cast<i32_t>(std::floor(panels.shownRange[1])), first + kMaxLegendIds - 1);
    for (i32_t id = first; id <= last; ++id) {
        const std::array<f32_t, 3> colour = IdColour(id);
        std::array<char, 16>       label{};
        std::snprintf(label.data(), label.size(), "%d", id);
        if ((id - first) % 8 != 0) {
            ImGui::SameLine();
        }
        ImGui::ColorButton(label.data(), ImVec4{colour[0], colour[1], colour[2], 1.0f}, ImGuiColorEditFlags_NoTooltip,
                           ImVec2{14.0f, 14.0f});
        ImGui::SameLine();
        ImGui::TextUnformatted(label.data());
    }
}

/// What is drawn over the relief: water, a shown output and the guide image. Viewer state, no reload.
void DisplayTab(Ui::Panels& panels) {
    Section("Water");
    ImGui::Checkbox("Water", &panels.waterEnabled);
    if (panels.waterEnabled) {
        // The level is the sea's; lakes and rivers carry their own surfaces from the script.
        ImGui::Checkbox("Sea", &panels.waterSea);
        ImGui::SameLine();
        ImGui::Checkbox("Lakes", &panels.waterLakes);
        ImGui::SameLine();
        ImGui::Checkbox("Rivers", &panels.waterRivers);
        ImGui::DragFloat("Sea level (m)", &panels.waterLevel, 1.0f);
        ImGui::ColorEdit3("Colour", panels.waterColour.data());
        ImGui::SliderFloat("Opacity", &panels.waterOpacity, 0.0f, 1.0f);
    }

    // Any R2->R1 output painted over the terrain: a ramp over its declared range, or a colour an id.
    Section("Show output");
    const char* current = panels.shown >= 0 ? panels.outputs[static_cast<usize_t>(panels.shown)] : "(none)";
    if (ImGui::BeginCombo("Output", current)) {
        if (ImGui::Selectable("(none)", panels.shown < 0)) {
            panels.shown = -1;
        }
        for (u32_t i = 0; i < panels.outputCount; ++i) {
            if (ImGui::Selectable(panels.outputs[i], panels.shown == static_cast<i32_t>(i))) {
                panels.shown = static_cast<i32_t>(i);
            }
        }
        ImGui::EndCombo();
    }
    if (panels.shown >= 0) {
        ImGui::Checkbox("As ids", &panels.showIds);
        ImGui::SameLine();
        ImGui::Text("%g .. %g", static_cast<f64_t>(panels.shownRange[0]), static_cast<f64_t>(panels.shownRange[1]));
        if (panels.showIds) {
            IdLegend(panels);
        }
    }

    if (panels.guide) {
        Section("Guide image");
        ImGui::SliderFloat("Opacity##guide", &panels.guideOpacity, 0.0f, 1.0f, "%.2f");
    }
}

/// The script's parameters, generated from whatever it was given: the engine does not know what a
/// parameter means, only its name and its text, so a numeric one gets a drag field and anything else a
/// text box. Returns true when one changed or a reload was asked for.
[[nodiscard]] b8_t ScriptTab(Ui::Panels& panels) {
    b8_t changed = ImGui::Button("Reload (R)");
    ImGui::SameLine();
    ImGui::TextDisabled("load %llu", static_cast<unsigned long long>(panels.loadCount));
    Section("Parameters");
    if (panels.params == nullptr || panels.params->Count() == 0) {
        ImGui::TextDisabled("none given (--param key=value)");
        return changed;
    }
    for (usize_t i = 0; i < panels.params->Count(); ++i) {
        const std::string_view key  = panels.params->KeyAt(i);
        const std::string_view text = panels.params->ValueAt(i);

        std::array<char, 64> label{};
        detail::CopyBounded(label, key);
        std::array<char, 64> value{};
        detail::CopyBounded(value, text);
        char*       end     = nullptr;
        const f64_t number  = std::strtod(value.data(), &end);
        const b8_t  numeric = end != nullptr && *end == 0 && value[0] != 0;

        ImGui::PushID(static_cast<int>(i));
        if (numeric) {
            f32_t editable = static_cast<f32_t>(number);
            // Step scaled to the value, so a frequency of 0.002 and an amplitude of 400 are both
            // adjustable with the same gesture.
            const f32_t step = std::max(std::abs(editable) * 0.01f, 1e-4f);
            if (ImGui::DragFloat(label.data(), &editable, step)) {
                std::array<char, 64> updated{};
                std::snprintf(updated.data(), updated.size(), "%g", static_cast<f64_t>(editable));
                (void)panels.params->Set(key, std::string_view{updated.data()});
                changed = true;
            }
        } else if (ImGui::InputText(label.data(), value.data(), value.size(), ImGuiInputTextFlags_EnterReturnsTrue)) {
            (void)panels.params->Set(key, std::string_view{value.data()});
            changed = true;
        }
        ImGui::PopID();
    }
    return changed;
}

/// One line along the bottom: what the frame costs and what the ring is doing.
void StatusBar(const Ui::Panels& panels, f32_t height) {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x, viewport->WorkPos.y + viewport->WorkSize.y - height));
    ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x, height));
    if (ImGui::Begin("status", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoFocusOnAppearing)) {
        ImGui::Text("%.0f fps  %.2f ms   |   %u of %u tiles drawn, %u streaming   |   level %u, %g m/sample   |   %s%s",
                    static_cast<f64_t>(panels.framesPerSecond), static_cast<f64_t>(panels.frameMilliseconds),
                    panels.tilesDrawn, panels.tileCount, panels.missingTiles, panels.level,
                    static_cast<f64_t>(panels.spacing), panels.plan ? "plan view" : panels.cameraMode,
                    panels.wireframe ? ", wireframe" : "");
    }
    ImGui::End();
}

/// ImGui allocates through the engine's general allocator rather than `malloc`, so it shows up as part
/// of `CPU/General` instead of swelling the untracked `operator new` pool.
///
/// The size is not passed to the free function, so the allocation is prefixed with its own size. The
/// prefix is the alignment the allocator was asked for, which keeps the returned pointer aligned.
constexpr usize_t kAllocationPrefix = alignof(std::max_align_t);

void* UiAllocate(usize_t size, void* /*user*/) {
    auto* base = static_cast<u8_t*>(
        memory::General().Allocate(size + kAllocationPrefix, kAllocationPrefix));
    if (base == nullptr) {
        return nullptr;
    }
    const usize_t total = size + kAllocationPrefix;
    std::memcpy(base, &total, sizeof(total));
    return base + kAllocationPrefix;
}

void UiFree(void* pointer, void* /*user*/) {
    if (pointer == nullptr) {
        return;
    }
    auto* base = static_cast<u8_t*>(pointer) - kAllocationPrefix;
    memory::General().Free(base);
}

void CheckVulkanResult(VkResult result) {
    if (result != VK_SUCCESS) {
        LOG_ERROR("the UI backend reported {}", vulkan::ResultName(result));
    }
}

} // namespace

struct Ui::State {
    VkDevice         device         = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    b8_t             initialized    = false;
};

Ui::~Ui() { Shutdown(); }

Status Ui::Initialize(vulkan::Context& context, const vulkan::Window& window,
                      VkRenderPass renderPass, u32_t imageCount, VkSampleCountFlagBits samples) {
    Shutdown();

    void* storage = memory::General().Allocate(sizeof(State), alignof(State));
    if (storage == nullptr) {
        ENGINE_FAIL(ErrorCode::OutOfMemory, ErrorStage::Init, "could not allocate the UI state");
    }
    m_state         = ::new (storage) State{};
    m_state->device = context.Device();

    // Set before any context exists, so even the context itself comes from the engine's allocator.
    ImGui::SetAllocatorFunctions(UiAllocate, UiFree, nullptr);
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    // No .ini: the viewer has one layout and writing a file next to the executable would be a surprise.
    io.IniFilename  = nullptr;
    io.LogFilename  = nullptr;
    ApplyStyle();

    const std::array<VkDescriptorPoolSize, 1> sizes{
        VkDescriptorPoolSize{.type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                             .descriptorCount = kDescriptorCount}};
    const VkDescriptorPoolCreateInfo poolInfo{
        .sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
        .maxSets       = kDescriptorCount,
        .poolSizeCount = static_cast<u32_t>(sizes.size()),
        .pPoolSizes    = sizes.data()};
    VK_TRY(vkCreateDescriptorPool(m_state->device, &poolInfo, nullptr, &m_state->descriptorPool));

    if (!ImGui_ImplGlfw_InitForVulkan(static_cast<GLFWwindow*>(window.Handle()), true)) {
        ENGINE_FAIL(ErrorCode::InternalError, ErrorStage::Init,
                    "the UI backend could not attach to the window");
    }

    ImGui_ImplVulkan_InitInfo vulkanInfo{};
    vulkanInfo.Instance       = context.Instance();
    vulkanInfo.PhysicalDevice = context.PhysicalDevice();
    vulkanInfo.Device         = m_state->device;
    vulkanInfo.QueueFamily    = context.GraphicsQueue().Family();
    vulkanInfo.Queue          = context.GraphicsQueue().Handle();
    vulkanInfo.DescriptorPool = m_state->descriptorPool;
    vulkanInfo.RenderPass     = renderPass;
    vulkanInfo.MinImageCount  = imageCount;
    vulkanInfo.ImageCount     = imageCount;
    vulkanInfo.MSAASamples    = samples;
    vulkanInfo.PipelineCache  = context.Pipelines().Handle();
    vulkanInfo.CheckVkResultFn = CheckVulkanResult;
    if (!ImGui_ImplVulkan_Init(&vulkanInfo)) {
        ENGINE_FAIL(ErrorCode::InternalError, ErrorStage::Init,
                    "the UI backend could not create its Vulkan resources");
    }

    m_state->initialized = true;
    LOG_INFO("UI ready (Dear ImGui {})", IMGUI_VERSION);
    return {};
}

void Ui::Shutdown() {
    if (m_state == nullptr) {
        return;
    }
    if (m_state->initialized) {
        ImGui_ImplVulkan_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
    }
    if (m_state->descriptorPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(m_state->device, m_state->descriptorPool, nullptr);
    }
    m_state->~State();
    memory::General().Free(m_state);
    m_state = nullptr;
}

b8_t Ui::IsValid() const noexcept { return m_state != nullptr && m_state->initialized; }

void Ui::Begin() {
    if (!IsValid()) {
        return;
    }
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
}

f32_t SidebarWidth(f32_t windowWidth) noexcept { return std::min(kSidebarWidth, windowWidth * 0.5f); }

b8_t Ui::Draw(Panels& panels) {
    if (!IsValid()) {
        return false;
    }
    b8_t changed = false;

    // A sidebar docked to the right, the full height above the status bar, as the settings pane of a
    // modern tool is: fixed, so it never wanders over the terrain, and tabbed after World Machine's
    // project dialog (its world, the view, what is drawn over it, and the script).
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const f32_t          status   = ImGui::GetFrameHeight() + 2.0f * ImGui::GetStyle().WindowPadding.y;
    const f32_t          width    = SidebarWidth(viewport->WorkSize.x);
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - width, viewport->WorkPos.y));
    ImGui::SetNextWindowSize(ImVec2(width, viewport->WorkSize.y - status));
    if (ImGui::Begin("YATE", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse)) {
        ImGui::TextDisabled("%s", panels.scriptPath);
        if (ImGui::BeginTabBar("tabs")) {
            if (ImGui::BeginTabItem("World")) {
                changed = WorldTab(panels) || changed;
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("View")) {
                ViewTab(panels);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Display")) {
                DisplayTab(panels);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Script")) {
                changed = ScriptTab(panels) || changed;
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }
    ImGui::End();
    StatusBar(panels, status);

    // The error panel is the spec's requirement that an error reaches the person looking at the screen
    // and not only the log (spec section 11). Red, over the status bar left of the sidebar, and only
    // present when there is something to say.
    if (panels.error != nullptr && panels.error[0] != 0) {
        ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + 12.0f, viewport->WorkPos.y + viewport->WorkSize.y - status - 12.0f),
                                ImGuiCond_Always, ImVec2(0.0f, 1.0f));
        ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x - width - 24.0f, 0.0f), ImGuiCond_Always);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.30f, 0.06f, 0.06f, 0.92f));
        if (ImGui::Begin("Script error", nullptr,
                         ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar
                             | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) {
            ImGui::TextWrapped("%s", panels.error);
            ImGui::TextUnformatted("the previous terrain is still on screen");
        }
        ImGui::End();
        ImGui::PopStyleColor();
    }

    return changed;
}

void Ui::Record(VkCommandBuffer commands) {
    if (!IsValid()) {
        return;
    }
    ImGui::Render();
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), commands);
}

b8_t Ui::WantsMouse() const {
    return IsValid() && ImGui::GetIO().WantCaptureMouse;
}

b8_t Ui::WantsKeyboard() const {
    return IsValid() && ImGui::GetIO().WantCaptureKeyboard;
}

} // namespace engine::render
