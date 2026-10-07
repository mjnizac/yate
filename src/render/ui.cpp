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

#include <array>
#include <cstdio>
#include <cstring>
#include <new>

namespace engine::render {
namespace {

/// Enough for the panels: ImGui allocates one set per texture it draws, and the viewer draws one.
constexpr u32_t kDescriptorCount = 8;

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
                      VkRenderPass renderPass, u32_t imageCount) {
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
    ImGui::StyleColorsDark();

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
    vulkanInfo.MSAASamples    = VK_SAMPLE_COUNT_1_BIT;
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

b8_t Ui::Draw(Panels& panels) {
    if (!IsValid()) {
        return false;
    }
    b8_t changed = false;

    ImGui::SetNextWindowPos(ImVec2(12.0f, 12.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(340.0f, 0.0f), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Terrain", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(panels.scriptPath);
        ImGui::Text("%u of %u tiles drawn, %u streaming", panels.tilesDrawn, panels.tileCount,
                    panels.missingTiles);
        ImGui::Text("level %u, %.2f m per sample%s", panels.level,
                    static_cast<f64_t>(panels.spacing), panels.wireframe ? ", wireframe" : "");
        ImGui::Text("%.1f fps (%.2f ms)", panels.framesPerSecond, panels.frameMilliseconds);
        ImGui::Text("load %llu", static_cast<unsigned long long>(panels.loadCount));
        ImGui::Separator();

        ImGui::TextUnformatted(panels.cameraMode);
        ImGui::Text("eye %.0f, %.0f, %.0f", static_cast<f64_t>(panels.eye[0]),
                    static_cast<f64_t>(panels.eye[1]), static_cast<f64_t>(panels.eye[2]));

        // The parameter list is generated from whatever the script was given, which is the whole of
        // "auto-generated": the engine does not know what a parameter means, only its name and its text,
        // so a numeric one gets a drag field and anything else gets a text box.
        if (panels.params != nullptr && panels.params->Count() != 0) {
            ImGui::Separator();
            ImGui::TextUnformatted("Parameters");
            for (usize_t i = 0; i < panels.params->Count(); ++i) {
                const std::string_view key  = panels.params->KeyAt(i);
                const std::string_view text = panels.params->ValueAt(i);

                std::array<char, 64> label{};
                detail::CopyBounded(label, key);

                char*       end    = nullptr;
                std::array<char, 64> value{};
                detail::CopyBounded(value, text);
                const f64_t number = std::strtod(value.data(), &end);
                const b8_t  numeric = end != nullptr && *end == 0 && value[0] != 0;

                ImGui::PushID(static_cast<int>(i));
                if (numeric) {
                    f32_t editable = static_cast<f32_t>(number);
                    // Step scaled to the value, so a frequency of 0.002 and an amplitude of 400 are both
                    // adjustable with the same gesture.
                    const f32_t step =
                        std::max(std::abs(editable) * 0.01f, 1e-4f);
                    if (ImGui::DragFloat(label.data(), &editable, step)) {
                        std::array<char, 64> updated{};
                        std::snprintf(updated.data(), updated.size(), "%g",
                                      static_cast<f64_t>(editable));
                        (void)panels.params->Set(key, std::string_view{updated.data()});
                        changed = true;
                    }
                } else {
                    std::array<char, 64> editable{};
                    detail::CopyBounded(editable, text);
                    if (ImGui::InputText(label.data(), editable.data(), editable.size(),
                                         ImGuiInputTextFlags_EnterReturnsTrue)) {
                        (void)panels.params->Set(key, std::string_view{editable.data()});
                        changed = true;
                    }
                }
                ImGui::PopID();
            }
            if (ImGui::Button("Reload")) {
                changed = true;
            }
        }
    }
    ImGui::End();

    // The error panel is the spec's requirement that an error reaches the person looking at the screen
    // and not only the log (spec section 11). Red, pinned to the bottom, and only present when there is
    // something to say.
    if (panels.error != nullptr && panels.error[0] != 0) {
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(ImVec2(12.0f, viewport->WorkSize.y - 12.0f), ImGuiCond_Always,
                                ImVec2(0.0f, 1.0f));
        ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x - 24.0f, 0.0f), ImGuiCond_Always);
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
