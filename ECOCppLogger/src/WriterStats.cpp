// 集計（ECO_STAT 等）を期間ごとに1行ずつ出す、最近の値（ECO_STAT_HISTORY）を写し取って出す。

#include <algorithm>
#include <cmath>

#include "ECOCppLogger/Logger.h"
#include "Registry.h"
#include "Writer.h"

namespace lg::detail {

namespace {

std::string_view kindName(StatKind kind) {
    switch (kind) {
    case StatKind::Count: return "count";
    case StatKind::Time: return "time";
    case StatKind::Value: break;
    }
    return "value";
}

void appendSource(std::string& out, const StatSite& site) {
    std::string source(sourceFileName(site.file ? site.file : "", false));
    source.push_back(':');
    appendUnsigned(source, site.line);
    appendJsonString(out, source);
}

void appendRounded(std::string& out, double value) {
    // 集計は読みやすさのため有効数字を抑える（小数3桁まで）
    appendDouble(out, std::round(value * 1000.0) / 1000.0);
}

}  // namespace

double Writer::ticksToMicroseconds(double ticks) const {
    const double perNanosecond = clock_ ? clock_->ticksPerNanosecond() : ticksPerMillisecond() / 1e6;
    return ticks / perNanosecond / 1000.0;
}

void Writer::collectStats(ThreadBuffer& buffer, uint64_t epoch, StatTotals& totals) {
    buffer.forEachStat([&](StatSlot& slot) {
        const uint64_t count = slot.count.load(std::memory_order_relaxed);
        if (count == slot.reportedCount) {
            return;
        }
        const double sum = slot.sum.load(std::memory_order_relaxed);
        const double last = slot.last.load(std::memory_order_relaxed);
        double minimum = last;
        double maximum = last;
        if (slot.epoch.load(std::memory_order_acquire) == epoch) {
            minimum = slot.minimum[epoch & 1].load(std::memory_order_relaxed);
            maximum = slot.maximum[epoch & 1].load(std::memory_order_relaxed);
        }
        const StatSite* site = slot.site.load(std::memory_order_acquire);
        auto [entry, inserted] = totals.try_emplace(std::string_view(site->name));
        StatTotal& total = entry->second;
        if (!total.site) total.site = site;
        if (inserted || total.count == 0) {
            total.minimum = minimum;
            total.maximum = maximum;
        } else {
            total.minimum = std::min(total.minimum, minimum);
            total.maximum = std::max(total.maximum, maximum);
        }
        total.count += count - slot.reportedCount;
        total.sum += sum - slot.reportedSum;
        total.last = last;
        slot.reportedCount = count;
        slot.reportedSum = sum;
    });
}

void Writer::foldRetiredStats(ThreadBuffer& buffer) {
    collectStats(buffer, g_statEpoch.load(std::memory_order_relaxed), carried_);
}

void Writer::reportStats(bool force) {
    const uint64_t now = steadyNanoseconds();
    const uint64_t elapsed = now - lastStatNanoseconds_;
    if (!force && elapsed < uint64_t{config_.statIntervalMs} * 1'000'000) {
        return;
    }
    lastStatNanoseconds_ = now;
    // 期間を進めてから、前の期間の最小・最大を読む（持ち主はもう次の期間の側に書く）。
    const uint64_t epoch = g_statEpoch.fetch_add(1, std::memory_order_acq_rel);
    StatTotals totals = std::move(carried_);
    carried_.clear();
    for (auto& entry : Registry::instance().snapshot(crashed_.load(std::memory_order_relaxed))) {
        collectStats(*entry.buffer, epoch, totals);
    }
    if (totals.empty()) {
        return;
    }
    std::vector<std::pair<std::string_view, StatTotal>> sorted(totals.begin(), totals.end());
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    const double milliseconds = static_cast<double>(elapsed) / 1e6;
    const uint64_t tick = ticks();
    for (const auto& [name, total] : sorted) {
        if (total.count == 0) continue;
        const StatSite* site = total.site;
        event_ = "\"lv\":\"stat\",\"name\":";
        appendJsonString(event_, site->name);
        event_.append(",\"kind\":\"");
        event_.append(kindName(site->kind));
        event_.append("\",\"n\":");
        appendUnsigned(event_, total.count);
        if (site->kind == StatKind::Count) {
            event_.append(",\"per_s\":");
            appendRounded(event_, milliseconds > 0 ? static_cast<double>(total.count) * 1000.0 / milliseconds : 0.0);
        } else {
            const bool time = site->kind == StatKind::Time;
            auto value = [&](double raw) { return time ? ticksToMicroseconds(raw) : raw; };
            if (time) event_.append(",\"unit\":\"us\"");
            event_.append(",\"avg\":");
            appendRounded(event_, value(total.sum / static_cast<double>(total.count)));
            event_.append(",\"min\":");
            appendRounded(event_, value(total.minimum));
            event_.append(",\"max\":");
            appendRounded(event_, value(total.maximum));
            event_.append(",\"last\":");
            appendRounded(event_, value(total.last));
            event_.append(",\"sum\":");
            appendRounded(event_, value(total.sum));
        }
        event_.append(",\"ms\":");
        appendRounded(event_, milliseconds);
        event_.append(",\"src\":");
        appendSource(event_, *site);
        writeEvent(EventKind::Stat, tick, event_);
    }
}

void Writer::dumpHistory(std::string_view reason) {
    std::lock_guard lock(io_);  // 書き出しがバッファを片付けている間は待つ
    captureHistory(reason);
}

void Writer::captureHistory(std::string_view reason) {
    const uint64_t now = ticks();
    const double perMillisecond = ticksPerMillisecond();
    for (auto& entry : Registry::instance().snapshot(crashed_.load(std::memory_order_relaxed))) {
        entry.buffer->forEachStat([&](StatSlot& slot) {
            const HistoryRing* ring = slot.history.load(std::memory_order_acquire);
            const StatSite* site = slot.site.load(std::memory_order_acquire);
            if (!ring || !site) return;
            const bool time = site->kind == StatKind::Time;
            std::string body = "\"lv\":\"history\",\"name\":";
            appendJsonString(body, site->name);
            body.append(",\"th\":");
            appendJsonString(body, entry.name);
            body.append(",\"reason\":");
            appendJsonString(body, reason);
            if (time) body.append(",\"unit\":\"us\"");
            body.append(",\"points\":[");
            bool first = true;
            ring->read([&](const HistoryRing::Point& point) {
                if (point.tick > now) return;  // 呼んだ後の値は含めない
                if (!first) body.push_back(',');
                first = false;
                body.append("{\"ago_ms\":");
                appendRounded(body, static_cast<double>(now - point.tick) / perMillisecond);
                body.append(",\"f\":");
                appendUnsigned(body, point.frame);
                body.append(",\"v\":");
                appendRounded(body, time ? ticksToMicroseconds(point.value) : point.value);
                body.push_back('}');
            });
            body.append("],\"src\":");
            appendSource(body, *site);
            pending_.push_back({EventKind::History, now, std::move(body)});
        });
    }
}

void Writer::queueEvent(EventKind kind, uint64_t tick, std::string body) {
    std::lock_guard lock(io_);
    pending_.push_back({kind, tick, std::move(body)});
}

void Writer::writePending() {
    for (const PendingEvent& event : pending_) {
        writeEvent(event.kind, event.tick, event.body);
    }
    pending_.clear();
}

}  // namespace lg::detail
