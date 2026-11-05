#include "ECOCppLogger/ThreadBuffer.h"

#include <algorithm>
#include <bit>
#include <new>

#include "Registry.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <pthread.h>
#endif

namespace lg {

ThreadBuffer::ThreadBuffer(size_t capacityBytes, uint32_t reservePercent, uint32_t index, uint64_t osThreadId)
    : capacity_(std::bit_ceil(std::max<size_t>(capacityBytes, 4096))), index_(index), osThreadId_(osThreadId) {
    mask_ = capacity_ - 1;
    normalLimit_ = capacity_ - capacity_ * std::min<uint32_t>(reservePercent, 90) / 100;
    data_ = static_cast<unsigned char*>(::operator new(capacity_, std::align_val_t{64}));
}

ThreadBuffer::~ThreadBuffer() {
    ::operator delete(data_, std::align_val_t{64});
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

void Registry::setBufferSettings(size_t bytes, uint32_t reservePercent) {
    std::lock_guard lock(mutex_);
    bufferBytes_ = bytes;
    reservePercent_ = reservePercent;
}

ThreadBuffer* Registry::create(uint64_t osThreadId) {
    std::lock_guard lock(mutex_);
    const uint32_t index = nextThreadIndex_++;
    buffers_.push_back(std::make_unique<ThreadBuffer>(bufferBytes_, reservePercent_, index, osThreadId));
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
