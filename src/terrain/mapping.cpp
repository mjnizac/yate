#include <engine/terrain/mapping.hpp>

namespace engine::terrain {

const char* ToString(Mapping mapping) noexcept {
    // One literal per valid mapping, so the spelling is a stable pointer with no formatting cost.
    if (mapping.domain == Domain::R2) {
        switch (mapping.components) {
            case 1: return "R2->R1";
            case 2: return "R2->R2";
            case 3: return "R2->R3";
            case 4: return "R2->R4";
            default: break;
        }
    } else if (mapping.domain == Domain::R3) {
        switch (mapping.components) {
            case 1: return "R3->R1";
            case 2: return "R3->R2";
            case 3: return "R3->R3";
            case 4: return "R3->R4";
            default: break;
        }
    }
    return "<invalid mapping>";
}

} // namespace engine::terrain
