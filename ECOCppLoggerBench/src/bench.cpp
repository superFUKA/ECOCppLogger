// ECOCppLogger の性能測定。Release で実行する：
//   ecobuild run --project ECOCppLoggerBench --configuration Release
//
// 測るもの
//   1. 呼び出し1回のコスト（記録する側のスレッドの時間。ファイルへの書き出しは lg::writeOut() のときだけ）
//   2. 複数のスレッドから同時に書いたときの1回のコスト
//   3. lg::writeOut() の書き出しの速さ（1秒あたりの件数）と、読む側（文章・JSON にする）の速さ

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "ECOCppLogger/ECOCppLogger.h"
#include "ECOCppLogger/Reader.h"

namespace {

using Clock = std::chrono::steady_clock;

std::atomic<uint64_t> g_sink{0};  // 最適化で消されないように
std::string g_directory;

ECO_LOG_TYPE(Physics, "physics");

lg::Config benchConfig() {
    const auto path = (std::filesystem::temp_directory_path() / "ECOCppLoggerBench").u8string();
    g_directory.assign(reinterpret_cast<const char*>(path.data()), path.size());
    lg::Config config;
    config.directory = g_directory.c_str();
    config.threadBufferBytes = uint64_t{64} << 20;
    config.maxFileBytes = uint64_t{256} << 20;
    config.maxFiles = 2;
    config.crash.handler = false;
    config.statIntervalMs = 60'000;
    return config;
}

// batch 回呼ぶ（書き出しは測らない）を rounds 回。1回あたりのナノ秒の最良を返す。
template <class Function>
double perCall(const char* name, Function function, int rounds = 7, int batch = 100'000) {
    double best = 1e30;
    for (int round = 0; round < rounds; ++round) {
        lg::writeOut();
        const auto start = Clock::now();
        for (int i = 0; i < batch; ++i) {
            function(i);
        }
        const double ns = std::chrono::duration<double, std::nano>(Clock::now() - start).count() / batch;
        best = std::min(best, ns);
    }
    std::printf("  %-52s %7.2f ns\n", name, best);
    return best;
}

void singleThread() {
    std::printf("\n[1] 呼び出し1回のコスト（100000回×7周の最良）\n");
    perCall("空のループ（基準）", [](int i) { g_sink.fetch_add(static_cast<uint64_t>(i), std::memory_order_relaxed); });
    perCall("snprintf で整形だけ（比較：普通のログの最低限）", [](int i) {
        char buffer[128];
        std::snprintf(buffer, sizeof(buffer), "player %d hp=%d pos=(%f, %f)", i, 100 - i % 100, i * 0.5, i * 0.25);
        g_sink.fetch_add(static_cast<unsigned char>(buffer[7]), std::memory_order_relaxed);
    });
    lg::setLevel(lg::Level::Info);
    perCall("ECO_LOG_DEBUG（重さで切られる）", [](int i) { ECO_LOG_DEBUG("hidden {}", i); });
    lg::setLevel(lg::Level::Trace);
    perCall("ECO_LOG_INFO 引数なし", [](int) { ECO_LOG_INFO("tick"); });
    perCall("ECO_LOG_INFO 整数2つ", [](int i) { ECO_LOG_INFO("player {id} hp={hp}", i, 100 - i % 100); });
    perCall("ECO_LOG_INFO 整数2つ＋小数2つ", [](int i) {
        ECO_LOG_INFO("player {id} hp={hp} pos=({x}, {y})", i, 100 - i % 100, i * 0.5, i * 0.25);
    });
    perCall("ECO_LOG(Warn | Physics) 小数1つ", [](int i) { ECO_LOG(Warn | Physics, "depth={d:.3}", i * 0.01); });
    perCall("ECO_LOG_INFO 文字列リテラル", [](int) { ECO_LOG_INFO("name={name}", "goblin"); });
    static const char* const kName = "goblin";
    perCall("ECO_LOG_INFO const char*", [](int) { ECO_LOG_INFO("name={name}", kName); });
    static const std::string kText = "goblin warrior";
    perCall("ECO_LOG_INFO std::string", [](int) { ECO_LOG_INFO("name={name}", kText); });

    lg::stop();
    lg::Config noTime = benchConfig();
    noTime.recordTime = false;
    lg::start(noTime);
    perCall("ECO_LOG_INFO 整数2つ（時刻なし）", [](int i) { ECO_LOG_INFO("player {id} hp={hp}", i, 100 - i % 100); });
    lg::stop();
    lg::start(benchConfig());

    std::printf("\n    毎フレーム何万回も通る所\n");
    perCall("ECO_LOG_EVERY_N(1000)", [](int i) { ECO_LOG_EVERY_N(Debug, 1000, "step {i}", i); });
    perCall("ECO_LOG_ON_CHANGE（値が同じ）", [](int i) { ECO_LOG_ON_CHANGE(Info, "state", i / 1'000'000); });
    perCall("ECO_STAT", [](int i) { ECO_STAT("bench.value", i); });
    perCall("ECO_STAT_COUNT", [](int) { ECO_STAT_COUNT("bench.count"); });
    perCall("ECO_STAT_HISTORY(64)", [](int i) { ECO_STAT_HISTORY("bench.history", 64, i); });
    perCall("ECO_SCOPE（入る＋出る）", [](int i) {
        ECO_SCOPE("Solve {i}", i);
        g_sink.fetch_add(1, std::memory_order_relaxed);
    });
    perCall("ECO_ASSERT（成功）", [](int i) { ECO_ASSERT(i >= 0, "i={i}", i); });
    {
        ECO_LOG_BATCH(rows, Debug, "candidate {id} distance {d}", int, float);
        perCall("ECO_LOG_BATCH の add（int, float）", [&](int i) { rows.add(i, static_cast<float>(i) * 0.5f); });
    }
}

void multiThread() {
    std::printf("\n[2] 複数のスレッドから同時に（1スレッド 200000回、1回あたり）\n");
    for (int threads : {1, 2, 4, 8}) {
        lg::writeOut();
        std::atomic<int> ready{0};
        std::atomic<bool> go{false};
        std::vector<double> results(static_cast<size_t>(threads));
        std::vector<std::thread> workers;
        for (int t = 0; t < threads; ++t) {
            workers.emplace_back([&, t] {
                ECO_LOG_INFO("warm {t}", t);  // バッファを先に作る
                ready.fetch_add(1);
                while (!go.load()) {
                }
                const auto start = Clock::now();
                for (int i = 0; i < 200'000; ++i) {
                    ECO_LOG_INFO("thread {t} value {i}", t, i);
                }
                results[static_cast<size_t>(t)] =
                    std::chrono::duration<double, std::nano>(Clock::now() - start).count() / 200'000;
            });
        }
        while (ready.load() < threads) {
        }
        go.store(true);
        for (auto& worker : workers) worker.join();
        double worst = 0;
        for (double result : results) worst = std::max(worst, result);
        std::printf("  %d スレッド                                             %7.2f ns（一番遅いスレッド）\n", threads,
                    worst);
    }
}

void writeAndRead() {
    std::printf("\n[3] 書き出しと読む側\n");
    lg::stop();  // 新しいファイルに、測る分だけを書く
    lg::start(benchConfig());
    constexpr int kRecords = 1'000'000;
    for (int i = 0; i < kRecords; ++i) {
        ECO_LOG_INFO("player {id} hp={hp} pos=({x}, {y})", i, 100 - i % 100, i * 0.5, i * 0.25);
    }
    auto start = Clock::now();
    lg::writeOut();
    double seconds = std::chrono::duration<double>(Clock::now() - start).count();
    std::printf("  lg::writeOut()（バッファ → .ecolog）                    %7.1f M件/秒\n", kRecords / seconds / 1e6);
    const std::string file = lg::currentFile();
    lg::stop();
    std::error_code error;
    const auto bytes = std::filesystem::file_size(std::filesystem::path(std::u8string(file.begin(), file.end())), error);
    std::printf("  .ecolog の大きさ                                        %7.1f B/件\n",
                static_cast<double>(bytes) / kRecords);
    for (lg::ViewFormat format : {lg::ViewFormat::Text, lg::ViewFormat::Json}) {
        size_t total = 0;
        start = Clock::now();
        lg::readLog(file.c_str(), format, {},
                    [](const char*, size_t size, void* user) { *static_cast<size_t*>(user) += size; }, &total);
        seconds = std::chrono::duration<double>(Clock::now() - start).count();
        std::printf("  読む側（%s）                                            %7.1f M行/秒（%.0f B/行）\n",
                    format == lg::ViewFormat::Text ? "文章" : "JSON", kRecords / seconds / 1e6,
                    static_cast<double>(total) / kRecords);
    }
}

}  // namespace

int main() {
    lg::start(benchConfig());
    singleThread();
    multiThread();
    writeAndRead();
    lg::stop();
    std::printf("\n(sink %llu)\n", static_cast<unsigned long long>(g_sink.load()));
    return 0;
}
