#include "ECOCppLogger/ThreadBuffer.h"

#include <algorithm>
#include <bit>
#include <new>

namespace eco::log {

namespace {

std::atomic<std::size_t> g_threadBufferBytes{std::size_t{1} << 20};

// スレッドが終わるときにバッファを「終わった」にする（バッファ自体は裏のスレッドが読み切ってから片付ける）。
struct Retirer {
    ThreadBuffer* buffer = nullptr;
    ~Retirer() {
        if (buffer) {
            buffer->retired.store(true, std::memory_order_release);
        }
        detail::t_buffer = nullptr;
    }
};

thread_local Retirer t_retirer;

}  // namespace

ThreadBuffer::ThreadBuffer(std::size_t capacityBytes, std::uint32_t index)
    : capacity_(std::bit_ceil(std::max<std::size_t>(capacityBytes, 4096))), index_(index) {
    mask_ = capacity_ - 1;
    data_ = static_cast<std::byte*>(::operator new(capacity_, std::align_val_t{64}));
}

ThreadBuffer::~ThreadBuffer() {
    ::operator delete(data_, std::align_val_t{64});
    for (auto& entry : chunks_) {
        delete entry.load(std::memory_order_relaxed);
    }
}

StatChunk* ThreadBuffer::allocateChunk(std::size_t chunkIndex) {
    auto* chunk = new StatChunk();
    chunks_[chunkIndex].store(chunk, std::memory_order_release);
    return chunk;
}

Registry& Registry::instance() {
    // わざと消さない：静的な破棄の後・クラッシュ中にも使われるため。
    static Registry* registry = new Registry();
    return *registry;
}

ThreadBuffer* Registry::create(std::size_t capacityBytes) {
    std::lock_guard lock(mutex);
    auto buffer = std::make_unique<ThreadBuffer>(capacityBytes, nextThreadIndex_++);
    buffer->name = "T" + std::to_string(buffer->index());
    ThreadBuffer* raw = buffer.get();
    buffers_.push_back(std::move(buffer));
    return raw;
}

std::vector<Registry::Entry> Registry::snapshot(bool emergency) {
    std::unique_lock lock(mutex, std::defer_lock);
    if (emergency) {
        (void)lock.try_lock();
    } else {
        lock.lock();
    }
    std::vector<Entry> result;
    result.reserve(buffers_.size());
    for (auto& buffer : buffers_) {
        result.push_back({buffer.get(), buffer->name});
    }
    return result;
}

void Registry::setName(ThreadBuffer* buffer, std::string_view name) {
    std::lock_guard lock(mutex);
    buffer->name = name;
}

void Registry::removeRetired(const std::vector<ThreadBuffer*>& done) {
    if (done.empty()) {
        return;
    }
    std::lock_guard lock(mutex);
    std::erase_if(buffers_, [&](const std::unique_ptr<ThreadBuffer>& buffer) {
        return std::find(done.begin(), done.end(), buffer.get()) != done.end();
    });
}

std::uint32_t Registry::registerStat(StatSite& site) noexcept {
    // 2つのスレッドが同時に初めて使っても、1つの番号に決まるようにする。
    std::uint32_t expected = 0;
    const std::uint32_t candidate = nextStatIndex_.fetch_add(1, std::memory_order_relaxed);
    if (site.index.compare_exchange_strong(expected, candidate, std::memory_order_acq_rel)) {
        return candidate;
    }
    return expected;
}

namespace detail {

void setThreadBufferBytes(std::size_t bytes) noexcept {
    g_threadBufferBytes.store(bytes, std::memory_order_relaxed);
}

ThreadBuffer* createThreadBuffer() {
    ThreadBuffer* buffer = Registry::instance().create(g_threadBufferBytes.load(std::memory_order_relaxed));
    t_buffer = buffer;
    t_retirer.buffer = buffer;
    return buffer;
}

}  // namespace detail

}  // namespace eco::log
