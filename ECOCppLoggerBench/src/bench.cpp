// ECOCppLogger の性能測定。Release で実行する：
//   ecobuild run ECOCppLoggerBench --configuration Release
//
// 測るもの
//   1. 呼び出し1回のコスト（記録する側のスレッドの時間。整形・書き出しは裏のスレッド）
//   2. 呼び出し1回の時間のばらつき（p50・p99・p99.9・最大）
//   3. 複数のスレッドから同時に書いたときの量と、捨てた数
//   4. 毎フレーム 5 万回通る処理に、集計・間引きを入れたときの1フレームあたりの増え方
//   5. 裏のスレッドの書き出しの速さ（1秒あたりの行数）
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "ECOCppLogger/ECOCppLogger.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <intrin.h>
#endif
#include <cstdlib>

namespace {

using Clock = std::chrono::steady_clock;
using eco::log::Level;

std::atomic<std::uint64_t> g_sink{0};  // 最適化で消されないように

std::filesystem::path benchDirectory() {
    return std::filesystem::temp_directory_path() / "ECOCppLoggerBench";
}

eco::log::Config benchConfig() {
    eco::log::Config config;
    config.directory = benchDirectory();
    config.consoleLevel = Level::Off;
    config.threadBufferBytes = std::size_t{64} << 20;
    config.maxFileBytes = std::uint64_t{256} << 20;  // 測定の出力は大きいので、残すのは最後の2つ（512MB まで）
    config.maxFiles = 2;
    config.installCrashHandler = false;
    return config;
}

// 1回 batch 回呼び、裏のスレッドが追いつくのを待つ（待つ時間は測らない）を rounds 回。1回あたりのナノ秒を返す。
template <class Function>
double perCall(const char* name, int rounds, int batch, Function function) {
    double best = 1e30;
    for (int round = 0; round < rounds; ++round) {
        eco::log::flush();
        const auto start = Clock::now();
        for (int i = 0; i < batch; ++i) {
            function(i);
        }
        const double ns = std::chrono::duration<double, std::nano>(Clock::now() - start).count() / batch;
        best = std::min(best, ns);
    }
    std::printf("  %-44s %7.2f ns/call\n", name, best);
    return best;
}

void singleThread() {
    std::printf("\n[1] 呼び出し1回のコスト（記録する側。%s）\n", "100000回×5周の最良");
    constexpr int kRounds = 5;
    constexpr int kBatch = 100000;
    perCall("空のループ（比較の基準）", kRounds, kBatch, [](int i) { g_sink.fetch_add(i, std::memory_order_relaxed); });
    perCall("snprintf で整形だけ（比較：普通のログの最低限）", kRounds, kBatch, [](int i) {
        char buffer[128];
        std::snprintf(buffer, sizeof(buffer), "player %d hp=%d pos=(%f, %f)", i, 100 - i % 100, i * 0.5, i * 0.25);
        g_sink.fetch_add(static_cast<unsigned char>(buffer[7]), std::memory_order_relaxed);
    });
    eco::log::setLevel(Level::Info);
    perCall("ECO_LOG_DEBUG（重さで切られる）", kRounds, kBatch, [](int i) { ECO_LOG_DEBUG("hidden {}", i); });
    eco::log::setLevel(Level::Trace);
    perCall("ECO_LOG_INFO 引数なし", kRounds, kBatch, [](int) { ECO_LOG_INFO("tick"); });
    perCall("ECO_LOG_INFO 整数2つ＋小数2つ", kRounds, kBatch, [](int i) {
        ECO_LOG_INFO("player {} hp={} pos=({}, {})", i, 100 - i % 100, i * 0.5, i * 0.25);
    });
    const std::string name = "enemy_goblin_042";
    perCall("ECO_LOG_INFO 文字列（16文字）＋整数", kRounds, kBatch,
            [&](int i) { ECO_LOG_INFO("spawn {} at {}", name, i); });
    perCall("ECO_LOG_EVERY_N 1000回に1回", kRounds, kBatch,
            [](int i) { ECO_LOG_EVERY_N(Level::Debug, 1000, "step {}", i); });
    perCall("ECO_STAT（値の集計）", kRounds, kBatch, [](int i) { ECO_STAT("bench.value", i); });
    perCall("ECO_COUNT（回数）", kRounds, kBatch, [](int) { ECO_COUNT("bench.count"); });
    perCall("ECO_LOG_ON_CHANGE（値が変わらない回）", kRounds, kBatch,
            [](int i) { ECO_LOG_ON_CHANGE(Level::Info, "bench.state", i / 1000000); });
    perCall("ECO_LOG_ON_CHANGE（100回に1回変わる）", kRounds, kBatch,
            [](int i) { ECO_LOG_ON_CHANGE(Level::Info, "bench.state2", i / 100); });
    perCall("ECO_HISTORY（最近の値を覚える＋集計）", kRounds, kBatch, [](int i) { ECO_HISTORY("bench.history", i); });
    {
        ECO_LOG_BATCH(batch, Level::Debug, "candidate {} value {}", int, int);
        perCall("ECO_LOG_BATCH の add（整数2つ）", kRounds, kBatch, [&](int i) { batch.add(i, i & 1023); });
    }
    perCall("ECO_SCOPE_TIME（区間の時間）", kRounds, kBatch, [](int i) {
        ECO_SCOPE_TIME("bench.scope");
        g_sink.fetch_add(i, std::memory_order_relaxed);
    });
}

void latency() {
    std::printf("\n[2] ECO_LOG_INFO（整数2つ＋小数2つ）1回の時間のばらつき（100万回）\n");
    constexpr int kCount = 1000000;
    std::vector<std::uint64_t> samples(kCount);
    eco::log::Clock clock;
    eco::log::flush();
    for (int i = 0; i < kCount; ++i) {
        const auto start = eco::log::ticks();
        ECO_LOG_INFO("player {} hp={} pos=({}, {})", i, 100 - i % 100, i * 0.5, i * 0.25);
        samples[i] = eco::log::ticks() - start;
        if (i % 200000 == 0) {
            eco::log::flush();
        }
    }
    clock.recalibrate();
    std::sort(samples.begin(), samples.end());
    const auto at = [&](double q) {
        return clock.toNanoseconds(static_cast<double>(samples[static_cast<std::size_t>(q * (kCount - 1))]));
    };
    std::printf("  p50 %.1f ns  p99 %.1f ns  p99.9 %.1f ns  最大 %.1f ns（TSC を読む時間を含む）\n", at(0.5), at(0.99),
                at(0.999), at(1.0));
}

void multiThread() {
    std::printf("\n[3] 複数のスレッドから同時に（各スレッド 20万回）\n");
    const unsigned cores = std::max(2u, std::thread::hardware_concurrency());
    for (unsigned threads : {1u, 4u, std::min(8u, cores)}) {
        eco::log::flush();
        constexpr int kPerThread = 200000;
        std::vector<std::thread> workers;
        std::atomic<int> ready{0};
        std::atomic<bool> go{false};
        std::vector<double> nsPerCall(threads);
        for (unsigned t = 0; t < threads; ++t) {
            workers.emplace_back([&, t] {
                ECO_LOG_INFO("warm up {}", t);  // バッファを作っておく
                ready.fetch_add(1);
                while (!go.load()) {
                }
                const auto start = Clock::now();
                for (int i = 0; i < kPerThread; ++i) {
                    ECO_LOG_INFO("worker {} item {} value {}", t, i, i * 0.001);
                }
                nsPerCall[t] = std::chrono::duration<double, std::nano>(Clock::now() - start).count() / kPerThread;
            });
        }
        while (ready.load() < static_cast<int>(threads)) {
        }
        const auto start = Clock::now();
        go.store(true);
        for (auto& worker : workers) {
            worker.join();
        }
        const double produceMs = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        eco::log::flush();
        const double totalMs = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        double average = 0;
        for (double ns : nsPerCall) {
            average += ns / threads;
        }
        const double total = static_cast<double>(threads) * kPerThread;
        std::printf("  %u スレッド：1回 %.1f ns（各スレッドの平均）、書き込み %.1f M件/秒、ファイルまで %.1f M行/秒\n",
                    threads, average, total / produceMs / 1000.0, total / totalMs / 1000.0);
    }
}

void frameScenario() {
    std::printf("\n[4] 毎フレーム 5万回通る処理（60フレームの平均）\n");
    constexpr int kFrames = 60;
    constexpr int kCallsPerFrame = 50000;
    const auto work = [](int i) {
        // 衝突判定のような軽い計算の代わり
        std::uint64_t x = static_cast<std::uint64_t>(i) * 2654435761u;
        x ^= x >> 13;
        return static_cast<int>(x & 1023);
    };
    const auto run = [&](const char* name, auto&& body) {
        eco::log::flush();
        const auto start = Clock::now();
        for (int frame = 0; frame < kFrames; ++frame) {
            ECO_FRAME();
            for (int i = 0; i < kCallsPerFrame; ++i) {
                body(i, work(i));
            }
        }
        const double ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count() / kFrames;
        std::printf("  %-48s %.3f ms/frame\n", name, ms);
        return ms;
    };
    const double base = run("ログなし", [](int, int value) { g_sink.fetch_add(value, std::memory_order_relaxed); });
    const double stat = run("毎回 ECO_STAT", [](int, int value) {
        g_sink.fetch_add(value, std::memory_order_relaxed);
        ECO_STAT("frame.candidates", value);
    });
    const double sampled = run("毎回 ECO_STAT＋ECO_LOG_EVERY_N(10000)", [](int i, int value) {
        g_sink.fetch_add(value, std::memory_order_relaxed);
        ECO_STAT("frame.candidates2", value);
        ECO_LOG_EVERY_N(Level::Debug, 10000, "candidate {} value {}", i, value);
    });
    const double full = run("毎回 ECO_LOG_DEBUG（全部残す）", [](int i, int value) {
        g_sink.fetch_add(value, std::memory_order_relaxed);
        ECO_LOG_DEBUG("candidate {} value {}", i, value);
    });
    // まとめ書き：ループの外で1回だけ用意し、中では値を並べるだけ
    eco::log::flush();
    const auto batchStart = Clock::now();
    for (int frame = 0; frame < kFrames; ++frame) {
        ECO_FRAME();
        ECO_LOG_BATCH(batch, Level::Debug, "candidate {} value {}", int, int);
        for (int i = 0; i < kCallsPerFrame; ++i) {
            const int value = work(i);
            g_sink.fetch_add(value, std::memory_order_relaxed);
            batch.add(i, value);
        }
    }
    const double batched = std::chrono::duration<double, std::milli>(Clock::now() - batchStart).count() / kFrames;
    std::printf("  %-48s %.3f ms/frame\n", "毎回 ECO_LOG_BATCH の add（全部残す）", batched);
    std::printf("  → 1フレームあたりの増え方：集計 %.3f ms、集計＋間引き %.3f ms、全部残す %.3f ms、まとめ書きで全部残す %.3f ms\n",
                stat - base, sampled - base, full - base, batched - base);
}

void backendThroughput() {
    std::printf("\n[5] 裏のスレッドの書き出し（100万行をファイルまで）\n");
    constexpr int kCount = 1000000;
    eco::log::flush();
    const auto start = Clock::now();
    for (int i = 0; i < kCount; ++i) {
        ECO_LOG_INFO("player {} hp={} pos=({}, {})", i, 100 - i % 100, i * 0.5, i * 0.25);
    }
    const double produceMs = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    eco::log::flush();
    const double totalMs = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    std::printf("  書き込み %.0f ms、ファイルまで %.0f ms（%.2f M行/秒）\n", produceMs, totalMs, kCount / totalMs / 1000.0);
}

// ゲームループの終わりに書き出す：1フレームに 1000 件のログ＋5万件のまとめ書き、終わりに writeOut()。
void writeModes() {
    std::printf("\n[6] 書き出しのタイミング（1フレーム：ログ1000件＋まとめ書き5万件、終わりに writeOut。300フレームの平均）\n");
    constexpr int kFrames = 300;
    for (int variant = 0; variant < 4; ++variant) {
        const auto mode = variant == 0   ? eco::log::WriteMode::Interval
                          : variant == 1 ? eco::log::WriteMode::OnRequest
                                         : eco::log::WriteMode::Manual;
        eco::log::stop();
        auto config = benchConfig();
        config.writeMode = mode;
        if (variant == 3) {
            // 書き出し先を3つに分ける（人向け：Info 以上・まとめ書きなし／ゲーム：型 gameplay だけ／全部）
            config.destinations = {{.name = "human", .level = Level::Info, .excludeTypes = {"batch", "stat"}},
                                   {.name = "game", .types = {"gameplay"}},
                                   {.name = "all"}};
        }
        eco::log::start(config);
        double total = 0.0;
        double worst = 0.0;
        double writeTotal = 0.0;
        for (int frame = 0; frame < kFrames; ++frame) {
            const auto start = Clock::now();
            ECO_FRAME();
            for (int i = 0; i < 1000; ++i) {
                ECO_LOG_INFO("entity {} hp={}", i, i % 100);
            }
            {
                ECO_LOG_BATCH(batch, Level::Debug, "candidate {} value {}", int, int);
                for (int i = 0; i < 50000; ++i) {
                    batch.add(i, i & 1023);
                }
            }
            const auto beforeWrite = Clock::now();
            eco::log::writeOut();
            const auto end = Clock::now();
            const double ms = std::chrono::duration<double, std::milli>(end - start).count();
            total += ms;
            worst = std::max(worst, ms);
            writeTotal += std::chrono::duration<double, std::milli>(end - beforeWrite).count();
        }
        const char* name = variant == 0   ? "Interval（2msごとに裏で）"
                           : variant == 1 ? "OnRequest（頼んだときに裏で）"
                           : variant == 2 ? "Manual（呼んだスレッドで）"
                                          : "Manual＋書き出し先3つ";
        std::printf("  %-34s 1フレーム %.3f ms（最大 %.3f ms）、うち writeOut() %.3f ms\n", name, total / kFrames, worst,
                    writeTotal / kFrames);
    }
    eco::log::stop();
    eco::log::start(benchConfig());
}

// ---- ファイバー対策：自分のスレッドのバッファーを毎回読み直す方法の比べ ---------------------------------------
// ファイバーが中断の前後で別のスレッドへ移ると、コンパイラが thread_local の番地を使い回して古いスレッドの値を読む
// おそれがある。毎回読み直す方法を3つ比べる。
thread_local void* t_inlineSlot = &g_sink;

__declspec(noinline) void* readNoInline() {
    return t_inlineSlot;
}

#if defined(_WIN32) && defined(_M_X64)
DWORD g_tlsIndex = TLS_OUT_OF_INDEXES;
// TEB の TlsSlots（gs:[0x1480 + index*8]、index < 64）を直接1回読む。__readgsqword はコンパイラが使い回さない。
inline void* readTlsSlot() {
    return reinterpret_cast<void*>(__readgsqword(0x1480 + g_tlsIndex * 8));
}
#endif

void fiberSafeThreadLocal() {
    std::printf("\n[7] ファイバー対策：スレッドごとの値を毎回読み直す方法（1億回の平均）\n");
    constexpr int kCount = 100'000'000;
    const auto run = [&](const char* name, auto&& read) {
        std::uintptr_t sum = 0;
        const auto start = Clock::now();
        for (int i = 0; i < kCount; ++i) {
            sum += reinterpret_cast<std::uintptr_t>(read()) + static_cast<std::uintptr_t>(i);
            // 毎回「外の関数を呼んだ」ことにして、コンパイラに読み直しを強いる（ログの間にほかの処理がある状況）
            g_sink.fetch_add(sum & 1, std::memory_order_relaxed);
        }
        const double ns = std::chrono::duration<double, std::nano>(Clock::now() - start).count() / kCount;
        std::printf("  %-52s %.2f ns\n", name, ns);
    };
    run("thread_local をインラインで読む（今。ファイバーに安全でない）", [] { return t_inlineSlot; });
    run("thread_local を非インライン関数で読む", [] { return readNoInline(); });
#if defined(_WIN32) && defined(_M_X64)
    g_tlsIndex = TlsAlloc();
    if (g_tlsIndex < 64) {
        TlsSetValue(g_tlsIndex, &g_sink);
        run("TLS スロットを gs から直接読む（インライン・安全）", [] { return readTlsSlot(); });
        run("TlsGetValue（OS の関数）", [] { return TlsGetValue(g_tlsIndex); });
    }
#endif
}

}  // namespace

int main() {
    if (std::getenv("ECO_BENCH_TLS_ONLY")) {
        fiberSafeThreadLocal();
        return 0;
    }
    std::error_code error;
    std::filesystem::remove_all(benchDirectory(), error);
    eco::log::start(benchConfig());
    std::printf("ECOCppLogger の性能測定（出力：%s）\n", eco::log::currentFile().string().c_str());
    singleThread();
    {
        // 時刻を残さない設定
        eco::log::stop();
        auto config = benchConfig();
        config.recordTime = false;
        eco::log::start(config);
        std::printf("\n[1b] 時刻を残さない設定（Config::recordTime = false）\n");
        perCall("ECO_LOG_INFO 整数2つ＋小数2つ", 5, 100000, [](int i) {
            ECO_LOG_INFO("player {} hp={} pos=({}, {})", i, 100 - i % 100, i * 0.5, i * 0.25);
        });
        eco::log::stop();
        eco::log::start(benchConfig());
    }
    latency();
    multiThread();
    frameScenario();
    backendThroughput();
    writeModes();
    const auto file = eco::log::currentFile();
    eco::log::stop();
    std::printf("\nログ：%s（%.1f MB）\n", file.string().c_str(),
                static_cast<double>(std::filesystem::file_size(file, error)) / (1 << 20));
    // 最後の行（session.end）に、記録した数と捨てた数がある。
    std::FILE* input = std::fopen(file.string().c_str(), "rb");
    if (input) {
        std::fseek(input, -512, SEEK_END);
        char tail[513] = {};
        const std::size_t read = std::fread(tail, 1, 512, input);
        std::fclose(input);
        const std::string text(tail, read);
        const auto position = text.rfind("\"records\"");
        if (position != std::string::npos) {
            std::printf("最後の行：%s", text.c_str() + position);
        }
    }
    return 0;
}
