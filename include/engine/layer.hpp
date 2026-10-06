#pragma once

#include <engine/common.hpp>

namespace engine {

class Application;

/// One slice of per-frame (viewer) or per-tick (export) work.
/// Layers are owned by the application and are neither copyable nor movable.
class ENGINE_API Layer {
public:
    explicit Layer(Application& application) noexcept : m_application(application) {}
    virtual ~Layer() = default;

    ENGINE_NO_COPY(Layer);
    ENGINE_NO_MOVE(Layer);

    /// Called once, in push order, before the first update.
    virtual void OnAttach() {}

    /// Called once, in reverse push order, during shutdown.
    virtual void OnDetach() {}

    /// Called every frame or tick, in push order.
    virtual void OnUpdate() {}

    /// Stable name used for log entries and Tracy zones.
    [[nodiscard]] virtual const char* Name() const noexcept = 0;

protected:
    [[nodiscard]] Application& App() const noexcept { return m_application; }

private:
    Application& m_application;
};

} // namespace engine
