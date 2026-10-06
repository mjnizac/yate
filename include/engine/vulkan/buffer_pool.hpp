#pragma once

#include <engine/common.hpp>

#ifdef IS_ENGINE

#    include <engine/error.hpp>
#    include <engine/terrain/mapping.hpp>
#    include <engine/vulkan/allocator.hpp>

#    include <array>
#    include <memory_resource>
#    include <vector>

namespace engine::vulkan {

inline constexpr u32_t kInvalidBlock = ~0u;

/// Alignment every section slot is given, so a slot can be bound as a storage buffer and read
/// with scalar block layout without straddling a cache line boundary.
inline constexpr VkDeviceSize kSectionSlotAlignment = 256;

/// One sub-allocation of the section pool, handed to a node's output value.
struct SectionSlot {
    VkBuffer             buffer     = VK_NULL_HANDLE;
    VkDeviceAddress      address    = 0;
    VkDeviceSize         offset     = 0;
    /// Slot size, which is the size class and is >= the requested value size.
    VkDeviceSize         size       = 0;
    VmaVirtualAllocation allocation = nullptr;
    u32_t                blockIndex = kInvalidBlock;

    [[nodiscard]] b8_t IsValid() const noexcept { return buffer != VK_NULL_HANDLE; }
};

struct SectionPoolConfig {
    /// Bytes added per block. The pool grows by whole blocks and only shrinks on `Trim`.
    VkDeviceSize blockSize = 256ull * 1024 * 1024;
};

/// Section buffer pool: one large `VkBuffer` per domain and block, sub-allocated with a VMA
/// virtual block (spec section 7.3).
///
/// Slots are grouped in size classes derived from the mapping sizes. `R2` and `R3` values never
/// share a block, so a 512x512 heightmap and a 64x64x64 brick cannot land in each other's slots,
/// and a value is never placed in a slot smaller than its computed size.
class SectionPool {
public:
    SectionPool() = default;
    ~SectionPool();

    SectionPool(SectionPool&& other) noexcept;
    SectionPool& operator=(SectionPool&& other) noexcept;
    ENGINE_NO_COPY(SectionPool);

    [[nodiscard]] static Result<SectionPool> Create(Allocator&               allocator,
                                                    const SectionPoolConfig& config);

    /// Reserves a slot of `sizeClass.slotSize` bytes. `size` must fit in the class.
    [[nodiscard]] Result<SectionSlot> Acquire(terrain::SizeClass sizeClass, VkDeviceSize size);

    void Release(SectionSlot& slot) noexcept;

    /// Releases every block that currently holds no slot.
    void Trim() noexcept;

    [[nodiscard]] VkDeviceSize ReservedBytes() const noexcept { return m_reserved; }
    [[nodiscard]] VkDeviceSize UsedBytes() const noexcept { return m_used; }
    [[nodiscard]] usize_t      BlockCount() const noexcept { return m_blocks.size(); }

    /// Refreshes the reserved/used/unused plots of both section-buffer pools.
    void UpdatePlots() const noexcept;

    /// Re-points the pool at `allocator`. The owning context is movable, so the address of the
    /// allocator it holds changes with it; the pool keeps a pointer and must be told.
    void Rebind(Allocator& allocator) noexcept { m_allocator = &allocator; }

private:
    struct Block {
        Buffer          buffer;
        VmaVirtualBlock virtualBlock = nullptr;
        terrain::Domain domain       = terrain::Domain::R2;
        VkDeviceSize    used         = 0;
        usize_t         slotCount    = 0;
    };

    [[nodiscard]] Result<u32_t> AddBlock(terrain::Domain domain, VkDeviceSize minimumSize);
    void                        DestroyBlock(Block& block) noexcept;

    Allocator*                  m_allocator = nullptr;
    SectionPoolConfig           m_config;
    std::pmr::vector<Block>     m_blocks;
    VkDeviceSize                m_reserved = 0;
    VkDeviceSize                m_used     = 0;
};

/// Persistently mapped ring buffer for staging uploads or readback.
///
/// Chunks are reserved and released in FIFO order, which matches the section pipeline: while
/// section k is computed, section k-1 is read back and section k-2 is written to disk
/// (spec section 10). A ring is sized for at least two sections in flight.
class RingBuffer {
public:
    RingBuffer() = default;
    ~RingBuffer();

    RingBuffer(RingBuffer&& other) noexcept;
    RingBuffer& operator=(RingBuffer&& other) noexcept;
    ENGINE_NO_COPY(RingBuffer);

    /// `category` must be `Staging` or `Readback`; it also selects the host access pattern.
    [[nodiscard]] static Result<RingBuffer> Create(Allocator& allocator, VkDeviceSize capacity,
                                                   VramCategory category);

    /// Reserves `size` bytes and returns their offset, or an error when the ring is full.
    [[nodiscard]] Result<VkDeviceSize> Reserve(VkDeviceSize size, VkDeviceSize alignment);

    /// Releases the oldest outstanding chunk. Call once the GPU has finished with it.
    void ReleaseOldest() noexcept;

    [[nodiscard]] const Buffer& GetBuffer() const noexcept { return m_buffer; }
    [[nodiscard]] VkDeviceSize  Capacity() const noexcept { return m_buffer.size; }
    [[nodiscard]] VkDeviceSize  LiveBytes() const noexcept { return m_live; }
    [[nodiscard]] usize_t       ChunkCount() const noexcept { return m_chunkCount; }

    /// CPU pointer to `offset` inside the persistent mapping.
    [[nodiscard]] void* MappedAt(VkDeviceSize offset) const noexcept;

    /// Re-points the ring at `allocator`, for the same reason as `SectionPool::Rebind`.
    void Rebind(Allocator& allocator) noexcept { m_allocator = &allocator; }

private:
    /// Outstanding chunks in flight. Two sections in flight need far fewer than this.
    static constexpr usize_t kMaxChunks = 32;

    struct Chunk {
        VkDeviceSize offset = 0;
        /// Bytes charged to this chunk, including the alignment padding and any skipped tail.
        VkDeviceSize span = 0;
    };

    Allocator*                     m_allocator = nullptr;
    Buffer                         m_buffer;
    VkDeviceSize                   m_head = 0;
    VkDeviceSize                   m_live = 0;
    std::array<Chunk, kMaxChunks>  m_chunks{};
    usize_t                        m_firstChunk = 0;
    usize_t                        m_chunkCount = 0;
};

} // namespace engine::vulkan

#endif // IS_ENGINE
