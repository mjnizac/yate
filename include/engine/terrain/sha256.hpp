#pragma once

#include <engine/common.hpp>

#ifdef IS_ENGINE

#    include <array>
#    include <cstdio>
#    include <cstring>
#    include <string_view>

namespace engine::terrain {

/// SHA-256, incremental, with no allocation.
///
/// This replaces a 64-bit FNV-1a as the identifier the sidecar records for the script. FNV is a hash
/// for hash tables: two inputs that differ in a few bytes collide far more easily than a provenance
/// record should allow, and a 64-bit value is short enough that comparing hashes across machines
/// could, in a large enough corpus, call two different scripts the same. The cost of the real thing
/// is a few microseconds for a script of this size.
///
/// It is hand-written rather than a dependency because it is sixty lines and the project's rule is
/// that a dependency has to earn its place.
class Sha256 {
public:
    static constexpr usize_t kHexSize = 65; // 64 digits and the terminator

    Sha256() noexcept { Reset(); }

    void Reset() noexcept {
        m_state     = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                       0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
        m_buffered  = 0;
        m_totalBits = 0;
    }

    void Update(const void* data, usize_t size) noexcept {
        const auto* bytes = static_cast<const u8_t*>(data);
        m_totalBits += static_cast<u64_t>(size) * 8;
        while (size > 0) {
            const usize_t take = size < 64 - m_buffered ? size : 64 - m_buffered;
            std::memcpy(m_block.data() + m_buffered, bytes, take);
            m_buffered += take;
            bytes += take;
            size -= take;
            if (m_buffered == 64) {
                Compress();
                m_buffered = 0;
            }
        }
    }

    void Update(std::string_view text) noexcept { Update(text.data(), text.size()); }

    /// Finishes the hash and returns it as 64 lowercase hex digits. The object is reset afterwards.
    [[nodiscard]] std::array<char, kHexSize> Final() noexcept {
        const u64_t bits = m_totalBits;
        const u8_t  one  = 0x80;
        Update(&one, 1);
        const u8_t zero = 0;
        while (m_buffered != 56) {
            Update(&zero, 1);
        }
        std::array<u8_t, 8> length{};
        for (usize_t i = 0; i < 8; ++i) {
            length[i] = static_cast<u8_t>(bits >> (56 - 8 * i));
        }
        Update(length.data(), length.size());

        std::array<char, kHexSize> text{};
        for (usize_t i = 0; i < 8; ++i) {
            std::snprintf(text.data() + i * 8, 9, "%08x", m_state[i]);
        }
        Reset();
        return text;
    }

private:
    static constexpr u32_t Rotr(u32_t value, u32_t count) noexcept {
        return (value >> count) | (value << (32 - count));
    }

    void Compress() noexcept {
        static constexpr std::array<u32_t, 64> kRound{
            0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u,
            0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
            0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u,
            0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
            0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
            0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
            0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
            0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
            0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au,
            0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
            0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

        std::array<u32_t, 64> w{};
        for (usize_t i = 0; i < 16; ++i) {
            w[i] = (static_cast<u32_t>(m_block[i * 4]) << 24)
                   | (static_cast<u32_t>(m_block[i * 4 + 1]) << 16)
                   | (static_cast<u32_t>(m_block[i * 4 + 2]) << 8)
                   | static_cast<u32_t>(m_block[i * 4 + 3]);
        }
        for (usize_t i = 16; i < 64; ++i) {
            const u32_t s0 = Rotr(w[i - 15], 7) ^ Rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const u32_t s1 = Rotr(w[i - 2], 17) ^ Rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i]           = w[i - 16] + s0 + w[i - 7] + s1;
        }

        std::array<u32_t, 8> v = m_state;
        for (usize_t i = 0; i < 64; ++i) {
            const u32_t s1    = Rotr(v[4], 6) ^ Rotr(v[4], 11) ^ Rotr(v[4], 25);
            const u32_t ch    = (v[4] & v[5]) ^ (~v[4] & v[6]);
            const u32_t temp1 = v[7] + s1 + ch + kRound[i] + w[i];
            const u32_t s0    = Rotr(v[0], 2) ^ Rotr(v[0], 13) ^ Rotr(v[0], 22);
            const u32_t maj   = (v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]);
            const u32_t temp2 = s0 + maj;
            v[7] = v[6];
            v[6] = v[5];
            v[5] = v[4];
            v[4] = v[3] + temp1;
            v[3] = v[2];
            v[2] = v[1];
            v[1] = v[0];
            v[0] = temp1 + temp2;
        }
        for (usize_t i = 0; i < 8; ++i) {
            m_state[i] += v[i];
        }
    }

    std::array<u32_t, 8> m_state{};
    std::array<u8_t, 64> m_block{};
    usize_t              m_buffered  = 0;
    u64_t                m_totalBits = 0;
};

} // namespace engine::terrain

#endif // IS_ENGINE
