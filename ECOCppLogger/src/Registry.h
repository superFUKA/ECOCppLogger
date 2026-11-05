#pragma once

// 中の台帳：すべてのスレッドのバッファと、ログの呼び出し位置の番号。書き出す側・落ちたときの書き出しが使う。
// プロセスの終わりまで消さない（終了処理・落ちている最中にも使うため）。

#include <stddef.h>
#include <stdint.h>

#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "ECOCppLogger/ThreadBuffer.h"

namespace lg::detail {

class Registry {
public:
    static Registry& instance();

    struct Entry {
        ThreadBuffer* buffer;
        std::string name;
    };

    // 今あるバッファと名前。emergency なら、ロックを取れなくても待たずに読む（落ちたスレッドが持ったままかもしれない）。
    std::vector<Entry> snapshot(bool emergency = false);

    ThreadBuffer* create(uint64_t osThreadId);
    void setName(ThreadBuffer* buffer, std::string_view name);
    void removeRetired(const std::vector<ThreadBuffer*>& done);

    // これから作るバッファの大きさ・取っておく割合（start() の Config から）。
    void setBufferSettings(size_t bytes, uint32_t reservePercent);

    uint32_t registerSite(const Site& site, SiteIdSlot& slot) noexcept;
    const Site* site(uint32_t id) const noexcept;

    // 集計の呼び出し位置に番号を振る（1〜。足りなければ 0 で、その集計は数えない）。
    uint32_t registerStat(StatSite& site) noexcept;

private:
    static constexpr size_t kSiteChunkSize = 1024;
    static constexpr size_t kSiteChunks = 1024;  // 呼び出し位置は最大 約100万か所

    std::mutex mutex_;
    std::vector<std::unique_ptr<ThreadBuffer>> buffers_;
    std::vector<std::string> names_;  // buffers_ と同じ順
    uint32_t nextThreadIndex_ = 0;
    size_t bufferBytes_ = size_t{1} << 20;
    uint32_t reservePercent_ = 10;

    std::mutex siteMutex_;
    uint32_t nextSiteId_ = 1;
    std::atomic<std::atomic<const Site*>*> siteChunks_[kSiteChunks]{};
    uint32_t nextStatIndex_ = 1;
};

}  // namespace lg::detail
