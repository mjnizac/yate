#include <engine/vulkan/buffer_pool.hpp>

#include <engine/assert.hpp>
#include <engine/log.hpp>
#include <engine/memory/general.hpp>

#include <utility>

namespace engine::vulkan {

namespace {

constexpr VkBufferUsageFlags kSectionUsage =
    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT
    | VK_BUFFER_USAGE_TRANSFER_DST_BIT;

constexpr VramCategory CategoryOf(terrain::Domain domain) noexcept {
    return domain == terrain::Domain::R2 ? VramCategory::SectionBuffersR2
                                        : VramCategory::SectionBuffersR3;
}

} // namespace

// --- SectionPool ------------------------------------------------------------------------------

Result<SectionPool> SectionPool::Create(Allocator& allocator, const SectionPoolConfig& config) {
    if (config.blockSize < kSectionSlotAlignment) {
        ENGINE_FAIL(ErrorCode::InvalidArgument, ErrorStage::Vulkan,
                    "section pool block size {} is below the slot alignment {}", config.blockSize,
                    kSectionSlotAlignment);
    }
    SectionPool pool;
    pool.m_allocator = &allocator;
    pool.m_config    = config;
    pool.m_blocks    = std::pmr::vector<Block>(&memory::General().Resource());
    LOG_INFO("section pool ready, growing in blocks of {} MiB", config.blockSize / (1024 * 1024));
    return pool;
}

SectionPool::~SectionPool() {
    if (m_allocator == nullptr) {
        return;
    }
    ENGINE_ASSERT(m_used == 0, "section pool destroyed with {} bytes still in use", m_used);
    for (Block& block : m_blocks) {
        DestroyBlock(block);
    }
    m_blocks.clear();
    m_allocator = nullptr;
}

SectionPool::SectionPool(SectionPool&& other) noexcept { *this = std::move(other); }

SectionPool& SectionPool::operator=(SectionPool&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    this->~SectionPool();
    m_allocator       = other.m_allocator;
    m_config          = other.m_config;
    m_blocks          = std::move(other.m_blocks);
    m_reserved        = other.m_reserved;
    m_used            = other.m_used;
    // Carried over, not reset: a slot handed out before the move still names its block by id, and
    // restarting the counter would let a new block take that id.
    m_nextBlockId     = other.m_nextBlockId;
    other.m_allocator = nullptr;
    other.m_reserved  = 0;
    other.m_used      = 0;
    return *this;
}

Result<u32_t> SectionPool::AddBlock(terrain::Domain domain, VkDeviceSize minimumSize) {
    const VkDeviceSize size = minimumSize > m_config.blockSize ? minimumSize : m_config.blockSize;

    Block          block;
    Result<Buffer> buffer = m_allocator->CreateBuffer(BufferDesc{.size     = size,
                                                                .usage    = kSectionUsage,
                                                                .category = CategoryOf(domain),
                                                                .poolBlock = true});
    if (!buffer) {
        return std::unexpected(buffer.error());
    }
    block.buffer = *buffer;
    block.domain = domain;

    const VmaVirtualBlockCreateInfo virtualInfo{.size = size};
    const VkResult                  created =
        vmaCreateVirtualBlock(&virtualInfo, &block.virtualBlock);
    if (created != VK_SUCCESS) {
        m_allocator->DestroyBuffer(block.buffer);
        return std::unexpected(MakeVulkanError(created, "vmaCreateVirtualBlock"));
    }

    m_reserved += size;
    block.id = m_nextBlockId++;
    const u32_t id = block.id;
    m_blocks.push_back(std::move(block));
    LOG_DEBUG("section pool added an {} block of {} MiB (now {} block(s))",
              domain == terrain::Domain::R2 ? "R2" : "R3", size / (1024 * 1024), m_blocks.size());
    return id;
}

void SectionPool::DestroyBlock(Block& block) noexcept {
    if (block.virtualBlock != nullptr) {
        vmaDestroyVirtualBlock(block.virtualBlock);
        block.virtualBlock = nullptr;
    }
    if (block.buffer.IsValid()) {
        m_reserved -= block.buffer.size;
        m_allocator->DestroyBuffer(block.buffer);
    }
}

Result<SectionSlot> SectionPool::Acquire(terrain::SizeClass sizeClass, VkDeviceSize size) {
    if (sizeClass.slotSize == 0 || size > sizeClass.slotSize) {
        ENGINE_FAIL(ErrorCode::InvalidArgument, ErrorStage::Vulkan,
                    "a value of {} bytes does not fit in a {}-byte size class", size,
                    sizeClass.slotSize);
    }

    const VmaVirtualAllocationCreateInfo request{.size      = sizeClass.slotSize,
                                                 .alignment = kSectionSlotAlignment};

    for (u32_t attempt = 0; attempt < 2; ++attempt) {
        for (usize_t i = 0; i < m_blocks.size(); ++i) {
            Block& block = m_blocks[i];
            if (block.domain != sizeClass.domain) {
                continue;
            }
            VmaVirtualAllocation allocation = nullptr;
            VkDeviceSize         offset     = 0;
            if (vmaVirtualAllocate(block.virtualBlock, &request, &allocation, &offset)
                != VK_SUCCESS) {
                continue;
            }
            block.used += sizeClass.slotSize;
            ++block.slotCount;
            m_used += sizeClass.slotSize;
            m_allocator->ReportSubAllocation(CategoryOf(sizeClass.domain), allocation,
                                             sizeClass.slotSize);
            return SectionSlot{.buffer     = block.buffer.handle,
                               .address    = block.buffer.address + offset,
                               .offset     = offset,
                               .size       = sizeClass.slotSize,
                               .allocation = allocation,
                               .blockId    = block.id};
        }
        if (attempt == 0) {
            Result<u32_t> added = AddBlock(sizeClass.domain, sizeClass.slotSize);
            if (!added) {
                return std::unexpected(added.error());
            }
        }
    }

    ENGINE_FAIL(ErrorCode::OutOfMemory, ErrorStage::Vulkan,
                "section pool could not place a {}-byte slot", sizeClass.slotSize);
}

void SectionPool::Release(SectionSlot& slot) noexcept {
    if (!slot.IsValid()) {
        return;
    }
    Block* owner = FindBlock(slot.blockId);
    ENGINE_ASSERT_RETURN(, owner != nullptr, "section slot names block {}, which the pool does not have",
                         slot.blockId);
    Block& block = *owner;
    m_allocator->ReleaseSubAllocation(CategoryOf(block.domain), slot.allocation, slot.size);
    vmaVirtualFree(block.virtualBlock, slot.allocation);
    block.used -= slot.size;
    --block.slotCount;
    m_used -= slot.size;
    slot = SectionSlot{};
}

SectionPool::Block* SectionPool::FindBlock(u32_t id) noexcept {
    for (Block& block : m_blocks) {
        if (block.id == id) {
            return &block;
        }
    }
    return nullptr;
}

void SectionPool::Trim() noexcept {
    // Compacts freely: a live slot names its block by id, so moving the surviving blocks down does not
    // invalidate it. Before that, a slot held an index and a trim could only be safe when nothing was
    // live at all, which is why the evaluator could not release one graph's buffers and keep another
    // graph's across a trim.
    usize_t kept = 0;
    for (usize_t i = 0; i < m_blocks.size(); ++i) {
        if (m_blocks[i].slotCount == 0) {
            DestroyBlock(m_blocks[i]);
            continue;
        }
        if (kept != i) {
            m_blocks[kept] = std::move(m_blocks[i]);
        }
        ++kept;
    }
    m_blocks.resize(kept);
}

void SectionPool::UpdatePlots() const noexcept {
    if (m_allocator == nullptr) {
        return;
    }
    // Reserved (whole blocks) and used (live slots) are tracked by the allocator per category,
    // which also derives the `unused` plot from them.
    m_allocator->UpdateCategoryPlots(VramCategory::SectionBuffersR2);
    m_allocator->UpdateCategoryPlots(VramCategory::SectionBuffersR3);
}

// --- RingBuffer -------------------------------------------------------------------------------

Result<RingBuffer> RingBuffer::Create(Allocator& allocator, VkDeviceSize capacity,
                                     VramCategory category) {
    if (category != VramCategory::Staging && category != VramCategory::Readback) {
        ENGINE_FAIL(ErrorCode::InvalidArgument, ErrorStage::Vulkan,
                    "a ring buffer must be Staging or Readback, got {}", ToString(category));
    }

    const b8_t     readback = category == VramCategory::Readback;
    RingBuffer     ring;
    ring.m_category = category;
    Result<Buffer> buffer = allocator.CreateBuffer(
        BufferDesc{.size         = capacity,
                   .usage        = static_cast<VkBufferUsageFlags>(
                       readback ? VK_BUFFER_USAGE_TRANSFER_DST_BIT
                                            : VK_BUFFER_USAGE_TRANSFER_SRC_BIT),
                   .category     = category,
                   .hostVisible  = true,
                   .randomAccess = readback});
    if (!buffer) {
        return std::unexpected(buffer.error());
    }
    if (buffer->mapped == nullptr) {
        allocator.DestroyBuffer(*buffer);
        ENGINE_FAIL(ErrorCode::InternalError, ErrorStage::Vulkan,
                    "{} ring buffer was not persistently mapped", ToString(category));
    }

    ring.m_allocator = &allocator;
    ring.m_buffer    = *buffer;
    LOG_INFO("{} ring ready, {} KiB persistently mapped", ToString(category), capacity / 1024);
    return ring;
}

RingBuffer::~RingBuffer() {
    if (m_allocator == nullptr) {
        return;
    }
    ENGINE_ASSERT(m_chunkCount == 0, "ring buffer destroyed with {} chunk(s) in flight",
                  m_chunkCount);
    m_allocator->DestroyBuffer(m_buffer);
    m_allocator = nullptr;
}

RingBuffer::RingBuffer(RingBuffer&& other) noexcept { *this = std::move(other); }

RingBuffer& RingBuffer::operator=(RingBuffer&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    this->~RingBuffer();
    m_allocator       = other.m_allocator;
    m_buffer          = other.m_buffer;
    m_category        = other.m_category;
    m_head            = other.m_head;
    m_live            = other.m_live;
    m_chunks          = other.m_chunks;
    m_firstChunk      = other.m_firstChunk;
    m_chunkCount      = other.m_chunkCount;
    other.m_allocator = nullptr;
    other.m_buffer    = Buffer{};
    other.m_chunkCount = 0;
    return *this;
}

Result<VkDeviceSize> RingBuffer::Reserve(VkDeviceSize size, VkDeviceSize alignment) {
    if (size == 0 || size > m_buffer.size) {
        ENGINE_FAIL(ErrorCode::InvalidArgument, ErrorStage::Vulkan,
                    "cannot reserve {} bytes in a {}-byte ring", size, m_buffer.size);
    }
    if (m_chunkCount == kMaxChunks) {
        ENGINE_FAIL(ErrorCode::OutOfMemory, ErrorStage::Vulkan,
                    "ring buffer already has {} chunks in flight", kMaxChunks);
    }

    const VkDeviceSize align   = alignment == 0 ? 1 : alignment;
    VkDeviceSize       offset  = (m_head + align - 1) / align * align;
    VkDeviceSize       padding = offset - m_head;
    if (offset + size > m_buffer.size) {
        // Wrap: the skipped tail is charged to this chunk so `m_live` stays exact.
        padding = m_buffer.size - m_head;
        offset  = 0;
    }

    const VkDeviceSize span = padding + size;
    if (m_live + span > m_buffer.size) {
        ENGINE_FAIL(ErrorCode::OutOfMemory, ErrorStage::Vulkan,
                    "ring buffer is full: {} of {} bytes live, {} requested", m_live,
                    m_buffer.size, span);
    }

    m_chunks[(m_firstChunk + m_chunkCount) % kMaxChunks] = Chunk{.offset = offset, .span = span};
    ++m_chunkCount;
    m_live += span;
    m_head = offset + size;
    return offset;
}

Status RingBuffer::EnsureCapacity(VkDeviceSize bytes) {
    if (m_buffer.size >= bytes) {
        return {};
    }
    if (m_live != 0 || m_chunkCount != 0) {
        ENGINE_FAIL(ErrorCode::InternalError, ErrorStage::Vulkan,
                    "the {} ring cannot grow while {} bytes are live in {} chunk(s)",
                    ToString(m_category), m_live, m_chunkCount);
    }
    ENGINE_ASSERT_RETURN(Status{}, m_allocator != nullptr, "the ring has no allocator");

    // Rounded up to a whole mebibyte, so a sequence of slightly larger sections does not reallocate on
    // every one of them.
    constexpr VkDeviceSize kGranularity = 1024ull * 1024;
    const VkDeviceSize     capacity     = AlignUp(bytes, kGranularity);
    const VkDeviceSize     previous     = m_buffer.size;

    Result<RingBuffer> grown = Create(*m_allocator, capacity, m_category);
    if (!grown) {
        return std::unexpected(grown.error());
    }
    *this = std::move(*grown);
    LOG_INFO("{} ring grown from {} KiB to {} KiB", ToString(m_category), previous / 1024,
             m_buffer.size / 1024);
    return {};
}

void RingBuffer::ReleaseOldest() noexcept {
    ENGINE_ASSERT_RETURN(, m_chunkCount > 0, "ring buffer has no chunk to release");
    m_live -= m_chunks[m_firstChunk].span;
    m_firstChunk = (m_firstChunk + 1) % kMaxChunks;
    --m_chunkCount;
}

void* RingBuffer::MappedAt(VkDeviceSize offset) const noexcept {
    ENGINE_ASSERT_RETURN(nullptr, offset < m_buffer.size, "ring offset {} is out of range",
                         offset);
    return static_cast<u8_t*>(m_buffer.mapped) + offset;
}

} // namespace engine::vulkan
