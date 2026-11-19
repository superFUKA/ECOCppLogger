#include "ECOCppLogger/ThreadBuffer.h"

#include <algorithm>
#include <bit>
#include <cstdlib>
#include <new>

#include "Crash.h"
#include "Registry.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <malloc.h>
#include <windows.h>
#else
#include <pthread.h>
#endif

namespace lg {

namespace {

constexpr size_t kSegmentHeaderBytes = 64;  // 中身を 64 バイトにそろえるため、頭の分をこの大きさで取る

// ECOSTL 等の確保（とそのログ）を通らないように、malloc で直接取る。
void* allocateAligned(size_t bytes) noexcept {
#if defined(_WIN32)
    return _aligned_malloc(bytes, 64);
#else
    return std::aligned_alloc(64, bytes);
#endif
}

void freeAligned(void* memory) noexcept {
#if defined(_WIN32)
    _aligned_free(memory);
#else
    std::free(memory);
#endif
}

}  // namespace

ThreadBuffer::Segment* ThreadBuffer::createSegment(size_t capacity) noexcept {
    static_assert(sizeof(Segment) <= kSegmentHeaderBytes);
    void* memory = allocateAligned(kSegmentHeaderBytes + capacity);
    if (!memory) {
        return nullptr;
    }
    auto* segment = new (memory) Segment();
    segment->data = static_cast<unsigned char*>(memory) + kSegmentHeaderBytes;
    segment->capacity = capacity;
    segment->mask = capacity - 1;
    segment->end = 0;
    return segment;
}

void ThreadBuffer::freeSegment(Segment* segment) noexcept {
    segment->~Segment();
    freeAligned(segment);
}

ThreadBuffer::ThreadBuffer(size_t capacityBytes, uint32_t reservePercent, uint32_t index, uint64_t osThreadId,
                           size_t maxCapacityBytes)
    : reservePercent_(std::min<uint32_t>(reservePercent, 90)), index_(index), osThreadId_(osThreadId) {
    const size_t capacity = std::bit_ceil(std::max<size_t>(capacityBytes, 4096));
    maxCapacity_ = std::max(capacity, maxCapacityBytes > capacity ? std::bit_floor(maxCapacityBytes) : capacity);
    Segment* segment = createSegment(capacity);
    if (!segment) {
        throw std::bad_alloc();  // 作る所（台帳）は make_unique なので、今までの operator new と同じ扱い
    }
    readSegment_ = segment;
    useSegment(segment);
    reportedCapacity = capacity;
}

void ThreadBuffer::useSegment(Segment* segment) noexcept {
    writeSegment_ = segment;
    data_ = segment->data;
    capacity_ = segment->capacity;
    mask_ = segment->mask;
    normalLimit_ = capacity_ - capacity_ * reservePercent_ / 100;
    segmentBase_ = localWrite_;
    cachedRead_ = localWrite_;  // 新しいリングは空
    publishedCapacity_.store(capacity_, std::memory_order_relaxed);
}

#if defined(_MSC_VER)
__declspec(noinline)
#else
__attribute__((noinline))
#endif
unsigned char* ThreadBuffer::reserveSlow(size_t size, bool important) noexcept {
    for (int attempt = 0; attempt < 2; ++attempt) {
        const uint64_t write = localWrite_;
        const size_t offset = static_cast<size_t>(write & mask_);
        const size_t contiguous = capacity_ - offset;
        const size_t needed = size <= contiguous ? size : contiguous + size;
        const uint64_t limit = important ? capacity_ : normalLimit_;
        if (size <= capacity_ / 2) {
            // 書き出す側が古いリングを読んでいる間は、その位置は今のリングの始まりより前（今のリングは使っていない）。
            cachedRead_ = std::max(read_.load(std::memory_order_acquire), segmentBase_);
            if (write + needed - cachedRead_ <= limit) {
                return place(write, offset, contiguous, size);
            }
        }
        if (attempt == 0 && !grow(size, important)) {
            break;
        }
    }
    return nullptr;
}

bool ThreadBuffer::grow(size_t size, bool important) noexcept {
    // 空のリングに必ず入る大きさ：折り返しの詰め物を含めて size の 2 倍。軽い記録は取っておく分を除いて。
    const size_t usable = important ? 2 * size : (2 * size * 100 + (100 - reservePercent_) - 1) / (100 - reservePercent_);
    const size_t capacity = std::max(capacity_ * 2, std::bit_ceil(usable));
    if (capacity > maxCapacity_) {
        return false;
    }
    Segment* segment = createSegment(capacity);
    if (!segment) {
        return false;
    }
    // 古いリングの終わりを書いてから next を出す（書き出す側は next を見たら end まで読んで移る）。
    writeSegment_->end = localWrite_;
    writeSegment_->next.store(segment, std::memory_order_release);
    useSegment(segment);
    return true;
}

ThreadBuffer::~ThreadBuffer() {
    for (Segment* segment = readSegment_; segment;) {
        Segment* next = segment->next.load(std::memory_order_relaxed);
        freeSegment(segment);
        segment = next;
    }
    for (SiteState* chunk : stateChunks_) {
        delete[] chunk;
    }
    for (auto& chunk : statChunks_) {
        delete chunk.load(std::memory_order_relaxed);
    }
}

StatChunk* ThreadBuffer::createStatChunk(uint32_t chunkIndex) noexcept {
    auto* chunk = new (std::nothrow) StatChunk();
    statChunks_[chunkIndex].store(chunk, std::memory_order_release);
    return chunk;
}

ThreadBuffer::SiteState* ThreadBuffer::createStateChunk(uint32_t site) noexcept {
    SiteState*& chunk = stateChunks_[(site / kStateChunkSize) % kStateChunks];
    chunk = new (std::nothrow) SiteState[kStateChunkSize]();
    if (!chunk) {
        static thread_local SiteState fallback[kStateChunkSize];  // 取れなければ使い捨て（間引きが不正確になるだけ）
        return fallback;
    }
    return chunk;
}

namespace detail {

#if ECO_LOG_TLS_SLOT
namespace {
DWORD g_tlsIndex = TLS_OUT_OF_INDEXES;
}

// TEB の TlsSlots は gs:[0x1480] から 64 個（それより後ろの番号は別の場所なので、使えなければ遅い道）。
uint32_t g_tlsSlotOffset = [] {
    g_tlsIndex = TlsAlloc();
    return g_tlsIndex < 64 ? static_cast<uint32_t>(0x1480 + g_tlsIndex * 8) : 0u;
}();
#endif

namespace {

uint64_t currentOsThreadId() {
#if defined(_WIN32)
    return GetCurrentThreadId();
#else
    return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(pthread_self()));
#endif
}

// スレッドが終わるときにバッファを「終わった」にする（バッファ自体は書き出す側が読み切ってから片付ける）。
struct ThreadHolder {
    ThreadBuffer* buffer = nullptr;
    ~ThreadHolder() {
        if (buffer) {
            buffer->retired.store(true, std::memory_order_release);
        }
#if ECO_LOG_TLS_SLOT
        if (g_tlsIndex != TLS_OUT_OF_INDEXES) {
            TlsSetValue(g_tlsIndex, nullptr);
        }
#endif
        buffer = nullptr;
    }
};

thread_local ThreadHolder t_holder;

}  // namespace

#if defined(_MSC_VER)
__declspec(noinline)
#else
__attribute__((noinline))
#endif
ThreadBuffer* threadBufferSlow() noexcept {
    ThreadHolder& holder = t_holder;  // 非インラインなので、毎回今のスレッドのものを読む
    if (!holder.buffer) {
        holder.buffer = Registry::instance().create(currentOsThreadId());
        installThreadCrashHandler();  // std::set_terminate はスレッドごと（MSVC）
#if ECO_LOG_TLS_SLOT
        if (g_tlsIndex != TLS_OUT_OF_INDEXES) {
            TlsSetValue(g_tlsIndex, holder.buffer);
        }
#endif
    }
    return holder.buffer;
}

// ---- 台帳 ---------------------------------------------------------------------------------------------

Registry& Registry::instance() {
    // わざと消さない：静的な破棄の後・落ちている最中にも使われるため。
    static Registry* registry = new Registry();
    return *registry;
}

void Registry::setBufferSettings(size_t bytes, size_t maxBytes, uint32_t reservePercent) {
    std::lock_guard lock(mutex_);
    bufferBytes_ = bytes;
    maxBufferBytes_ = maxBytes;
    reservePercent_ = reservePercent;
}

ThreadBuffer* Registry::create(uint64_t osThreadId) {
    std::lock_guard lock(mutex_);
    const uint32_t index = nextThreadIndex_++;
    buffers_.push_back(std::make_unique<ThreadBuffer>(bufferBytes_, reservePercent_, index, osThreadId, maxBufferBytes_));
    names_.push_back("T" + std::to_string(index));
    return buffers_.back().get();
}

std::vector<Registry::Entry> Registry::snapshot(bool emergency) {
    std::unique_lock lock(mutex_, std::defer_lock);
    if (emergency) {
        (void)lock.try_lock();
    } else {
        lock.lock();
    }
    std::vector<Entry> result;
    result.reserve(buffers_.size());
    for (size_t i = 0; i < buffers_.size(); ++i) {
        result.push_back({buffers_[i].get(), names_[i]});
    }
    return result;
}

void Registry::setName(ThreadBuffer* buffer, std::string_view name) {
    std::lock_guard lock(mutex_);
    for (size_t i = 0; i < buffers_.size(); ++i) {
        if (buffers_[i].get() == buffer) {
            names_[i] = name;
        }
    }
}

std::string Registry::name(const ThreadBuffer* buffer) {
    std::lock_guard lock(mutex_);
    for (size_t i = 0; i < buffers_.size(); ++i) {
        if (buffers_[i].get() == buffer) return names_[i];
    }
    return {};
}

void Registry::removeRetired(const std::vector<ThreadBuffer*>& done) {
    if (done.empty()) {
        return;
    }
    std::lock_guard lock(mutex_);
    for (size_t i = buffers_.size(); i-- > 0;) {
        if (std::find(done.begin(), done.end(), buffers_[i].get()) != done.end()) {
            buffers_.erase(buffers_.begin() + static_cast<std::ptrdiff_t>(i));
            names_.erase(names_.begin() + static_cast<std::ptrdiff_t>(i));
        }
    }
}

uint32_t Registry::registerSite(const Site& site, SiteIdSlot& slot) noexcept {
    std::lock_guard lock(siteMutex_);
    uint32_t id = slot.load(std::memory_order_acquire);
    if (id != 0) {
        return id;  // ほかのスレッドが先に振った
    }
    id = nextSiteId_;
    const size_t chunkIndex = id / kSiteChunkSize;
    if (chunkIndex >= kSiteChunks) {
        return 0;  // 多すぎる：このログは書かない
    }
    std::atomic<const Site*>* chunk = siteChunks_[chunkIndex].load(std::memory_order_relaxed);
    if (!chunk) {
        chunk = new std::atomic<const Site*>[kSiteChunkSize]();
        siteChunks_[chunkIndex].store(chunk, std::memory_order_release);
    }
    chunk[id % kSiteChunkSize].store(&site, std::memory_order_release);
    ++nextSiteId_;
    slot.store(id, std::memory_order_release);
    return id;
}

uint32_t Registry::registerStat(StatSite& site) noexcept {
    std::lock_guard lock(siteMutex_);
    uint32_t index = site.index.load(std::memory_order_relaxed);
    if (index == 0 && nextStatIndex_ < kStatChunks * kStatChunkSlots) {
        index = nextStatIndex_++;
        site.index.store(index, std::memory_order_relaxed);
    }
    return index;
}

const Site* Registry::site(uint32_t id) const noexcept {
    const size_t chunkIndex = id / kSiteChunkSize;
    if (chunkIndex >= kSiteChunks) {
        return nullptr;
    }
    const std::atomic<const Site*>* chunk = siteChunks_[chunkIndex].load(std::memory_order_acquire);
    return chunk ? chunk[id % kSiteChunkSize].load(std::memory_order_acquire) : nullptr;
}

uint32_t registerSite(const Site& site, SiteIdSlot& slot) noexcept {
    return Registry::instance().registerSite(site, slot);
}

}  // namespace detail

}  // namespace lg
