//
// Copyright 2018 The ANGLE Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// vk_helpers:
//   Helper utilitiy classes that manage Vulkan resources.

#ifndef LIBANGLE_RENDERER_VULKAN_VK_HELPERS_H_
#define LIBANGLE_RENDERER_VULKAN_VK_HELPERS_H_

#include "common/MemoryBuffer.h"
#include "libANGLE/renderer/vulkan/ResourceVk.h"
#include "libANGLE/renderer/vulkan/vk_cache_utils.h"
#include "libANGLE/renderer/vulkan/vk_format_utils.h"

namespace gl
{
class ImageIndex;
}  // namespace gl

namespace rx
{
namespace vk
{
constexpr VkBufferUsageFlags kVertexBufferUsageFlags =
    VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
constexpr VkBufferUsageFlags kIndexBufferUsageFlags =
    VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
constexpr VkBufferUsageFlags kIndirectBufferUsageFlags =
    VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
constexpr size_t kVertexBufferAlignment   = 4;
constexpr size_t kIndexBufferAlignment    = 4;
constexpr size_t kIndirectBufferAlignment = 4;

constexpr VkBufferUsageFlags kStagingBufferFlags =
    VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
constexpr size_t kStagingBufferSize = 1024 * 16;

constexpr VkImageCreateFlags kVkImageCreateFlagsNone = 0;

using StagingBufferOffsetArray = std::array<VkDeviceSize, 2>;

struct TextureUnit final
{
    TextureVk *texture;
    const SamplerHelper *sampler;
    GLenum srgbDecode;
};

// A dynamic buffer is conceptually an infinitely long buffer. Each time you write to the buffer,
// you will always write to a previously unused portion. After a series of writes, you must flush
// the buffer data to the device. Buffer lifetime currently assumes that each new allocation will
// last as long or longer than each prior allocation.
//
// Dynamic buffers are used to implement a variety of data streaming operations in Vulkan, such
// as for immediate vertex array and element array data, uniform updates, and other dynamic data.
//
// Internally dynamic buffers keep a collection of VkBuffers. When we write past the end of a
// currently active VkBuffer we keep it until it is no longer in use. We then mark it available
// for future allocations in a free list.
class BufferHelper;
using BufferHelperPointerVector = std::vector<std::unique_ptr<BufferHelper>>;

enum class DynamicBufferPolicy
{
    // Used where future allocations from the dynamic buffer are unlikely, so it's best to free the
    // memory when the allocated buffers are no longer in use.
    OneShotUse,
    // Used where multiple small allocations are made every frame, so it's worth keeping the free
    // buffers around to avoid release/reallocation.
    FrequentSmallAllocations,
    // Used where bursts of allocation happen occasionally, but the steady state may make
    // allocations every now and then.  In that case, a limited number of buffers are retained.
    SporadicTextureUpload,
};

class DynamicBuffer : angle::NonCopyable
{
  public:
    DynamicBuffer();
    DynamicBuffer(DynamicBuffer &&other);
    ~DynamicBuffer();

    // Init is called after the buffer creation so that the alignment can be specified later.
    void init(RendererVk *renderer,
              VkBufferUsageFlags usage,
              size_t alignment,
              size_t initialSize,
              bool hostVisible,
              DynamicBufferPolicy policy);

    // Init that gives the ability to pass in specified memory property flags for the buffer.
    void initWithFlags(RendererVk *renderer,
                       VkBufferUsageFlags usage,
                       size_t alignment,
                       size_t initialSize,
                       VkMemoryPropertyFlags memoryProperty,
                       DynamicBufferPolicy policy);

    // This call will allocate a new region at the end of the current buffer. If it can't find
    // enough space in the current buffer, it returns false. This gives caller a chance to deal with
    // buffer switch that may occur with allocate call.
    bool allocateFromCurrentBuffer(size_t sizeInBytes, uint8_t **ptrOut, VkDeviceSize *offsetOut);

    // This call will allocate a new region at the end of the buffer. It internally may trigger
    // a new buffer to be created (which is returned in the optional parameter
    // `newBufferAllocatedOut`).  The new region will be in the returned buffer at given offset. If
    // a memory pointer is given, the buffer will be automatically map()ed.
    angle::Result allocateWithAlignment(ContextVk *contextVk,
                                        size_t sizeInBytes,
                                        size_t alignment,
                                        uint8_t **ptrOut,
                                        VkBuffer *bufferOut,
                                        VkDeviceSize *offsetOut,
                                        bool *newBufferAllocatedOut);

    // Allocate with default alignment
    angle::Result allocate(ContextVk *contextVk,
                           size_t sizeInBytes,
                           uint8_t **ptrOut,
                           VkBuffer *bufferOut,
                           VkDeviceSize *offsetOut,
                           bool *newBufferAllocatedOut)
    {
        return allocateWithAlignment(contextVk, sizeInBytes, mAlignment, ptrOut, bufferOut,
                                     offsetOut, newBufferAllocatedOut);
    }

    // After a sequence of writes, call flush to ensure the data is visible to the device.
    angle::Result flush(ContextVk *contextVk);

    // After a sequence of writes, call invalidate to ensure the data is visible to the host.
    angle::Result invalidate(ContextVk *contextVk);

    // This releases resources when they might currently be in use.
    void release(RendererVk *renderer);

    // This releases all the buffers that have been allocated since this was last called.
    void releaseInFlightBuffers(ContextVk *contextVk);

    // This adds inflight buffers to the context's mResourceUseList and then releases them
    void releaseInFlightBuffersToResourceUseList(ContextVk *contextVk);

    // This frees resources immediately.
    void destroy(RendererVk *renderer);

    BufferHelper *getCurrentBuffer() const { return mBuffer.get(); }

    // **Accumulate** an alignment requirement.  A dynamic buffer is used as the staging buffer for
    // image uploads, which can contain updates to unrelated mips, possibly with different formats.
    // The staging buffer should have an alignment that can satisfy all those formats, i.e. it's the
    // lcm of all alignments set in its lifetime.
    void requireAlignment(RendererVk *renderer, size_t alignment);
    size_t getAlignment() const { return mAlignment; }

    // For testing only!
    void setMinimumSizeForTesting(size_t minSize);

    bool isCoherent() const
    {
        return (mMemoryPropertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
    }

    bool valid() const { return mSize != 0; }

  private:
    void reset();
    angle::Result allocateNewBuffer(ContextVk *contextVk);

    VkBufferUsageFlags mUsage;
    bool mHostVisible;
    DynamicBufferPolicy mPolicy;
    size_t mInitialSize;
    std::unique_ptr<BufferHelper> mBuffer;
    uint32_t mNextAllocationOffset;
    uint32_t mLastFlushOrInvalidateOffset;
    size_t mSize;
    size_t mAlignment;
    VkMemoryPropertyFlags mMemoryPropertyFlags;

    BufferHelperPointerVector mInFlightBuffers;
    BufferHelperPointerVector mBufferFreeList;
};

// Based off of the DynamicBuffer class, DynamicShadowBuffer provides
// a similar conceptually infinitely long buffer that will only be written
// to and read by the CPU. This can be used to provide CPU cached copies of
// GPU-read only buffers. The value add here is that when an app requests
// CPU access to a buffer we can fullfil such a request in O(1) time since
// we don't need to wait for GPU to be done with in-flight commands.
//
// The hidden cost here is that any operation that updates a buffer, either
// through a buffer sub data update or a buffer-to-buffer copy will have an
// additional overhead of having to update its CPU only buffer
class DynamicShadowBuffer : public angle::NonCopyable
{
  public:
    DynamicShadowBuffer();
    DynamicShadowBuffer(DynamicShadowBuffer &&other);
    ~DynamicShadowBuffer();

    // Initialize the DynamicShadowBuffer.
    void init(size_t initialSize);

    // Returns whether this DynamicShadowBuffer is active
    ANGLE_INLINE bool valid() { return (mSize != 0); }

    // This call will actually allocate a new CPU only memory from the heap.
    // The size can be different than the one specified during `init`.
    angle::Result allocate(size_t sizeInBytes);

    ANGLE_INLINE void updateData(const uint8_t *data, size_t size, size_t offset)
    {
        ASSERT(!mBuffer.empty());
        // Memcopy data into the buffer
        memcpy((mBuffer.data() + offset), data, size);
    }

    // Map the CPU only buffer and return the pointer. We map the entire buffer for now.
    ANGLE_INLINE void map(size_t offset, void **mapPtr)
    {
        ASSERT(mapPtr);
        ASSERT(!mBuffer.empty());
        *mapPtr = mBuffer.data() + offset;
    }

    // Unmap the CPU only buffer, NOOP for now
    ANGLE_INLINE void unmap() {}

    // This releases resources when they might currently be in use.
    void release();

    // This frees resources immediately.
    void destroy(VkDevice device);

    ANGLE_INLINE uint8_t *getCurrentBuffer()
    {
        ASSERT(!mBuffer.empty());
        return mBuffer.data();
    }

    ANGLE_INLINE const uint8_t *getCurrentBuffer() const
    {
        ASSERT(!mBuffer.empty());
        return mBuffer.data();
    }

  private:
    void reset();

    size_t mInitialSize;
    size_t mSize;
    angle::MemoryBuffer mBuffer;
};

// Uses DescriptorPool to allocate descriptor sets as needed. If a descriptor pool becomes full, we
// allocate new pools internally as needed. RendererVk takes care of the lifetime of the discarded
// pools. Note that we used a fixed layout for descriptor pools in ANGLE.

// Shared handle to a descriptor pool. Each helper is allocated from the dynamic descriptor pool.
// Can be used to share descriptor pools between multiple ProgramVks and the ContextVk.
class DescriptorPoolHelper : public Resource
{
  public:
    DescriptorPoolHelper();
    ~DescriptorPoolHelper() override;

    bool valid() { return mDescriptorPool.valid(); }

    bool hasCapacity(uint32_t descriptorSetCount) const;
    angle::Result init(ContextVk *contextVk,
                       const std::vector<VkDescriptorPoolSize> &poolSizesIn,
                       uint32_t maxSets);
    void destroy(VkDevice device);
    void release(ContextVk *contextVk);

    angle::Result allocateSets(ContextVk *contextVk,
                               const VkDescriptorSetLayout *descriptorSetLayout,
                               uint32_t descriptorSetCount,
                               VkDescriptorSet *descriptorSetsOut);

  private:
    uint32_t mFreeDescriptorSets;
    DescriptorPool mDescriptorPool;
};

using RefCountedDescriptorPoolHelper  = RefCounted<DescriptorPoolHelper>;
using RefCountedDescriptorPoolBinding = BindingPointer<DescriptorPoolHelper>;

class DynamicDescriptorPool final : angle::NonCopyable
{
  public:
    DynamicDescriptorPool();
    ~DynamicDescriptorPool();

    // The DynamicDescriptorPool only handles one pool size at this time.
    // Note that setSizes[i].descriptorCount is expected to be the number of descriptors in
    // an individual set.  The pool size will be calculated accordingly.
    angle::Result init(ContextVk *contextVk,
                       const VkDescriptorPoolSize *setSizes,
                       size_t setSizeCount,
                       VkDescriptorSetLayout descriptorSetLayout);
    void destroy(VkDevice device);
    void release(ContextVk *contextVk);

    // We use the descriptor type to help count the number of free sets.
    // By convention, sets are indexed according to the constants in vk_cache_utils.h.
    ANGLE_INLINE angle::Result allocateSets(ContextVk *contextVk,
                                            const VkDescriptorSetLayout *descriptorSetLayout,
                                            uint32_t descriptorSetCount,
                                            RefCountedDescriptorPoolBinding *bindingOut,
                                            VkDescriptorSet *descriptorSetsOut)
    {
        bool ignoreNewPoolAllocated;
        return allocateSetsAndGetInfo(contextVk, descriptorSetLayout, descriptorSetCount,
                                      bindingOut, descriptorSetsOut, &ignoreNewPoolAllocated);
    }

    // We use the descriptor type to help count the number of free sets.
    // By convention, sets are indexed according to the constants in vk_cache_utils.h.
    angle::Result allocateSetsAndGetInfo(ContextVk *contextVk,
                                         const VkDescriptorSetLayout *descriptorSetLayout,
                                         uint32_t descriptorSetCount,
                                         RefCountedDescriptorPoolBinding *bindingOut,
                                         VkDescriptorSet *descriptorSetsOut,
                                         bool *newPoolAllocatedOut);

    // For testing only!
    static uint32_t GetMaxSetsPerPoolForTesting();
    static void SetMaxSetsPerPoolForTesting(uint32_t maxSetsPerPool);
    static uint32_t GetMaxSetsPerPoolMultiplierForTesting();
    static void SetMaxSetsPerPoolMultiplierForTesting(uint32_t maxSetsPerPool);

  private:
    angle::Result allocateNewPool(ContextVk *contextVk);

    static constexpr uint32_t kMaxSetsPerPoolMax = 512;
    static uint32_t mMaxSetsPerPool;
    static uint32_t mMaxSetsPerPoolMultiplier;
    size_t mCurrentPoolIndex;
    std::vector<RefCountedDescriptorPoolHelper *> mDescriptorPools;
    std::vector<VkDescriptorPoolSize> mPoolSizes;
    // This cached handle is used for verifying the layout being used to allocate descriptor sets
    // from the pool matches the layout that the pool was created for, to ensure that the free
    // descriptor count is accurate and new pools are created appropriately.
    VkDescriptorSetLayout mCachedDescriptorSetLayout;
};

template <typename Pool>
class DynamicallyGrowingPool : angle::NonCopyable
{
  public:
    DynamicallyGrowingPool();
    virtual ~DynamicallyGrowingPool();

    bool isValid() { return mPoolSize > 0; }

  protected:
    angle::Result initEntryPool(Context *contextVk, uint32_t poolSize);
    void destroyEntryPool();

    // Checks to see if any pool is already free, in which case it sets it as current pool and
    // returns true.
    bool findFreeEntryPool(ContextVk *contextVk);

    // Allocates a new entry and initializes it with the given pool.
    angle::Result allocateNewEntryPool(ContextVk *contextVk, Pool &&pool);

    // Called by the implementation whenever an entry is freed.
    void onEntryFreed(ContextVk *contextVk, size_t poolIndex);

    // The pool size, to know when a pool is completely freed.
    uint32_t mPoolSize;

    std::vector<Pool> mPools;

    struct PoolStats
    {
        // A count corresponding to each pool indicating how many of its allocated entries
        // have been freed. Once that value reaches mPoolSize for each pool, that pool is considered
        // free and reusable.  While keeping a bitset would allow allocation of each index, the
        // slight runtime overhead of finding free indices is not worth the slight memory overhead
        // of creating new pools when unnecessary.
        uint32_t freedCount;
        // The serial of the renderer is stored on each object free to make sure no
        // new allocations are made from the pool until it's not in use.
        Serial serial;
    };
    std::vector<PoolStats> mPoolStats;

    // Index into mPools indicating pool we are currently allocating from.
    size_t mCurrentPool;
    // Index inside mPools[mCurrentPool] indicating which index can be allocated next.
    uint32_t mCurrentFreeEntry;
};

// DynamicQueryPool allocates indices out of QueryPool as needed.  Once a QueryPool is exhausted,
// another is created.  The query pools live permanently, but are recycled as indices get freed.

// These are arbitrary default sizes for query pools.
constexpr uint32_t kDefaultOcclusionQueryPoolSize           = 64;
constexpr uint32_t kDefaultTimestampQueryPoolSize           = 64;
constexpr uint32_t kDefaultTransformFeedbackQueryPoolSize   = 128;
constexpr uint32_t kDefaultPrimitivesGeneratedQueryPoolSize = 128;

class QueryHelper;

class DynamicQueryPool final : public DynamicallyGrowingPool<QueryPool>
{
  public:
    DynamicQueryPool();
    ~DynamicQueryPool() override;

    angle::Result init(ContextVk *contextVk, VkQueryType type, uint32_t poolSize);
    void destroy(VkDevice device);

    angle::Result allocateQuery(ContextVk *contextVk, QueryHelper *queryOut, uint32_t queryCount);
    void freeQuery(ContextVk *contextVk, QueryHelper *query);

    const QueryPool &getQueryPool(size_t index) const { return mPools[index]; }

  private:
    angle::Result allocateNewPool(ContextVk *contextVk);

    // Information required to create new query pools
    VkQueryType mQueryType;
};

// Stores the result of a Vulkan query call. XFB queries in particular store two result values.
class QueryResult final
{
  public:
    QueryResult(uint32_t intsPerResult) : mIntsPerResult(intsPerResult), mResults{} {}

    void operator+=(const QueryResult &rhs)
    {
        mResults[0] += rhs.mResults[0];
        mResults[1] += rhs.mResults[1];
    }

    size_t getDataSize() const { return mIntsPerResult * sizeof(uint64_t); }
    void setResults(uint64_t *results, uint32_t queryCount);
    uint64_t getResult(size_t index) const
    {
        ASSERT(index < mIntsPerResult);
        return mResults[index];
    }

    static constexpr size_t kDefaultResultIndex                      = 0;
    static constexpr size_t kTransformFeedbackPrimitivesWrittenIndex = 0;
    static constexpr size_t kPrimitivesGeneratedIndex                = 1;

  private:
    uint32_t mIntsPerResult;
    std::array<uint64_t, 2> mResults;
};

// Queries in Vulkan are identified by the query pool and an index for a query within that pool.
// Unlike other pools, such as descriptor pools where an allocation returns an independent object
// from the pool, the query allocations are not done through a Vulkan function and are only an
// integer index.
//
// Furthermore, to support arbitrarily large number of queries, DynamicQueryPool creates query pools
// of a fixed size as needed and allocates indices within those pools.
//
// The QueryHelper class below keeps the pool and index pair together.  For multiview, multiple
// consecutive query indices are implicitly written to by the driver, so the query count is
// additionally kept.
class QueryHelper final : public Resource
{
  public:
    QueryHelper();
    ~QueryHelper() override;
    QueryHelper(QueryHelper &&rhs);
    QueryHelper &operator=(QueryHelper &&rhs);
    void init(const DynamicQueryPool *dynamicQueryPool,
              const size_t queryPoolIndex,
              uint32_t query,
              uint32_t queryCount);
    void deinit();

    bool valid() const { return mDynamicQueryPool != nullptr; }

    // Begin/end queries.  These functions break the render pass.
    angle::Result beginQuery(ContextVk *contextVk);
    angle::Result endQuery(ContextVk *contextVk);
    // Begin/end queries within a started render pass.
    angle::Result beginRenderPassQuery(ContextVk *contextVk);
    void endRenderPassQuery(ContextVk *contextVk);

    angle::Result flushAndWriteTimestamp(ContextVk *contextVk);
    // When syncing gpu/cpu time, main thread accesses primary directly
    void writeTimestampToPrimary(ContextVk *contextVk, PrimaryCommandBuffer *primary);
    // All other timestamp accesses should be made on outsideRenderPassCommandBuffer
    void writeTimestamp(ContextVk *contextVk, CommandBuffer *outsideRenderPassCommandBuffer);

    // Whether this query helper has generated and submitted any commands.
    bool hasSubmittedCommands() const;

    angle::Result getUint64ResultNonBlocking(ContextVk *contextVk,
                                             QueryResult *resultOut,
                                             bool *availableOut);
    angle::Result getUint64Result(ContextVk *contextVk, QueryResult *resultOut);

  private:
    friend class DynamicQueryPool;
    const QueryPool &getQueryPool() const
    {
        ASSERT(valid());
        return mDynamicQueryPool->getQueryPool(mQueryPoolIndex);
    }

    // Reset needs to always be done outside a render pass, which may be different from the
    // passed-in command buffer (which could be the render pass').
    void beginQueryImpl(ContextVk *contextVk,
                        CommandBuffer *resetCommandBuffer,
                        CommandBuffer *commandBuffer);
    void endQueryImpl(ContextVk *contextVk, CommandBuffer *commandBuffer);
    VkResult getResultImpl(ContextVk *contextVk,
                           const VkQueryResultFlags flags,
                           QueryResult *resultOut);

    const DynamicQueryPool *mDynamicQueryPool;
    size_t mQueryPoolIndex;
    uint32_t mQuery;
    uint32_t mQueryCount;
};

// DynamicSemaphorePool allocates semaphores as needed.  It uses a std::vector
// as a pool to allocate many semaphores at once.  The pools live permanently,
// but are recycled as semaphores get freed.

// These are arbitrary default sizes for semaphore pools.
constexpr uint32_t kDefaultSemaphorePoolSize = 64;

class SemaphoreHelper;

class DynamicSemaphorePool final : public DynamicallyGrowingPool<std::vector<Semaphore>>
{
  public:
    DynamicSemaphorePool();
    ~DynamicSemaphorePool() override;

    angle::Result init(ContextVk *contextVk, uint32_t poolSize);
    void destroy(VkDevice device);

    bool isValid() { return mPoolSize > 0; }

    // autoFree can be used to allocate a semaphore that's expected to be freed at the end of the
    // frame.  This renders freeSemaphore unnecessary and saves an eventual search.
    angle::Result allocateSemaphore(ContextVk *contextVk, SemaphoreHelper *semaphoreOut);
    void freeSemaphore(ContextVk *contextVk, SemaphoreHelper *semaphore);

  private:
    angle::Result allocateNewPool(ContextVk *contextVk);
};

// Semaphores that are allocated from the semaphore pool are encapsulated in a helper object,
// keeping track of where in the pool they are allocated from.
class SemaphoreHelper final : angle::NonCopyable
{
  public:
    SemaphoreHelper();
    ~SemaphoreHelper();

    SemaphoreHelper(SemaphoreHelper &&other);
    SemaphoreHelper &operator=(SemaphoreHelper &&other);

    void init(const size_t semaphorePoolIndex, const Semaphore *semaphore);
    void deinit();

    const Semaphore *getSemaphore() const { return mSemaphore; }

    // Used only by DynamicSemaphorePool.
    size_t getSemaphorePoolIndex() const { return mSemaphorePoolIndex; }

  private:
    size_t mSemaphorePoolIndex;
    const Semaphore *mSemaphore;
};

// This class' responsibility is to create index buffers needed to support line loops in Vulkan.
// In the setup phase of drawing, the createIndexBuffer method should be called with the
// current draw call parameters. If an element array buffer is bound for an indexed draw, use
// createIndexBufferFromElementArrayBuffer.
//
// If the user wants to draw a loop between [v1, v2, v3], we will create an indexed buffer with
// these indexes: [0, 1, 2, 3, 0] to emulate the loop.
class LineLoopHelper final : angle::NonCopyable
{
  public:
    LineLoopHelper(RendererVk *renderer);
    ~LineLoopHelper();

    angle::Result getIndexBufferForDrawArrays(ContextVk *contextVk,
                                              uint32_t clampedVertexCount,
                                              GLint firstVertex,
                                              BufferHelper **bufferOut,
                                              VkDeviceSize *offsetOut);

    angle::Result getIndexBufferForElementArrayBuffer(ContextVk *contextVk,
                                                      BufferVk *elementArrayBufferVk,
                                                      gl::DrawElementsType glIndexType,
                                                      int indexCount,
                                                      intptr_t elementArrayOffset,
                                                      BufferHelper **bufferOut,
                                                      VkDeviceSize *bufferOffsetOut,
                                                      uint32_t *indexCountOut);

    angle::Result streamIndices(ContextVk *contextVk,
                                gl::DrawElementsType glIndexType,
                                GLsizei indexCount,
                                const uint8_t *srcPtr,
                                BufferHelper **bufferOut,
                                VkDeviceSize *bufferOffsetOut,
                                uint32_t *indexCountOut);

    angle::Result streamIndicesIndirect(ContextVk *contextVk,
                                        gl::DrawElementsType glIndexType,
                                        BufferHelper *indexBuffer,
                                        VkDeviceSize indexBufferOffset,
                                        BufferHelper *indirectBuffer,
                                        VkDeviceSize indirectBufferOffset,
                                        BufferHelper **indexBufferOut,
                                        VkDeviceSize *indexBufferOffsetOut,
                                        BufferHelper **indirectBufferOut,
                                        VkDeviceSize *indirectBufferOffsetOut);

    angle::Result streamArrayIndirect(ContextVk *contextVk,
                                      size_t vertexCount,
                                      BufferHelper *arrayIndirectBuffer,
                                      VkDeviceSize arrayIndirectBufferOffset,
                                      BufferHelper **indexBufferOut,
                                      VkDeviceSize *indexBufferOffsetOut,
                                      BufferHelper **indexIndirectBufferOut,
                                      VkDeviceSize *indexIndirectBufferOffsetOut);

    void release(ContextVk *contextVk);
    void destroy(RendererVk *renderer);

    static void Draw(uint32_t count, uint32_t baseVertex, CommandBuffer *commandBuffer);

  private:
    DynamicBuffer mDynamicIndexBuffer;
    DynamicBuffer mDynamicIndirectBuffer;
};

// This defines enum for VkPipelineStageFlagBits so that we can use it to compare and index into
// array.
enum class PipelineStage : uint16_t
{
    // Bellow are ordered based on Graphics Pipeline Stages
    TopOfPipe             = 0,
    DrawIndirect          = 1,
    VertexInput           = 2,
    VertexShader          = 3,
    GeometryShader        = 4,
    TransformFeedback     = 5,
    EarlyFragmentTest     = 6,
    FragmentShader        = 7,
    LateFragmentTest      = 8,
    ColorAttachmentOutput = 9,

    // Compute specific pipeline Stage
    ComputeShader = 10,

    // Transfer specific pipeline Stage
    Transfer     = 11,
    BottomOfPipe = 12,

    // Host specific pipeline stage
    Host = 13,

    InvalidEnum = 14,
    EnumCount   = InvalidEnum,
};
using PipelineStagesMask = angle::PackedEnumBitSet<PipelineStage, uint16_t>;

PipelineStage GetPipelineStage(gl::ShaderType stage);

// This wraps data and API for vkCmdPipelineBarrier call
class PipelineBarrier : angle::NonCopyable
{
  public:
    PipelineBarrier()
        : mSrcStageMask(0),
          mDstStageMask(0),
          mMemoryBarrierSrcAccess(0),
          mMemoryBarrierDstAccess(0),
          mImageMemoryBarriers()
    {}
    ~PipelineBarrier() = default;

    bool isEmpty() const { return mImageMemoryBarriers.empty() && mMemoryBarrierDstAccess == 0; }

    void execute(PrimaryCommandBuffer *primary)
    {
        if (isEmpty())
        {
            return;
        }

        // Issue vkCmdPipelineBarrier call
        VkMemoryBarrier memoryBarrier = {};
        uint32_t memoryBarrierCount   = 0;
        if (mMemoryBarrierDstAccess != 0)
        {
            memoryBarrier.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            memoryBarrier.srcAccessMask = mMemoryBarrierSrcAccess;
            memoryBarrier.dstAccessMask = mMemoryBarrierDstAccess;
            memoryBarrierCount++;
        }
        primary->pipelineBarrier(
            mSrcStageMask, mDstStageMask, 0, memoryBarrierCount, &memoryBarrier, 0, nullptr,
            static_cast<uint32_t>(mImageMemoryBarriers.size()), mImageMemoryBarriers.data());

        reset();
    }

    void executeIndividually(PrimaryCommandBuffer *primary)
    {
        if (isEmpty())
        {
            return;
        }

        // Issue vkCmdPipelineBarrier call
        VkMemoryBarrier memoryBarrier = {};
        uint32_t memoryBarrierCount   = 0;
        if (mMemoryBarrierDstAccess != 0)
        {
            memoryBarrier.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            memoryBarrier.srcAccessMask = mMemoryBarrierSrcAccess;
            memoryBarrier.dstAccessMask = mMemoryBarrierDstAccess;
            memoryBarrierCount++;
        }

        for (const VkImageMemoryBarrier &imageBarrier : mImageMemoryBarriers)
        {
            primary->pipelineBarrier(mSrcStageMask, mDstStageMask, 0, memoryBarrierCount,
                                     &memoryBarrier, 0, nullptr, 1, &imageBarrier);
        }

        reset();
    }

    // merge two barriers into one
    void merge(PipelineBarrier *other)
    {
        mSrcStageMask |= other->mSrcStageMask;
        mDstStageMask |= other->mDstStageMask;
        mMemoryBarrierSrcAccess |= other->mMemoryBarrierSrcAccess;
        mMemoryBarrierDstAccess |= other->mMemoryBarrierDstAccess;
        mImageMemoryBarriers.insert(mImageMemoryBarriers.end(), other->mImageMemoryBarriers.begin(),
                                    other->mImageMemoryBarriers.end());
        other->reset();
    }

    void mergeMemoryBarrier(VkPipelineStageFlags srcStageMask,
                            VkPipelineStageFlags dstStageMask,
                            VkFlags srcAccess,
                            VkFlags dstAccess)
    {
        mSrcStageMask |= srcStageMask;
        mDstStageMask |= dstStageMask;
        mMemoryBarrierSrcAccess |= srcAccess;
        mMemoryBarrierDstAccess |= dstAccess;
    }

    void mergeImageBarrier(VkPipelineStageFlags srcStageMask,
                           VkPipelineStageFlags dstStageMask,
                           const VkImageMemoryBarrier &imageMemoryBarrier)
    {
        ASSERT(imageMemoryBarrier.pNext == nullptr);
        mSrcStageMask |= srcStageMask;
        mDstStageMask |= dstStageMask;
        mImageMemoryBarriers.push_back(imageMemoryBarrier);
    }

    void reset()
    {
        mSrcStageMask           = 0;
        mDstStageMask           = 0;
        mMemoryBarrierSrcAccess = 0;
        mMemoryBarrierDstAccess = 0;
        mImageMemoryBarriers.clear();
    }

    void addDiagnosticsString(std::ostringstream &out) const;

  private:
    VkPipelineStageFlags mSrcStageMask;
    VkPipelineStageFlags mDstStageMask;
    VkFlags mMemoryBarrierSrcAccess;
    VkFlags mMemoryBarrierDstAccess;
    std::vector<VkImageMemoryBarrier> mImageMemoryBarriers;
};
using PipelineBarrierArray = angle::PackedEnumMap<PipelineStage, PipelineBarrier>;

class FramebufferHelper;

class BufferMemory : angle::NonCopyable
{
  public:
    BufferMemory();
    ~BufferMemory();
    angle::Result initExternal(GLeglClientBufferEXT clientBuffer);
    angle::Result init();

    void destroy(RendererVk *renderer);

    angle::Result map(ContextVk *contextVk, VkDeviceSize size, uint8_t **ptrOut)
    {
        if (mMappedMemory == nullptr)
        {
            ANGLE_TRY(mapImpl(contextVk, size));
        }
        *ptrOut = mMappedMemory;
        return angle::Result::Continue;
    }
    void unmap(RendererVk *renderer);
    void flush(RendererVk *renderer,
               VkMemoryMapFlags memoryPropertyFlags,
               VkDeviceSize offset,
               VkDeviceSize size);
    void invalidate(RendererVk *renderer,
                    VkMemoryMapFlags memoryPropertyFlags,
                    VkDeviceSize offset,
                    VkDeviceSize size);

    bool isExternalBuffer() const { return mClientBuffer != nullptr; }

    uint8_t *getMappedMemory() const { return mMappedMemory; }
    DeviceMemory *getExternalMemoryObject() { return &mExternalMemory; }
    Allocation *getMemoryObject() { return &mAllocation; }

  private:
    angle::Result mapImpl(ContextVk *contextVk, VkDeviceSize size);

    Allocation mAllocation;        // use mAllocation if isExternalBuffer() is false
    DeviceMemory mExternalMemory;  // use mExternalMemory if isExternalBuffer() is true

    GLeglClientBufferEXT mClientBuffer;
    uint8_t *mMappedMemory;
};

class BufferHelper final : public ReadWriteResource
{
  public:
    BufferHelper();
    ~BufferHelper() override;

    angle::Result init(ContextVk *contextVk,
                       const VkBufferCreateInfo &createInfo,
                       VkMemoryPropertyFlags memoryPropertyFlags);
    angle::Result initExternal(ContextVk *contextVk,
                               VkMemoryPropertyFlags memoryProperties,
                               const VkBufferCreateInfo &requestedCreateInfo,
                               GLeglClientBufferEXT clientBuffer);
    void destroy(RendererVk *renderer);

    void release(RendererVk *renderer);

    BufferSerial getBufferSerial() const { return mSerial; }
    bool valid() const { return mBuffer.valid(); }
    const Buffer &getBuffer() const { return mBuffer; }
    VkDeviceSize getSize() const { return mSize; }
    uint8_t *getMappedMemory() const
    {
        ASSERT(isMapped());
        return mMemory.getMappedMemory();
    }
    bool isHostVisible() const
    {
        return (mMemoryPropertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;
    }
    bool isCoherent() const
    {
        return (mMemoryPropertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
    }

    bool isMapped() const { return mMemory.getMappedMemory() != nullptr; }
    bool isExternalBuffer() const { return mMemory.isExternalBuffer(); }

    // Also implicitly sets up the correct barriers.
    angle::Result copyFromBuffer(ContextVk *contextVk,
                                 BufferHelper *srcBuffer,
                                 uint32_t regionCount,
                                 const VkBufferCopy *copyRegions);

    angle::Result map(ContextVk *contextVk, uint8_t **ptrOut)
    {
        return mMemory.map(contextVk, mSize, ptrOut);
    }

    angle::Result mapWithOffset(ContextVk *contextVk, uint8_t **ptrOut, size_t offset)
    {
        uint8_t *mapBufPointer;
        ANGLE_TRY(mMemory.map(contextVk, mSize, &mapBufPointer));
        *ptrOut = mapBufPointer + offset;
        return angle::Result::Continue;
    }

    void unmap(RendererVk *renderer);

    // After a sequence of writes, call flush to ensure the data is visible to the device.
    angle::Result flush(RendererVk *renderer, VkDeviceSize offset, VkDeviceSize size);

    // After a sequence of writes, call invalidate to ensure the data is visible to the host.
    angle::Result invalidate(RendererVk *renderer, VkDeviceSize offset, VkDeviceSize size);

    void changeQueue(uint32_t newQueueFamilyIndex, CommandBuffer *commandBuffer);

    // Performs an ownership transfer from an external instance or API.
    void acquireFromExternal(ContextVk *contextVk,
                             uint32_t externalQueueFamilyIndex,
                             uint32_t rendererQueueFamilyIndex,
                             CommandBuffer *commandBuffer);

    // Performs an ownership transfer to an external instance or API.
    void releaseToExternal(ContextVk *contextVk,
                           uint32_t rendererQueueFamilyIndex,
                           uint32_t externalQueueFamilyIndex,
                           CommandBuffer *commandBuffer);

    // Returns true if the image is owned by an external API or instance.
    bool isReleasedToExternal() const;

    bool recordReadBarrier(VkAccessFlags readAccessType,
                           VkPipelineStageFlags readStage,
                           PipelineBarrier *barrier);

    bool recordWriteBarrier(VkAccessFlags writeAccessType,
                            VkPipelineStageFlags writeStage,
                            PipelineBarrier *barrier);

  private:
    angle::Result initializeNonZeroMemory(Context *context, VkDeviceSize size);

    // Vulkan objects.
    Buffer mBuffer;
    BufferMemory mMemory;

    // Cached properties.
    VkMemoryPropertyFlags mMemoryPropertyFlags;
    VkDeviceSize mSize;
    uint32_t mCurrentQueueFamilyIndex;

    // For memory barriers.
    VkFlags mCurrentWriteAccess;
    VkFlags mCurrentReadAccess;
    VkPipelineStageFlags mCurrentWriteStages;
    VkPipelineStageFlags mCurrentReadStages;

    BufferSerial mSerial;
};

enum class BufferAccess
{
    Read,
    Write,
};

enum class AliasingMode
{
    Allowed,
    Disallowed,
};

// Stores clear value In packed attachment index
class PackedClearValuesArray final
{
  public:
    PackedClearValuesArray();
    ~PackedClearValuesArray();

    PackedClearValuesArray(const PackedClearValuesArray &other);
    PackedClearValuesArray &operator=(const PackedClearValuesArray &rhs);
    void store(PackedAttachmentIndex index,
               VkImageAspectFlags aspectFlags,
               const VkClearValue &clearValue);
    void storeNoDepthStencil(PackedAttachmentIndex index, const VkClearValue &clearValue);
    const VkClearValue &operator[](PackedAttachmentIndex index) const
    {
        return mValues[index.get()];
    }
    const VkClearValue *data() const { return mValues.data(); }

  private:
    gl::AttachmentArray<VkClearValue> mValues;
};

// Stores ImageHelpers In packed attachment index
class PackedImageAttachmentArray final
{
  public:
    PackedImageAttachmentArray() : mImages{} {}
    ~PackedImageAttachmentArray() = default;
    ImageHelper *&operator[](PackedAttachmentIndex index) { return mImages[index.get()]; }
    void reset() { mImages.fill(nullptr); }

  private:
    gl::AttachmentArray<ImageHelper *> mImages;
};

// The following are used to help track the state of an invalidated attachment.

// This value indicates an "infinite" CmdSize that is not valid for comparing
constexpr uint32_t kInfiniteCmdSize = 0xFFFFFFFF;

// CommandBufferHelper (CBH) class wraps ANGLE's custom command buffer
//  class, SecondaryCommandBuffer. This provides a way to temporarily
//  store Vulkan commands that be can submitted in-line to a primary
//  command buffer at a later time.
// The current plan is for the main ANGLE thread to record commands
//  into the CBH and then pass the CBH off to a worker thread that will
//  process the commands into a primary command buffer and then submit
//  those commands to the queue.
class CommandBufferHelper : angle::NonCopyable
{
  public:
    CommandBufferHelper();
    ~CommandBufferHelper();

    // General Functions (non-renderPass specific)
    void initialize(bool isRenderPassCommandBuffer);

    void bufferRead(ContextVk *contextVk,
                    VkAccessFlags readAccessType,
                    PipelineStage readStage,
                    BufferHelper *buffer);
    void bufferWrite(ContextVk *contextVk,
                     VkAccessFlags writeAccessType,
                     PipelineStage writeStage,
                     AliasingMode aliasingMode,
                     BufferHelper *buffer);

    void imageRead(ContextVk *contextVk,
                   VkImageAspectFlags aspectFlags,
                   ImageLayout imageLayout,
                   ImageHelper *image);
    void imageWrite(ContextVk *contextVk,
                    gl::LevelIndex level,
                    uint32_t layerStart,
                    uint32_t layerCount,
                    VkImageAspectFlags aspectFlags,
                    ImageLayout imageLayout,
                    AliasingMode aliasingMode,
                    ImageHelper *image);

    void colorImagesDraw(ResourceUseList *resourceUseList,
                         ImageHelper *image,
                         ImageHelper *resolveImage,
                         PackedAttachmentIndex packedAttachmentIndex);
    void depthStencilImagesDraw(ResourceUseList *resourceUseList,
                                gl::LevelIndex level,
                                uint32_t layerStart,
                                uint32_t layerCount,
                                ImageHelper *image,
                                ImageHelper *resolveImage);

    CommandBuffer &getCommandBuffer() { return mCommandBuffer; }

    angle::Result flushToPrimary(const angle::FeaturesVk &features,
                                 PrimaryCommandBuffer *primary,
                                 const RenderPass *renderPass);

    void executeBarriers(const angle::FeaturesVk &features, PrimaryCommandBuffer *primary);

    void setHasRenderPass(bool hasRenderPass) { mIsRenderPassCommandBuffer = hasRenderPass; }

    // The markOpen and markClosed functions are to aid in proper use of the CommandBufferHelper.
    // saw invalid use due to threading issues that can be easily caught by marking when it's safe
    // (open) to write to the commandbuffer.
#if defined(ANGLE_ENABLE_ASSERTS)
    void markOpen() { mCommandBuffer.open(); }
    void markClosed() { mCommandBuffer.close(); }
#else
    void markOpen() {}
    void markClosed() {}
#endif

    void reset();

    // Returns true if we have no work to execute. For renderpass command buffer, even if the
    // underlying command buffer is empty, we may still have a renderpass with an empty command
    // buffer just to do the clear.
    bool empty() const
    {
        return mIsRenderPassCommandBuffer ? !mRenderPassStarted : mCommandBuffer.empty();
    }
    // RenderPass related functions. This is equivalent to !empty(), but only when you know this is
    // a RenderPass command buffer
    bool started() const
    {
        ASSERT(mIsRenderPassCommandBuffer);
        return mRenderPassStarted;
    }

    // Finalize the layout if image has any deferred layout transition.
    void finalizeImageLayout(Context *context, const ImageHelper *image);

    void beginRenderPass(const Framebuffer &framebuffer,
                         const gl::Rectangle &renderArea,
                         const RenderPassDesc &renderPassDesc,
                         const AttachmentOpsArray &renderPassAttachmentOps,
                         const vk::PackedAttachmentCount colorAttachmentCount,
                         const PackedAttachmentIndex depthStencilAttachmentIndex,
                         const PackedClearValuesArray &clearValues,
                         CommandBuffer **commandBufferOut);

    void endRenderPass(ContextVk *contextVk);

    void updateStartedRenderPassWithDepthMode(bool readOnlyDepthStencilMode);

    void beginTransformFeedback(size_t validBufferCount,
                                const VkBuffer *counterBuffers,
                                bool rebindBuffers);

    void endTransformFeedback();

    void invalidateRenderPassColorAttachment(PackedAttachmentIndex attachmentIndex);
    void invalidateRenderPassDepthAttachment(const gl::DepthStencilState &dsState,
                                             const gl::Rectangle &invalidateArea);
    void invalidateRenderPassStencilAttachment(const gl::DepthStencilState &dsState,
                                               const gl::Rectangle &invalidateArea);

    bool hasWriteAfterInvalidate(uint32_t cmdCountInvalidated, uint32_t cmdCountDisabled)
    {
        ASSERT(mIsRenderPassCommandBuffer);
        return (cmdCountInvalidated != kInfiniteCmdSize &&
                std::min(cmdCountDisabled, mCommandBuffer.getCommandSize()) != cmdCountInvalidated);
    }

    bool isInvalidated(uint32_t cmdCountInvalidated, uint32_t cmdCountDisabled)
    {
        ASSERT(mIsRenderPassCommandBuffer);
        return cmdCountInvalidated != kInfiniteCmdSize &&
               std::min(cmdCountDisabled, mCommandBuffer.getCommandSize()) == cmdCountInvalidated;
    }

    void updateRenderPassColorClear(PackedAttachmentIndex colorIndex,
                                    const VkClearValue &colorClearValue);
    void updateRenderPassDepthStencilClear(VkImageAspectFlags aspectFlags,
                                           const VkClearValue &clearValue);

    const gl::Rectangle &getRenderArea() const
    {
        ASSERT(mIsRenderPassCommandBuffer);
        return mRenderArea;
    }

    // If render pass is started with a small render area due to a small scissor, and if a new
    // larger scissor is specified, grow the render area to accomodate it.
    void growRenderArea(ContextVk *contextVk, const gl::Rectangle &newRenderArea);

    void resumeTransformFeedback();
    void pauseTransformFeedback();
    bool isTransformFeedbackStarted() const { return mValidTransformFeedbackBufferCount > 0; }
    bool isTransformFeedbackActiveUnpaused() const { return mIsTransformFeedbackActiveUnpaused; }

    uint32_t getAndResetCounter()
    {
        ASSERT(mIsRenderPassCommandBuffer);
        uint32_t count = mCounter;
        mCounter       = 0;
        return count;
    }

    VkFramebuffer getFramebufferHandle() const
    {
        ASSERT(mIsRenderPassCommandBuffer);
        return mFramebuffer.getHandle();
    }

    bool usesBuffer(const BufferHelper &buffer) const;
    bool usesBufferForWrite(const BufferHelper &buffer) const;
    bool usesImageInRenderPass(const ImageHelper &image) const;
    size_t getUsedBuffersCount() const { return mUsedBuffers.size(); }

    // Dumping the command stream is disabled by default.
    static constexpr bool kEnableCommandStreamDiagnostics = false;

    void onDepthAccess(ResourceAccess access);
    void onStencilAccess(ResourceAccess access);

    void updateRenderPassForResolve(ContextVk *contextVk,
                                    Framebuffer *newFramebuffer,
                                    const RenderPassDesc &renderPassDesc);

    bool hasDepthStencilWriteOrClear() const
    {
        return mDepthAccess == ResourceAccess::Write || mStencilAccess == ResourceAccess::Write ||
               mAttachmentOps[mDepthStencilAttachmentIndex].loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR ||
               mAttachmentOps[mDepthStencilAttachmentIndex].stencilLoadOp ==
                   VK_ATTACHMENT_LOAD_OP_CLEAR;
    }

    void addCommandDiagnostics(ContextVk *contextVk);

    const RenderPassDesc &getRenderPassDesc() const { return mRenderPassDesc; }
    const AttachmentOpsArray &getAttachmentOps() const { return mAttachmentOps; }

    bool hasRenderPass() const { return mIsRenderPassCommandBuffer; }

    void setHasShaderStorageOutput() { mHasShaderStorageOutput = true; }
    bool hasShaderStorageOutput() const { return mHasShaderStorageOutput; }

    void setGLMemoryBarrierIssued()
    {
        if (!empty())
        {
            mHasGLMemoryBarrierIssued = true;
        }
    }
    bool hasGLMemoryBarrierIssued() const { return mHasGLMemoryBarrierIssued; }
    void setImageOptimizeForPresent(ImageHelper *image) { mImageOptimizeForPresent = image; }

  private:
    bool onDepthStencilAccess(ResourceAccess access,
                              uint32_t *cmdCountInvalidated,
                              uint32_t *cmdCountDisabled);
    void restoreDepthContent();
    void restoreStencilContent();

    // We can't determine the image layout at the renderpass start time since their full usage
    // aren't known until later time. We finalize the layout when either ImageHelper object is
    // released or when renderpass ends.
    void finalizeColorImageLayout(Context *context,
                                  ImageHelper *image,
                                  PackedAttachmentIndex packedAttachmentIndex,
                                  bool isResolveImage);
    void finalizeDepthStencilImageLayout(Context *context);
    void finalizeDepthStencilResolveImageLayout(Context *context);
    void finalizeDepthStencilLoadStore(Context *context);
    void finalizeDepthStencilLoadStoreOps(Context *context,
                                          ResourceAccess access,
                                          RenderPassLoadOp *loadOp,
                                          RenderPassStoreOp *storeOp);
    void finalizeDepthStencilImageLayoutAndLoadStore(Context *context);

    void updateImageLayoutAndBarrier(Context *context,
                                     ImageHelper *image,
                                     VkImageAspectFlags aspectFlags,
                                     ImageLayout imageLayout);

    // Allocator used by this class. Using a pool allocator per CBH to avoid threading issues
    //  that occur w/ shared allocator between multiple CBHs.
    angle::PoolAllocator mAllocator;

    // General state (non-renderPass related)
    PipelineBarrierArray mPipelineBarriers;
    PipelineStagesMask mPipelineBarrierMask;
    CommandBuffer mCommandBuffer;

    // RenderPass state
    uint32_t mCounter;
    RenderPassDesc mRenderPassDesc;
    AttachmentOpsArray mAttachmentOps;
    Framebuffer mFramebuffer;
    gl::Rectangle mRenderArea;
    PackedClearValuesArray mClearValues;
    bool mRenderPassStarted;

    // Transform feedback state
    gl::TransformFeedbackBuffersArray<VkBuffer> mTransformFeedbackCounterBuffers;
    uint32_t mValidTransformFeedbackBufferCount;
    bool mRebindTransformFeedbackBuffers;
    bool mIsTransformFeedbackActiveUnpaused;

    bool mIsRenderPassCommandBuffer;

    // Whether the command buffers contains any draw/dispatch calls that possibly output data
    // through storage buffers and images.  This is used to determine whether glMemoryBarrier*
    // should flush the command buffer.
    bool mHasShaderStorageOutput;
    // Whether glMemoryBarrier has been called while commands are recorded in this command buffer.
    // This is used to know when to check and potentially flush the command buffer if storage
    // buffers and images are used in it.
    bool mHasGLMemoryBarrierIssued;

    // State tracking for the maximum (Write been the highest) depth access during the entire
    // renderpass. Note that this does not include VK_ATTACHMENT_LOAD_OP_CLEAR which is tracked
    // separately. This is done this way to allow clear op to being optimized out when we find out
    // that the depth buffer is not being used during the entire renderpass and store op is
    // VK_ATTACHMENT_STORE_OP_DONTCARE.
    ResourceAccess mDepthAccess;
    // Similar tracking to mDepthAccess but for the stencil aspect.
    ResourceAccess mStencilAccess;

    // State tracking for whether to optimize the storeOp to DONT_CARE
    uint32_t mDepthCmdSizeInvalidated;
    uint32_t mDepthCmdSizeDisabled;
    uint32_t mStencilCmdSizeInvalidated;
    uint32_t mStencilCmdSizeDisabled;
    gl::Rectangle mDepthInvalidateArea;
    gl::Rectangle mStencilInvalidateArea;

    // Keep track of the depth/stencil attachment index
    PackedAttachmentIndex mDepthStencilAttachmentIndex;

    // Tracks resources used in the command buffer.
    // For Buffers, we track the read/write access type so we can enable simultaneous reads.
    // Images have unique layouts unlike buffers therefore we don't support multi-read.
    angle::FastIntegerMap<BufferAccess> mUsedBuffers;
    angle::FastIntegerSet mRenderPassUsedImages;

    ImageHelper *mDepthStencilImage;
    ImageHelper *mDepthStencilResolveImage;
    gl::LevelIndex mDepthStencilLevelIndex;
    uint32_t mDepthStencilLayerIndex;
    uint32_t mDepthStencilLayerCount;

    // Array size of mColorImages
    PackedAttachmentCount mColorImagesCount;
    // Attached render target images. Color and depth resolve images are always come last.
    PackedImageAttachmentArray mColorImages;
    PackedImageAttachmentArray mColorResolveImages;
    // This is last renderpass before present and this is the image will be presented. We can use
    // final layout of the renderpass to transit it to the presentable layout
    ImageHelper *mImageOptimizeForPresent;
};

// Imagine an image going through a few layout transitions:
//
//           srcStage 1    dstStage 2          srcStage 2     dstStage 3
//  Layout 1 ------Transition 1-----> Layout 2 ------Transition 2------> Layout 3
//           srcAccess 1  dstAccess 2          srcAccess 2   dstAccess 3
//   \_________________  ___________________/
//                     \/
//               A transition
//
// Every transition requires 6 pieces of information: from/to layouts, src/dst stage masks and
// src/dst access masks.  At the moment we decide to transition the image to Layout 2 (i.e.
// Transition 1), we need to have Layout 1, srcStage 1 and srcAccess 1 stored as history of the
// image.  To perform the transition, we need to know Layout 2, dstStage 2 and dstAccess 2.
// Additionally, we need to know srcStage 2 and srcAccess 2 to retain them for the next transition.
//
// That is, with the history kept, on every new transition we need 5 pieces of new information:
// layout/dstStage/dstAccess to transition into the layout, and srcStage/srcAccess for the future
// transition out from it.  Given the small number of possible combinations of these values, an
// enum is used were each value encapsulates these 5 pieces of information:
//
//                       +--------------------------------+
//           srcStage 1  | dstStage 2          srcStage 2 |   dstStage 3
//  Layout 1 ------Transition 1-----> Layout 2 ------Transition 2------> Layout 3
//           srcAccess 1 |dstAccess 2          srcAccess 2|  dstAccess 3
//                       +---------------  ---------------+
//                                       \/
//                                 One enum value
//
// Note that, while generally dstStage for the to-transition and srcStage for the from-transition
// are the same, they may occasionally be BOTTOM_OF_PIPE and TOP_OF_PIPE respectively.
enum class ImageLayout
{
    Undefined = 0,
    // Framebuffer attachment layouts are placed first, so they can fit in fewer bits in
    // PackedAttachmentOpsDesc.
    ColorAttachment,
    ColorAttachmentAndFragmentShaderRead,
    ColorAttachmentAndAllShadersRead,
    DSAttachmentWriteAndFragmentShaderRead,
    DSAttachmentWriteAndAllShadersRead,
    DSAttachmentReadAndFragmentShaderRead,
    DSAttachmentReadAndAllShadersRead,
    DepthStencilAttachmentReadOnly,
    DepthStencilAttachment,
    DepthStencilResolveAttachment,
    Present,
    // The rest of the layouts.
    ExternalPreInitialized,
    ExternalShadersReadOnly,
    ExternalShadersWrite,
    TransferSrc,
    TransferDst,
    VertexShaderReadOnly,
    VertexShaderWrite,
    // PreFragment == Vertex, Tessellation and Geometry stages
    PreFragmentShadersReadOnly,
    PreFragmentShadersWrite,
    FragmentShaderReadOnly,
    FragmentShaderWrite,
    ComputeShaderReadOnly,
    ComputeShaderWrite,
    AllGraphicsShadersReadOnly,
    AllGraphicsShadersWrite,

    InvalidEnum,
    EnumCount = InvalidEnum,
};

VkImageLayout ConvertImageLayoutToVkImageLayout(ImageLayout imageLayout);

// How the ImageHelper object is being used by the renderpass
enum class RenderPassUsage
{
    // Attached to the render taget of the current renderpass commands. It could be read/write or
    // read only access.
    RenderTargetAttachment,
    // This is special case of RenderTargetAttachment where the render target access is read only.
    // Right now it is only tracked for depth stencil attachment
    ReadOnlyAttachment,
    // Attached to the texture sampler of the current renderpass commands
    TextureSampler,

    InvalidEnum,
    EnumCount = InvalidEnum,
};
using RenderPassUsageFlags = angle::PackedEnumBitSet<RenderPassUsage, uint16_t>;

bool FormatHasNecessaryFeature(RendererVk *renderer,
                               angle::FormatID formatID,
                               VkImageTiling tilingMode,
                               VkFormatFeatureFlags featureBits);

bool CanCopyWithTransfer(RendererVk *renderer,
                         angle::FormatID srcFormatID,
                         VkImageTiling srcTilingMode,
                         angle::FormatID destFormatID,
                         VkImageTiling destTilingMode);

class ImageHelper final : public Resource, public angle::Subject
{
  public:
    ImageHelper();
    ImageHelper(ImageHelper &&other);
    ~ImageHelper() override;

    void initStagingBuffer(RendererVk *renderer,
                           size_t imageCopyBufferAlignment,
                           VkBufferUsageFlags usageFlags,
                           size_t initialSize);

    angle::Result init(Context *context,
                       gl::TextureType textureType,
                       const VkExtent3D &extents,
                       const Format &format,
                       GLint samples,
                       VkImageUsageFlags usage,
                       gl::LevelIndex firstLevel,
                       uint32_t mipLevels,
                       uint32_t layerCount,
                       bool isRobustResourceInitEnabled,
                       bool hasProtectedContent);
    angle::Result initMSAASwapchain(Context *context,
                                    gl::TextureType textureType,
                                    const VkExtent3D &extents,
                                    bool rotatedAspectRatio,
                                    const Format &format,
                                    GLint samples,
                                    VkImageUsageFlags usage,
                                    gl::LevelIndex firstLevel,
                                    uint32_t mipLevels,
                                    uint32_t layerCount,
                                    bool isRobustResourceInitEnabled,
                                    bool hasProtectedContent);
    angle::Result initExternal(Context *context,
                               gl::TextureType textureType,
                               const VkExtent3D &extents,
                               angle::FormatID intendedFormatID,
                               angle::FormatID actualFormatID,
                               GLint samples,
                               VkImageUsageFlags usage,
                               VkImageCreateFlags additionalCreateFlags,
                               ImageLayout initialLayout,
                               const void *externalImageCreateInfo,
                               gl::LevelIndex firstLevel,
                               uint32_t mipLevels,
                               uint32_t layerCount,
                               bool isRobustResourceInitEnabled,
                               bool *imageFormatListEnabledOut,
                               bool hasProtectedContent);
    angle::Result initMemory(Context *context,
                             bool hasProtectedContent,
                             const MemoryProperties &memoryProperties,
                             VkMemoryPropertyFlags flags);
    angle::Result initExternalMemory(
        Context *context,
        const MemoryProperties &memoryProperties,
        const VkMemoryRequirements &memoryRequirements,
        const VkSamplerYcbcrConversionCreateInfo *samplerYcbcrConversionCreateInfo,
        uint32_t extraAllocationInfoCount,
        const void **extraAllocationInfo,
        uint32_t currentQueueFamilyIndex,
        VkMemoryPropertyFlags flags);
    angle::Result initLayerImageView(Context *context,
                                     gl::TextureType textureType,
                                     VkImageAspectFlags aspectMask,
                                     const gl::SwizzleState &swizzleMap,
                                     ImageView *imageViewOut,
                                     LevelIndex baseMipLevelVk,
                                     uint32_t levelCount,
                                     uint32_t baseArrayLayer,
                                     uint32_t layerCount,
                                     gl::SrgbWriteControlMode mode) const;
    angle::Result initLayerImageViewWithFormat(Context *context,
                                               gl::TextureType textureType,
                                               VkFormat imageFormat,
                                               VkImageAspectFlags aspectMask,
                                               const gl::SwizzleState &swizzleMap,
                                               ImageView *imageViewOut,
                                               LevelIndex baseMipLevelVk,
                                               uint32_t levelCount,
                                               uint32_t baseArrayLayer,
                                               uint32_t layerCount) const;
    angle::Result initReinterpretedLayerImageView(Context *context,
                                                  gl::TextureType textureType,
                                                  VkImageAspectFlags aspectMask,
                                                  const gl::SwizzleState &swizzleMap,
                                                  ImageView *imageViewOut,
                                                  LevelIndex baseMipLevelVk,
                                                  uint32_t levelCount,
                                                  uint32_t baseArrayLayer,
                                                  uint32_t layerCount,
                                                  VkImageUsageFlags imageUsageFlags,
                                                  angle::FormatID imageViewFormat) const;
    angle::Result initImageView(Context *context,
                                gl::TextureType textureType,
                                VkImageAspectFlags aspectMask,
                                const gl::SwizzleState &swizzleMap,
                                ImageView *imageViewOut,
                                LevelIndex baseMipLevelVk,
                                uint32_t levelCount);
    // Create a 2D[Array] for staging purposes.  Used by:
    //
    // - TextureVk::copySubImageImplWithDraw
    // - FramebufferVk::readPixelsImpl
    //
    angle::Result init2DStaging(Context *context,
                                bool hasProtectedContent,
                                const MemoryProperties &memoryProperties,
                                const gl::Extents &glExtents,
                                angle::FormatID intendedFormatID,
                                angle::FormatID actualFormatID,
                                VkImageUsageFlags usage,
                                uint32_t layerCount);
    // Create an image for staging purposes.  Used by:
    //
    // - TextureVk::copyAndStageImageData
    //
    angle::Result initStaging(Context *context,
                              bool hasProtectedContent,
                              const MemoryProperties &memoryProperties,
                              VkImageType imageType,
                              const VkExtent3D &extents,
                              angle::FormatID intendedFormatID,
                              angle::FormatID actualFormatID,
                              GLint samples,
                              VkImageUsageFlags usage,
                              uint32_t mipLevels,
                              uint32_t layerCount);
    // Create a multisampled image for use as the implicit image in multisampled render to texture
    // rendering.  If LAZILY_ALLOCATED memory is available, it will prefer that.
    angle::Result initImplicitMultisampledRenderToTexture(Context *context,
                                                          bool hasProtectedContent,
                                                          const MemoryProperties &memoryProperties,
                                                          gl::TextureType textureType,
                                                          GLint samples,
                                                          const ImageHelper &resolveImage,
                                                          bool isRobustResourceInitEnabled);
    // Release the underlining VkImage object for garbage collection.
    void releaseImage(RendererVk *renderer);
    // Similar to releaseImage, but also notify all contexts in the same share group to stop
    // accessing to it.
    void releaseImageFromShareContexts(RendererVk *renderer, ContextVk *contextVk);
    void releaseStagingBuffer(RendererVk *renderer);

    bool valid() const { return mImage.valid(); }

    VkImageAspectFlags getAspectFlags() const;
    // True if image contains both depth & stencil aspects
    bool isCombinedDepthStencilFormat() const;
    void destroy(RendererVk *renderer);
    void release(RendererVk *renderer) { destroy(renderer); }

    void init2DWeakReference(Context *context,
                             VkImage handle,
                             const gl::Extents &glExtents,
                             bool rotatedAspectRatio,
                             const Format &format,
                             GLint samples,
                             bool isRobustResourceInitEnabled);
    void resetImageWeakReference();

    const Image &getImage() const { return mImage; }
    const DeviceMemory &getDeviceMemory() const { return mDeviceMemory; }

    void setTilingMode(VkImageTiling tilingMode) { mTilingMode = tilingMode; }
    VkImageTiling getTilingMode() const { return mTilingMode; }
    VkImageCreateFlags getCreateFlags() const { return mCreateFlags; }
    VkImageUsageFlags getUsage() const { return mUsage; }
    VkImageType getType() const { return mImageType; }
    const VkExtent3D &getExtents() const { return mExtents; }
    const VkExtent3D getRotatedExtents() const;
    uint32_t getLayerCount() const { return mLayerCount; }
    uint32_t getLevelCount() const { return mLevelCount; }
    angle::FormatID getIntendedFormatID() const { return mIntendedFormatID; }
    const angle::Format &getIntendedFormat() const { return angle::Format::Get(mIntendedFormatID); }
    angle::FormatID getActualFormatID() const { return mActualFormatID; }
    VkFormat getActualVkFormat() const { return GetVkFormatFromFormatID(mActualFormatID); }
    const angle::Format &getActualFormat() const { return angle::Format::Get(mActualFormatID); }
    bool hasEmulatedImageChannels() const;
    bool hasEmulatedImageFormat() const { return mActualFormatID != mIntendedFormatID; }
    GLint getSamples() const { return mSamples; }

    ImageSerial getImageSerial() const
    {
        ASSERT(valid() && mImageSerial.valid());
        return mImageSerial;
    }

    void setCurrentImageLayout(ImageLayout newLayout) { mCurrentLayout = newLayout; }
    ImageLayout getCurrentImageLayout() const { return mCurrentLayout; }
    VkImageLayout getCurrentLayout() const;

    gl::Extents getLevelExtents(LevelIndex levelVk) const;
    // Helper function to calculate the extents of a render target created for a certain mip of the
    // image.
    gl::Extents getLevelExtents2D(LevelIndex levelVk) const;
    gl::Extents getRotatedLevelExtents2D(LevelIndex levelVk) const;

    bool isDepthOrStencil() const;

    void setRenderPassUsageFlag(RenderPassUsage flag);
    void clearRenderPassUsageFlag(RenderPassUsage flag);
    void resetRenderPassUsageFlags();
    bool hasRenderPassUsageFlag(RenderPassUsage flag) const;
    bool usedByCurrentRenderPassAsAttachmentAndSampler() const;

    // Clear either color or depth/stencil based on image format.
    void clear(VkImageAspectFlags aspectFlags,
               const VkClearValue &value,
               LevelIndex mipLevel,
               uint32_t baseArrayLayer,
               uint32_t layerCount,
               CommandBuffer *commandBuffer);

    static void Copy(ImageHelper *srcImage,
                     ImageHelper *dstImage,
                     const gl::Offset &srcOffset,
                     const gl::Offset &dstOffset,
                     const gl::Extents &copySize,
                     const VkImageSubresourceLayers &srcSubresources,
                     const VkImageSubresourceLayers &dstSubresources,
                     CommandBuffer *commandBuffer);

    static angle::Result CopyImageSubData(const gl::Context *context,
                                          ImageHelper *srcImage,
                                          GLint srcLevel,
                                          GLint srcX,
                                          GLint srcY,
                                          GLint srcZ,
                                          ImageHelper *dstImage,
                                          GLint dstLevel,
                                          GLint dstX,
                                          GLint dstY,
                                          GLint dstZ,
                                          GLsizei srcWidth,
                                          GLsizei srcHeight,
                                          GLsizei srcDepth);

    // Generate mipmap from level 0 into the rest of the levels with blit.
    angle::Result generateMipmapsWithBlit(ContextVk *contextVk,
                                          LevelIndex baseLevel,
                                          LevelIndex maxLevel);

    // Resolve this image into a destination image.  This image should be in the TransferSrc layout.
    // The destination image is automatically transitioned into TransferDst.
    void resolve(ImageHelper *dest, const VkImageResolve &region, CommandBuffer *commandBuffer);

    // Data staging
    void removeSingleSubresourceStagedUpdates(ContextVk *contextVk,
                                              gl::LevelIndex levelIndexGL,
                                              uint32_t layerIndex,
                                              uint32_t layerCount);
    void removeStagedUpdates(Context *context,
                             gl::LevelIndex levelGLStart,
                             gl::LevelIndex levelGLEnd);

    angle::Result stageSubresourceUpdateImpl(ContextVk *contextVk,
                                             const gl::ImageIndex &index,
                                             const gl::Extents &glExtents,
                                             const gl::Offset &offset,
                                             const gl::InternalFormat &formatInfo,
                                             const gl::PixelUnpackState &unpack,
                                             DynamicBuffer *stagingBufferOverride,
                                             GLenum type,
                                             const uint8_t *pixels,
                                             const Format &vkFormat,
                                             ImageAccess access,
                                             const GLuint inputRowPitch,
                                             const GLuint inputDepthPitch,
                                             const GLuint inputSkipBytes);

    angle::Result stageSubresourceUpdate(ContextVk *contextVk,
                                         const gl::ImageIndex &index,
                                         const gl::Extents &glExtents,
                                         const gl::Offset &offset,
                                         const gl::InternalFormat &formatInfo,
                                         const gl::PixelUnpackState &unpack,
                                         DynamicBuffer *stagingBufferOverride,
                                         GLenum type,
                                         const uint8_t *pixels,
                                         const Format &vkFormat,
                                         ImageAccess access);

    angle::Result stageSubresourceUpdateAndGetData(ContextVk *contextVk,
                                                   size_t allocationSize,
                                                   const gl::ImageIndex &imageIndex,
                                                   const gl::Extents &glExtents,
                                                   const gl::Offset &offset,
                                                   uint8_t **destData,
                                                   DynamicBuffer *stagingBufferOverride,
                                                   angle::FormatID formatID);

    angle::Result stageSubresourceUpdateFromFramebuffer(const gl::Context *context,
                                                        const gl::ImageIndex &index,
                                                        const gl::Rectangle &sourceArea,
                                                        const gl::Offset &dstOffset,
                                                        const gl::Extents &dstExtent,
                                                        const gl::InternalFormat &formatInfo,
                                                        ImageAccess access,
                                                        FramebufferVk *framebufferVk,
                                                        DynamicBuffer *stagingBufferOverride);

    void stageSubresourceUpdateFromImage(RefCounted<ImageHelper> *image,
                                         const gl::ImageIndex &index,
                                         LevelIndex srcMipLevel,
                                         const gl::Offset &destOffset,
                                         const gl::Extents &glExtents,
                                         const VkImageType imageType);

    // Takes an image and stages a subresource update for each level of it, including its full
    // extent and all its layers, at the specified GL level.
    void stageSubresourceUpdatesFromAllImageLevels(RefCounted<ImageHelper> *image,
                                                   gl::LevelIndex baseLevel);

    // Stage a clear to an arbitrary value.
    void stageClear(const gl::ImageIndex &index,
                    VkImageAspectFlags aspectFlags,
                    const VkClearValue &clearValue);

    // Stage a clear based on robust resource init.
    angle::Result stageRobustResourceClearWithFormat(ContextVk *contextVk,
                                                     const gl::ImageIndex &index,
                                                     const gl::Extents &glExtents,
                                                     const angle::Format &intendedFormat,
                                                     const angle::Format &actualFormat);
    void stageRobustResourceClear(const gl::ImageIndex &index);

    // Stage the currently allocated image as updates to base level and on, making this !valid().
    // This is used for:
    //
    // - Mipmap generation, where levelCount is 1 so only the base level is retained
    // - Image respecification, where every level (other than those explicitly skipped) is staged
    void stageSelfAsSubresourceUpdates(ContextVk *contextVk,
                                       uint32_t levelCount,
                                       gl::TexLevelMask skipLevelsMask);

    // Flush staged updates for a single subresource. Can optionally take a parameter to defer
    // clears to a subsequent RenderPass load op.
    angle::Result flushSingleSubresourceStagedUpdates(ContextVk *contextVk,
                                                      gl::LevelIndex levelGL,
                                                      uint32_t layer,
                                                      uint32_t layerCount,
                                                      ClearValuesArray *deferredClears,
                                                      uint32_t deferredClearIndex);

    // Flushes staged updates to a range of levels and layers from start to (but not including) end.
    // Due to the nature of updates (done wholly to a VkImageSubresourceLayers), some unsolicited
    // layers may also be updated.
    angle::Result flushStagedUpdates(ContextVk *contextVk,
                                     gl::LevelIndex levelGLStart,
                                     gl::LevelIndex levelGLEnd,
                                     uint32_t layerStart,
                                     uint32_t layerEnd,
                                     gl::TexLevelMask skipLevelsMask);

    // Creates a command buffer and flushes all staged updates.  This is used for one-time
    // initialization of resources that we don't expect to accumulate further staged updates, such
    // as with renderbuffers or surface images.
    angle::Result flushAllStagedUpdates(ContextVk *contextVk);

    bool hasStagedUpdatesForSubresource(gl::LevelIndex levelGL,
                                        uint32_t layer,
                                        uint32_t layerCount) const;
    bool hasStagedUpdatesInAllocatedLevels() const;

    void recordWriteBarrier(Context *context,
                            VkImageAspectFlags aspectMask,
                            ImageLayout newLayout,
                            CommandBuffer *commandBuffer)
    {
        barrierImpl(context, aspectMask, newLayout, mCurrentQueueFamilyIndex, commandBuffer);
    }

    // This function can be used to prevent issuing redundant layout transition commands.
    bool isReadBarrierNecessary(ImageLayout newLayout) const;

    void recordReadBarrier(Context *context,
                           VkImageAspectFlags aspectMask,
                           ImageLayout newLayout,
                           CommandBuffer *commandBuffer)
    {
        if (!isReadBarrierNecessary(newLayout))
        {
            return;
        }

        barrierImpl(context, aspectMask, newLayout, mCurrentQueueFamilyIndex, commandBuffer);
    }

    bool isQueueChangeNeccesary(uint32_t newQueueFamilyIndex) const
    {
        return mCurrentQueueFamilyIndex != newQueueFamilyIndex;
    }

    void changeLayoutAndQueue(Context *context,
                              VkImageAspectFlags aspectMask,
                              ImageLayout newLayout,
                              uint32_t newQueueFamilyIndex,
                              CommandBuffer *commandBuffer);

    // Returns true if barrier has been generated
    bool updateLayoutAndBarrier(Context *context,
                                VkImageAspectFlags aspectMask,
                                ImageLayout newLayout,
                                PipelineBarrier *barrier);

    // Performs an ownership transfer from an external instance or API.
    void acquireFromExternal(ContextVk *contextVk,
                             uint32_t externalQueueFamilyIndex,
                             uint32_t rendererQueueFamilyIndex,
                             ImageLayout currentLayout,
                             CommandBuffer *commandBuffer);

    // Performs an ownership transfer to an external instance or API.
    void releaseToExternal(ContextVk *contextVk,
                           uint32_t rendererQueueFamilyIndex,
                           uint32_t externalQueueFamilyIndex,
                           ImageLayout desiredLayout,
                           CommandBuffer *commandBuffer);

    // Returns true if the image is owned by an external API or instance.
    bool isReleasedToExternal() const;

    gl::LevelIndex getFirstAllocatedLevel() const { return mFirstAllocatedLevel; }
    void setFirstAllocatedLevel(gl::LevelIndex firstLevel);
    gl::LevelIndex getLastAllocatedLevel() const;
    LevelIndex toVkLevel(gl::LevelIndex levelIndexGL) const;
    gl::LevelIndex toGLLevel(LevelIndex levelIndexVk) const;

    angle::Result copyImageDataToBuffer(ContextVk *contextVk,
                                        gl::LevelIndex sourceLevelGL,
                                        uint32_t layerCount,
                                        uint32_t baseLayer,
                                        const gl::Box &sourceArea,
                                        BufferHelper **bufferOut,
                                        size_t *bufferSize,
                                        StagingBufferOffsetArray *bufferOffsetsOut,
                                        uint8_t **outDataPtr);

    static angle::Result GetReadPixelsParams(ContextVk *contextVk,
                                             const gl::PixelPackState &packState,
                                             gl::Buffer *packBuffer,
                                             GLenum format,
                                             GLenum type,
                                             const gl::Rectangle &area,
                                             const gl::Rectangle &clippedArea,
                                             PackPixelsParams *paramsOut,
                                             GLuint *skipBytesOut);

    angle::Result readPixelsForGetImage(ContextVk *contextVk,
                                        const gl::PixelPackState &packState,
                                        gl::Buffer *packBuffer,
                                        gl::LevelIndex levelGL,
                                        uint32_t layer,
                                        uint32_t layerCount,
                                        GLenum format,
                                        GLenum type,
                                        void *pixels);

    angle::Result readPixels(ContextVk *contextVk,
                             const gl::Rectangle &area,
                             const PackPixelsParams &packPixelsParams,
                             VkImageAspectFlagBits copyAspectFlags,
                             gl::LevelIndex levelGL,
                             uint32_t layer,
                             void *pixels,
                             DynamicBuffer *stagingBuffer);

    angle::Result CalculateBufferInfo(ContextVk *contextVk,
                                      const gl::Extents &glExtents,
                                      const gl::InternalFormat &formatInfo,
                                      const gl::PixelUnpackState &unpack,
                                      GLenum type,
                                      bool is3D,
                                      GLuint *inputRowPitch,
                                      GLuint *inputDepthPitch,
                                      GLuint *inputSkipBytes);

    // Mark a given subresource as written to.  The subresource is identified by [levelStart,
    // levelStart + levelCount) and [layerStart, layerStart + layerCount).
    void onWrite(gl::LevelIndex levelStart,
                 uint32_t levelCount,
                 uint32_t layerStart,
                 uint32_t layerCount,
                 VkImageAspectFlags aspectFlags);
    bool hasImmutableSampler() const;
    uint64_t getExternalFormat() const { return mExternalFormat; }

    // Used by framebuffer and render pass functions to decide loadOps and invalidate/un-invalidate
    // render target contents.
    bool hasSubresourceDefinedContent(gl::LevelIndex level,
                                      uint32_t layerIndex,
                                      uint32_t layerCount) const;
    bool hasSubresourceDefinedStencilContent(gl::LevelIndex level,
                                             uint32_t layerIndex,
                                             uint32_t layerCount) const;
    void invalidateSubresourceContent(ContextVk *contextVk,
                                      gl::LevelIndex level,
                                      uint32_t layerIndex,
                                      uint32_t layerCount);
    void invalidateSubresourceStencilContent(ContextVk *contextVk,
                                             gl::LevelIndex level,
                                             uint32_t layerIndex,
                                             uint32_t layerCount);
    void restoreSubresourceContent(gl::LevelIndex level, uint32_t layerIndex, uint32_t layerCount);
    void restoreSubresourceStencilContent(gl::LevelIndex level,
                                          uint32_t layerIndex,
                                          uint32_t layerCount);
    angle::Result reformatStagedUpdate(ContextVk *contextVk,
                                       angle::FormatID srcFormatID,
                                       angle::FormatID dstFormatID);

  private:
    enum class UpdateSource
    {
        Clear,
        Buffer,
        Image,
    };
    ANGLE_ENABLE_STRUCT_PADDING_WARNINGS
    struct ClearUpdate
    {
        bool operator==(const ClearUpdate &rhs)
        {
            return memcmp(this, &rhs, sizeof(ClearUpdate)) == 0;
        }
        VkImageAspectFlags aspectFlags;
        VkClearValue value;
        uint32_t levelIndex;
        uint32_t layerIndex;
        uint32_t layerCount;
    };
    ANGLE_DISABLE_STRUCT_PADDING_WARNINGS
    struct BufferUpdate
    {
        BufferHelper *bufferHelper;
        VkBufferImageCopy copyRegion;
        angle::FormatID formatID;
    };
    struct ImageUpdate
    {
        VkImageCopy copyRegion;
        angle::FormatID formatID;
    };

    struct SubresourceUpdate : angle::NonCopyable
    {
        SubresourceUpdate();
        ~SubresourceUpdate();
        SubresourceUpdate(BufferHelper *bufferHelperIn,
                          const VkBufferImageCopy &copyRegion,
                          angle::FormatID formatID);
        SubresourceUpdate(RefCounted<ImageHelper> *imageIn,
                          const VkImageCopy &copyRegion,
                          angle::FormatID formatID);
        SubresourceUpdate(VkImageAspectFlags aspectFlags,
                          const VkClearValue &clearValue,
                          const gl::ImageIndex &imageIndex);
        SubresourceUpdate(SubresourceUpdate &&other);

        SubresourceUpdate &operator=(SubresourceUpdate &&other);

        void release(RendererVk *renderer);

        bool isUpdateToLayers(uint32_t layerIndex, uint32_t layerCount) const;
        void getDestSubresource(uint32_t imageLayerCount,
                                uint32_t *baseLayerOut,
                                uint32_t *layerCountOut) const;
        VkImageAspectFlags getDestAspectFlags() const;

        UpdateSource updateSource;
        union
        {
            ClearUpdate clear;
            BufferUpdate buffer;
            ImageUpdate image;
        } data;
        RefCounted<ImageHelper> *image;
    };

    // Called from flushStagedUpdates, removes updates that are later superseded by another.  This
    // cannot be done at the time the updates were staged, as the image is not created (and thus the
    // extents are not known).
    void removeSupersededUpdates(ContextVk *contextVk, gl::TexLevelMask skipLevelsMask);

    void initImageMemoryBarrierStruct(VkImageAspectFlags aspectMask,
                                      ImageLayout newLayout,
                                      uint32_t newQueueFamilyIndex,
                                      VkImageMemoryBarrier *imageMemoryBarrier) const;

    // Generalized to accept both "primary" and "secondary" command buffers.
    template <typename CommandBufferT>
    void barrierImpl(Context *context,
                     VkImageAspectFlags aspectMask,
                     ImageLayout newLayout,
                     uint32_t newQueueFamilyIndex,
                     CommandBufferT *commandBuffer);

    // If the image has emulated channels, we clear them once so as not to leave garbage on those
    // channels.
    void stageClearIfEmulatedFormat(bool isRobustResourceInitEnabled);

    void clearColor(const VkClearColorValue &color,
                    LevelIndex baseMipLevelVk,
                    uint32_t levelCount,
                    uint32_t baseArrayLayer,
                    uint32_t layerCount,
                    CommandBuffer *commandBuffer);

    void clearDepthStencil(VkImageAspectFlags clearAspectFlags,
                           const VkClearDepthStencilValue &depthStencil,
                           LevelIndex baseMipLevelVk,
                           uint32_t levelCount,
                           uint32_t baseArrayLayer,
                           uint32_t layerCount,
                           CommandBuffer *commandBuffer);

    angle::Result initializeNonZeroMemory(Context *context,
                                          bool hasProtectedContent,
                                          VkDeviceSize size);

    std::vector<SubresourceUpdate> *getLevelUpdates(gl::LevelIndex level);
    const std::vector<SubresourceUpdate> *getLevelUpdates(gl::LevelIndex level) const;

    void appendSubresourceUpdate(gl::LevelIndex level, SubresourceUpdate &&update);
    void prependSubresourceUpdate(gl::LevelIndex level, SubresourceUpdate &&update);
    // Whether there are any updates in [start, end).
    bool hasStagedUpdatesInLevels(gl::LevelIndex levelStart, gl::LevelIndex levelEnd) const;

    // Used only for assertions, these functions verify that SubresourceUpdate::image references
    // have the correct ref count.  This is to prevent accidental leaks.
    bool validateSubresourceUpdateImageRefConsistent(RefCounted<ImageHelper> *image) const;
    bool validateSubresourceUpdateImageRefsConsistent() const;

    void resetCachedProperties();
    void setEntireContentDefined();
    void setEntireContentUndefined();
    void setContentDefined(LevelIndex levelStart,
                           uint32_t levelCount,
                           uint32_t layerStart,
                           uint32_t layerCount,
                           VkImageAspectFlags aspectFlags);

    // Up to 8 layers are tracked per level for whether contents are defined, above which the
    // contents are considered unconditionally defined.  This handles the more likely scenarios of:
    //
    // - Single layer framebuffer attachments,
    // - Cube map framebuffer attachments,
    // - Multi-view rendering.
    //
    // If there arises a need to optimize an application that invalidates layer >= 8, an additional
    // hash map can be used to track such subresources.
    static constexpr uint32_t kMaxContentDefinedLayerCount = 8;
    using LevelContentDefinedMask = angle::BitSet8<kMaxContentDefinedLayerCount>;

    // Use the following functions to access m*ContentDefined to make sure the correct level index
    // is used (i.e. vk::LevelIndex and not gl::LevelIndex).
    LevelContentDefinedMask &getLevelContentDefined(LevelIndex level);
    LevelContentDefinedMask &getLevelStencilContentDefined(LevelIndex level);
    const LevelContentDefinedMask &getLevelContentDefined(LevelIndex level) const;
    const LevelContentDefinedMask &getLevelStencilContentDefined(LevelIndex level) const;

    angle::Result initLayerImageViewImpl(
        Context *context,
        gl::TextureType textureType,
        VkImageAspectFlags aspectMask,
        const gl::SwizzleState &swizzleMap,
        ImageView *imageViewOut,
        LevelIndex baseMipLevelVk,
        uint32_t levelCount,
        uint32_t baseArrayLayer,
        uint32_t layerCount,
        VkFormat imageFormat,
        const VkImageViewUsageCreateInfo *imageViewUsageCreateInfo) const;

    bool canCopyWithTransformForReadPixels(const PackPixelsParams &packPixelsParams,
                                           const angle::Format *readFormat);
    // Vulkan objects.
    Image mImage;
    DeviceMemory mDeviceMemory;

    // Image properties.
    VkImageType mImageType;
    VkImageTiling mTilingMode;
    VkImageCreateFlags mCreateFlags;
    VkImageUsageFlags mUsage;
    // For Android swapchain images, the Vulkan VkImage must be "rotated".  However, most of ANGLE
    // uses non-rotated extents (i.e. the way the application views the extents--see "Introduction
    // to Android rotation and pre-rotation" in "SurfaceVk.cpp").  Thus, mExtents are non-rotated.
    // The rotated extents are also stored along with a bool that indicates if the aspect ratio is
    // different between the rotated and non-rotated extents.
    VkExtent3D mExtents;
    bool mRotatedAspectRatio;
    angle::FormatID mIntendedFormatID;
    angle::FormatID mActualFormatID;
    GLint mSamples;
    ImageSerial mImageSerial;

    // Current state.
    ImageLayout mCurrentLayout;
    uint32_t mCurrentQueueFamilyIndex;
    // For optimizing transition between different shader readonly layouts
    ImageLayout mLastNonShaderReadOnlyLayout;
    VkPipelineStageFlags mCurrentShaderReadStageMask;
    // Track how it is being used by current open renderpass.
    RenderPassUsageFlags mRenderPassUsageFlags;

    // For imported images
    BindingPointer<SamplerYcbcrConversion> mYuvConversionSampler;
    uint64_t mExternalFormat;

    // The first level that has been allocated. For mutable textures, this should be same as
    // mBaseLevel since we always reallocate VkImage based on mBaseLevel change. But for immutable
    // textures, we always allocate from level 0 regardless of mBaseLevel change.
    gl::LevelIndex mFirstAllocatedLevel;

    // Cached properties.
    uint32_t mLayerCount;
    uint32_t mLevelCount;

    // Staging buffer
    DynamicBuffer mStagingBuffer;
    std::vector<std::vector<SubresourceUpdate>> mSubresourceUpdates;

    // Optimization for repeated clear with the same value. If this pointer is not null, the entire
    // image it has been cleared to the specified clear value. If another clear call is made with
    // the exact same clear value, we will detect and skip the clear call.
    Optional<ClearUpdate> mCurrentSingleClearValue;

    // Track whether each subresource has defined contents.  Up to 8 layers are tracked per level,
    // above which the contents are considered unconditionally defined.
    gl::TexLevelArray<LevelContentDefinedMask> mContentDefined;
    gl::TexLevelArray<LevelContentDefinedMask> mStencilContentDefined;
};

// A vector of image views, such as one per level or one per layer.
using ImageViewVector = std::vector<ImageView>;

// A vector of vector of image views.  Primary index is layer, secondary index is level.
using LayerLevelImageViewVector = std::vector<ImageViewVector>;

// Address mode for layers: only possible to access either all layers, or up to
// IMPLEMENTATION_ANGLE_MULTIVIEW_MAX_VIEWS layers.  This enum uses 0 for all layers and the rest of
// the values conveniently alias the number of layers.
enum LayerMode
{
    All,
    _1,
    _2,
    _3,
    _4,
};
static_assert(gl::IMPLEMENTATION_ANGLE_MULTIVIEW_MAX_VIEWS == 4, "Update LayerMode");

LayerMode GetLayerMode(const vk::ImageHelper &image, uint32_t layerCount);

// Sampler decode mode indicating if an attachment needs to be decoded in linear colorspace or sRGB
enum class SrgbDecodeMode
{
    SkipDecode,
    SrgbDecode
};

class ImageViewHelper final : public Resource
{
  public:
    ImageViewHelper();
    ImageViewHelper(ImageViewHelper &&other);
    ~ImageViewHelper() override;

    void init(RendererVk *renderer);
    void release(RendererVk *renderer);
    void destroy(VkDevice device);

    const ImageView &getLinearReadImageView() const
    {
        return getValidReadViewImpl(mPerLevelLinearReadImageViews);
    }
    const ImageView &getSRGBReadImageView() const
    {
        return getValidReadViewImpl(mPerLevelSRGBReadImageViews);
    }
    const ImageView &getLinearFetchImageView() const
    {
        return getValidReadViewImpl(mPerLevelLinearFetchImageViews);
    }
    const ImageView &getSRGBFetchImageView() const
    {
        return getValidReadViewImpl(mPerLevelSRGBFetchImageViews);
    }
    const ImageView &getLinearCopyImageView() const
    {
        return getValidReadViewImpl(mPerLevelLinearCopyImageViews);
    }
    const ImageView &getSRGBCopyImageView() const
    {
        return getValidReadViewImpl(mPerLevelSRGBCopyImageViews);
    }
    const ImageView &getStencilReadImageView() const
    {
        return getValidReadViewImpl(mPerLevelStencilReadImageViews);
    }

    const ImageView &getReadImageView() const
    {
        return mLinearColorspace ? getReadViewImpl(mPerLevelLinearReadImageViews)
                                 : getReadViewImpl(mPerLevelSRGBReadImageViews);
    }

    const ImageView &getFetchImageView() const
    {
        return mLinearColorspace ? getReadViewImpl(mPerLevelLinearFetchImageViews)
                                 : getReadViewImpl(mPerLevelSRGBFetchImageViews);
    }

    const ImageView &getCopyImageView() const
    {
        return mLinearColorspace ? getReadViewImpl(mPerLevelLinearCopyImageViews)
                                 : getReadViewImpl(mPerLevelSRGBCopyImageViews);
    }

    // Used when initialized RenderTargets.
    bool hasStencilReadImageView() const
    {
        return mCurrentMaxLevel.get() < mPerLevelStencilReadImageViews.size()
                   ? mPerLevelStencilReadImageViews[mCurrentMaxLevel.get()].valid()
                   : false;
    }

    bool hasFetchImageView() const
    {
        if ((mLinearColorspace && mCurrentMaxLevel.get() < mPerLevelLinearFetchImageViews.size()) ||
            (!mLinearColorspace && mCurrentMaxLevel.get() < mPerLevelSRGBFetchImageViews.size()))
        {
            return getFetchImageView().valid();
        }
        else
        {
            return false;
        }
    }

    bool hasCopyImageView() const
    {
        if ((mLinearColorspace && mCurrentMaxLevel.get() < mPerLevelLinearCopyImageViews.size()) ||
            (!mLinearColorspace && mCurrentMaxLevel.get() < mPerLevelSRGBCopyImageViews.size()))
        {
            return getCopyImageView().valid();
        }
        else
        {
            return false;
        }
    }

    // For applications that frequently switch a texture's max level, and make no other changes to
    // the texture, change the currently-used max level, and potentially create new "read views"
    // for the new max-level
    angle::Result initReadViews(ContextVk *contextVk,
                                gl::TextureType viewType,
                                const ImageHelper &image,
                                const angle::Format &format,
                                const gl::SwizzleState &formatSwizzle,
                                const gl::SwizzleState &readSwizzle,
                                LevelIndex baseLevel,
                                uint32_t levelCount,
                                uint32_t baseLayer,
                                uint32_t layerCount,
                                bool requiresSRGBViews,
                                VkImageUsageFlags imageUsageFlags);

    // Creates a storage view with all layers of the level.
    angle::Result getLevelStorageImageView(ContextVk *contextVk,
                                           gl::TextureType viewType,
                                           const ImageHelper &image,
                                           LevelIndex levelVk,
                                           uint32_t layer,
                                           VkImageUsageFlags imageUsageFlags,
                                           angle::FormatID formatID,
                                           const ImageView **imageViewOut);

    // Creates a storage view with a single layer of the level.
    angle::Result getLevelLayerStorageImageView(ContextVk *contextVk,
                                                const ImageHelper &image,
                                                LevelIndex levelVk,
                                                uint32_t layer,
                                                VkImageUsageFlags imageUsageFlags,
                                                angle::FormatID formatID,
                                                const ImageView **imageViewOut);

    // Creates a draw view with a range of layers of the level.
    angle::Result getLevelDrawImageView(ContextVk *contextVk,
                                        const ImageHelper &image,
                                        LevelIndex levelVk,
                                        uint32_t layer,
                                        uint32_t layerCount,
                                        gl::SrgbWriteControlMode mode,
                                        const ImageView **imageViewOut);

    // Creates a draw view with a single layer of the level.
    angle::Result getLevelLayerDrawImageView(ContextVk *contextVk,
                                             const ImageHelper &image,
                                             LevelIndex levelVk,
                                             uint32_t layer,
                                             gl::SrgbWriteControlMode mode,
                                             const ImageView **imageViewOut);

    // Return unique Serial for an imageView.
    ImageOrBufferViewSubresourceSerial getSubresourceSerial(
        gl::LevelIndex levelGL,
        uint32_t levelCount,
        uint32_t layer,
        LayerMode layerMode,
        SrgbDecodeMode srgbDecodeMode,
        gl::SrgbOverride srgbOverrideMode) const;

  private:
    ImageView &getReadImageView()
    {
        return mLinearColorspace ? getReadViewImpl(mPerLevelLinearReadImageViews)
                                 : getReadViewImpl(mPerLevelSRGBReadImageViews);
    }
    ImageView &getFetchImageView()
    {
        return mLinearColorspace ? getReadViewImpl(mPerLevelLinearFetchImageViews)
                                 : getReadViewImpl(mPerLevelSRGBFetchImageViews);
    }
    ImageView &getCopyImageView()
    {
        return mLinearColorspace ? getReadViewImpl(mPerLevelLinearCopyImageViews)
                                 : getReadViewImpl(mPerLevelSRGBCopyImageViews);
    }

    // Used by public get*ImageView() methods to do proper assert based on vector size and validity
    inline const ImageView &getValidReadViewImpl(const ImageViewVector &imageViewVector) const
    {
        ASSERT(mCurrentMaxLevel.get() < imageViewVector.size() &&
               imageViewVector[mCurrentMaxLevel.get()].valid());
        return imageViewVector[mCurrentMaxLevel.get()];
    }

    // Used by public get*ImageView() methods to do proper assert based on vector size
    inline const ImageView &getReadViewImpl(const ImageViewVector &imageViewVector) const
    {
        ASSERT(mCurrentMaxLevel.get() < imageViewVector.size());
        return imageViewVector[mCurrentMaxLevel.get()];
    }

    // Used by private get*ImageView() methods to do proper assert based on vector size
    inline ImageView &getReadViewImpl(ImageViewVector &imageViewVector)
    {
        ASSERT(mCurrentMaxLevel.get() < imageViewVector.size());
        return imageViewVector[mCurrentMaxLevel.get()];
    }

    // Creates views with multiple layers and levels.
    angle::Result initReadViewsImpl(ContextVk *contextVk,
                                    gl::TextureType viewType,
                                    const ImageHelper &image,
                                    const angle::Format &format,
                                    const gl::SwizzleState &formatSwizzle,
                                    const gl::SwizzleState &readSwizzle,
                                    LevelIndex baseLevel,
                                    uint32_t levelCount,
                                    uint32_t baseLayer,
                                    uint32_t layerCount);

    // Create SRGB-reinterpreted read views
    angle::Result initSRGBReadViewsImpl(ContextVk *contextVk,
                                        gl::TextureType viewType,
                                        const ImageHelper &image,
                                        const angle::Format &format,
                                        const gl::SwizzleState &formatSwizzle,
                                        const gl::SwizzleState &readSwizzle,
                                        LevelIndex baseLevel,
                                        uint32_t levelCount,
                                        uint32_t baseLayer,
                                        uint32_t layerCount,
                                        VkImageUsageFlags imageUsageFlags);

    // For applications that frequently switch a texture's max level, and make no other changes to
    // the texture, keep track of the currently-used max level, and keep one "read view" per
    // max-level
    LevelIndex mCurrentMaxLevel;

    // Read views (one per max-level)
    ImageViewVector mPerLevelLinearReadImageViews;
    ImageViewVector mPerLevelSRGBReadImageViews;
    ImageViewVector mPerLevelLinearFetchImageViews;
    ImageViewVector mPerLevelSRGBFetchImageViews;
    ImageViewVector mPerLevelLinearCopyImageViews;
    ImageViewVector mPerLevelSRGBCopyImageViews;
    ImageViewVector mPerLevelStencilReadImageViews;

    bool mLinearColorspace;

    // Draw views
    LayerLevelImageViewVector mLayerLevelDrawImageViews;
    LayerLevelImageViewVector mLayerLevelDrawImageViewsLinear;
    angle::HashMap<ImageSubresourceRange, std::unique_ptr<ImageView>> mSubresourceDrawImageViews;

    // Storage views
    ImageViewVector mLevelStorageImageViews;
    LayerLevelImageViewVector mLayerLevelStorageImageViews;

    // Serial for the image view set. getSubresourceSerial combines it with subresource info.
    ImageOrBufferViewSerial mImageViewSerial;
};

ImageSubresourceRange MakeImageSubresourceReadRange(gl::LevelIndex level,
                                                    uint32_t levelCount,
                                                    uint32_t layer,
                                                    LayerMode layerMode,
                                                    SrgbDecodeMode srgbDecodeMode,
                                                    gl::SrgbOverride srgbOverrideMode);
ImageSubresourceRange MakeImageSubresourceDrawRange(gl::LevelIndex level,
                                                    uint32_t layer,
                                                    LayerMode layerMode,
                                                    gl::SrgbWriteControlMode srgbWriteControlMode);

class BufferViewHelper final : public Resource
{
  public:
    BufferViewHelper();
    BufferViewHelper(BufferViewHelper &&other);
    ~BufferViewHelper() override;

    void init(RendererVk *renderer, VkDeviceSize offset, VkDeviceSize size);
    void release(RendererVk *renderer);
    void destroy(VkDevice device);

    angle::Result getView(ContextVk *contextVk,
                          const BufferHelper &buffer,
                          VkDeviceSize bufferOffset,
                          const Format &format,
                          const BufferView **viewOut);

    // Return unique Serial for a bufferView.
    ImageOrBufferViewSubresourceSerial getSerial() const;

  private:
    // To support format reinterpretation, additional views for formats other than the one specified
    // to glTexBuffer may need to be created.  On draw/dispatch, the format layout qualifier of the
    // imageBuffer is used (if provided) to create a potentially different view of the buffer.
    angle::HashMap<VkFormat, BufferView> mViews;

    // View properties:
    //
    // Offset and size specified to glTexBufferRange
    VkDeviceSize mOffset;
    VkDeviceSize mSize;

    // Serial for the buffer view.  An ImageOrBufferViewSerial is used for texture buffers so that
    // they fit together with the other texture types.
    ImageOrBufferViewSerial mViewSerial;
};

class FramebufferHelper : public Resource
{
  public:
    FramebufferHelper();
    ~FramebufferHelper() override;

    FramebufferHelper(FramebufferHelper &&other);
    FramebufferHelper &operator=(FramebufferHelper &&other);

    angle::Result init(ContextVk *contextVk, const VkFramebufferCreateInfo &createInfo);
    void release(ContextVk *contextVk);

    bool valid() { return mFramebuffer.valid(); }

    const Framebuffer &getFramebuffer() const
    {
        ASSERT(mFramebuffer.valid());
        return mFramebuffer;
    }

    Framebuffer &getFramebuffer()
    {
        ASSERT(mFramebuffer.valid());
        return mFramebuffer;
    }

  private:
    // Vulkan object.
    Framebuffer mFramebuffer;
};

class ShaderProgramHelper : angle::NonCopyable
{
  public:
    ShaderProgramHelper();
    ~ShaderProgramHelper();

    bool valid(const gl::ShaderType shaderType) const;
    void destroy(RendererVk *rendererVk);
    void release(ContextVk *contextVk);

    ShaderAndSerial &getShader(gl::ShaderType shaderType) { return mShaders[shaderType].get(); }

    void setShader(gl::ShaderType shaderType, RefCounted<ShaderAndSerial> *shader);
    void setSpecializationConstant(sh::vk::SpecializationConstantId id, uint32_t value);

    // For getting a Pipeline and from the pipeline cache.
    ANGLE_INLINE angle::Result getGraphicsPipeline(
        ContextVk *contextVk,
        RenderPassCache *renderPassCache,
        const PipelineCache &pipelineCache,
        const PipelineLayout &pipelineLayout,
        const GraphicsPipelineDesc &pipelineDesc,
        const gl::AttributesMask &activeAttribLocationsMask,
        const gl::ComponentTypeMask &programAttribsTypeMask,
        const GraphicsPipelineDesc **descPtrOut,
        PipelineHelper **pipelineOut)
    {
        // Pull in a compatible RenderPass.
        RenderPass *compatibleRenderPass = nullptr;
        ANGLE_TRY(renderPassCache->getCompatibleRenderPass(
            contextVk, pipelineDesc.getRenderPassDesc(), &compatibleRenderPass));

        ShaderModule *vertexShader   = &mShaders[gl::ShaderType::Vertex].get().get();
        ShaderModule *fragmentShader = mShaders[gl::ShaderType::Fragment].valid()
                                           ? &mShaders[gl::ShaderType::Fragment].get().get()
                                           : nullptr;
        ShaderModule *geometryShader = mShaders[gl::ShaderType::Geometry].valid()
                                           ? &mShaders[gl::ShaderType::Geometry].get().get()
                                           : nullptr;
        ShaderModule *tessControlShader = mShaders[gl::ShaderType::TessControl].valid()
                                              ? &mShaders[gl::ShaderType::TessControl].get().get()
                                              : nullptr;
        ShaderModule *tessEvaluationShader =
            mShaders[gl::ShaderType::TessEvaluation].valid()
                ? &mShaders[gl::ShaderType::TessEvaluation].get().get()
                : nullptr;

        return mGraphicsPipelines.getPipeline(
            contextVk, pipelineCache, *compatibleRenderPass, pipelineLayout,
            activeAttribLocationsMask, programAttribsTypeMask, vertexShader, fragmentShader,
            geometryShader, tessControlShader, tessEvaluationShader, mSpecializationConstants,
            pipelineDesc, descPtrOut, pipelineOut);
    }

    angle::Result getComputePipeline(Context *context,
                                     const PipelineLayout &pipelineLayout,
                                     PipelineAndSerial **pipelineOut);

  private:
    gl::ShaderMap<BindingPointer<ShaderAndSerial>> mShaders;
    GraphicsPipelineCache mGraphicsPipelines;

    // We should probably use PipelineHelper here so we can remove PipelineAndSerial.
    PipelineAndSerial mComputePipeline;

    // Specialization constants, currently only used by the graphics queue.
    SpecializationConstants mSpecializationConstants;
};

// Tracks current handle allocation counts in the back-end. Useful for debugging and profiling.
// Note: not all handle types are currently implemented.
class ActiveHandleCounter final : angle::NonCopyable
{
  public:
    ActiveHandleCounter();
    ~ActiveHandleCounter();

    void onAllocate(HandleType handleType)
    {
        mActiveCounts[handleType]++;
        mAllocatedCounts[handleType]++;
    }

    void onDeallocate(HandleType handleType) { mActiveCounts[handleType]--; }

    uint32_t getActive(HandleType handleType) const { return mActiveCounts[handleType]; }
    uint32_t getAllocated(HandleType handleType) const { return mAllocatedCounts[handleType]; }

  private:
    angle::PackedEnumMap<HandleType, uint32_t> mActiveCounts;
    angle::PackedEnumMap<HandleType, uint32_t> mAllocatedCounts;
};

ANGLE_INLINE bool CommandBufferHelper::usesImageInRenderPass(const ImageHelper &image) const
{
    ASSERT(mIsRenderPassCommandBuffer);
    return mRenderPassUsedImages.contains(image.getImageSerial().getValue());
}

// Sometimes ANGLE issues a command internally, such as copies, draws and dispatches that do not
// directly correspond to the application draw/dispatch call.  Before the command is recorded in the
// command buffer, the render pass may need to be broken and/or appropriate barriers may need to be
// inserted.  The following struct aggregates all resources that such internal commands need.
struct CommandBufferBufferAccess
{
    BufferHelper *buffer;
    VkAccessFlags accessType;
    PipelineStage stage;
};
struct CommandBufferImageAccess
{
    ImageHelper *image;
    VkImageAspectFlags aspectFlags;
    ImageLayout imageLayout;
};
struct CommandBufferImageWrite
{
    CommandBufferImageAccess access;
    gl::LevelIndex levelStart;
    uint32_t levelCount;
    uint32_t layerStart;
    uint32_t layerCount;
};
class CommandBufferAccess : angle::NonCopyable
{
  public:
    CommandBufferAccess();
    ~CommandBufferAccess();

    void onBufferTransferRead(BufferHelper *buffer)
    {
        onBufferRead(VK_ACCESS_TRANSFER_READ_BIT, PipelineStage::Transfer, buffer);
    }
    void onBufferTransferWrite(BufferHelper *buffer)
    {
        onBufferWrite(VK_ACCESS_TRANSFER_WRITE_BIT, PipelineStage::Transfer, buffer);
    }
    void onBufferSelfCopy(BufferHelper *buffer)
    {
        onBufferWrite(VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
                      PipelineStage::Transfer, buffer);
    }
    void onBufferComputeShaderRead(BufferHelper *buffer)
    {
        onBufferRead(VK_ACCESS_SHADER_READ_BIT, PipelineStage::ComputeShader, buffer);
    }
    void onBufferComputeShaderWrite(BufferHelper *buffer)
    {
        onBufferWrite(VK_ACCESS_SHADER_WRITE_BIT, PipelineStage::ComputeShader, buffer);
    }

    void onImageTransferRead(VkImageAspectFlags aspectFlags, ImageHelper *image)
    {
        onImageRead(aspectFlags, ImageLayout::TransferSrc, image);
    }
    void onImageTransferWrite(gl::LevelIndex levelStart,
                              uint32_t levelCount,
                              uint32_t layerStart,
                              uint32_t layerCount,
                              VkImageAspectFlags aspectFlags,
                              ImageHelper *image)
    {
        onImageWrite(levelStart, levelCount, layerStart, layerCount, aspectFlags,
                     ImageLayout::TransferDst, image);
    }
    void onImageComputeShaderRead(VkImageAspectFlags aspectFlags, ImageHelper *image)
    {
        onImageRead(aspectFlags, ImageLayout::ComputeShaderReadOnly, image);
    }
    void onImageComputeShaderWrite(gl::LevelIndex levelStart,
                                   uint32_t levelCount,
                                   uint32_t layerStart,
                                   uint32_t layerCount,
                                   VkImageAspectFlags aspectFlags,
                                   ImageHelper *image)
    {
        onImageWrite(levelStart, levelCount, layerStart, layerCount, aspectFlags,
                     ImageLayout::ComputeShaderWrite, image);
    }

    // The limits reflect the current maximum concurrent usage of each resource type.  ASSERTs will
    // fire if this limit is exceeded in the future.
    using ReadBuffers  = angle::FixedVector<CommandBufferBufferAccess, 2>;
    using WriteBuffers = angle::FixedVector<CommandBufferBufferAccess, 2>;
    using ReadImages   = angle::FixedVector<CommandBufferImageAccess, 2>;
    using WriteImages  = angle::FixedVector<CommandBufferImageWrite, 1>;

    const ReadBuffers &getReadBuffers() const { return mReadBuffers; }
    const WriteBuffers &getWriteBuffers() const { return mWriteBuffers; }
    const ReadImages &getReadImages() const { return mReadImages; }
    const WriteImages &getWriteImages() const { return mWriteImages; }

  private:
    void onBufferRead(VkAccessFlags readAccessType, PipelineStage readStage, BufferHelper *buffer);
    void onBufferWrite(VkAccessFlags writeAccessType,
                       PipelineStage writeStage,
                       BufferHelper *buffer);

    void onImageRead(VkImageAspectFlags aspectFlags, ImageLayout imageLayout, ImageHelper *image);
    void onImageWrite(gl::LevelIndex levelStart,
                      uint32_t levelCount,
                      uint32_t layerStart,
                      uint32_t layerCount,
                      VkImageAspectFlags aspectFlags,
                      ImageLayout imageLayout,
                      ImageHelper *image);

    ReadBuffers mReadBuffers;
    WriteBuffers mWriteBuffers;
    ReadImages mReadImages;
    WriteImages mWriteImages;
};
}  // namespace vk
}  // namespace rx

#endif  // LIBANGLE_RENDERER_VULKAN_VK_HELPERS_H_
