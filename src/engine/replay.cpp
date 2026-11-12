#include "engine/replay.hpp"

#include <deque>
#include <optional>

namespace mt {

namespace {

bool same_order(const OrderRequest& a, const OrderRequest& b) {
    // decided_mono_ns is a stats timestamp, not a decision: ignore it.
    return a.type == b.type && a.side == b.side && a.post_only == b.post_only && a.instrument == b.instrument &&
           a.order_id == b.order_id && a.price == b.price && a.qty == b.qty;
}

// Collects what the replayed engine decides, to compare with the journal.
class RecordingSink : public OrderSink {
public:
    void send_order(const OrderRequest& request) override { decided.push_back({request, RiskCheckResult::Allowed, true}); }
    void on_risk_reject(const OrderRequest& request, RiskCheckResult reason) override {
        decided.push_back({request, reason, false});
    }
    struct Decision {
        OrderRequest order;
        RiskCheckResult reason;
        bool sent;
    };
    std::deque<Decision> decided;
};

}  // namespace

ReplayResult replay_journal(const std::string& path,
                            const std::function<void(const TradeEngine&, const InstrumentTable&)>& inspect) {
    ReplayResult result;

    // Pass 1: settings and instruments.
    std::optional<EngineConfig> config;
    InstrumentTable instruments;
    {
        JournalReader reader(path);
        if (!reader.ok()) {
            result.error = reader.error();
            return result;
        }
        while (auto record = reader.next()) {
            if (auto* c = std::get_if<EngineConfig>(&*record)) config = *c;
            if (auto* i = std::get_if<InstrumentInfo>(&*record)) instruments.add(*i);
        }
        if (!reader.error().empty()) {
            result.error = reader.error();
            return result;
        }
    }
    if (!config) {
        result.error = "journal has no engine config";
        return result;
    }

    // Pass 2: inputs in order, outputs checked as they come.
    RecordingSink sink;
    TradeEngine engine(*config, instruments, sink);
    JournalReader reader(path);
    auto check_next = [&](const OrderRequest& journaled, bool sent, RiskCheckResult reason) {
        ++result.outputs_checked;
        if (sink.decided.empty()) {
            ++result.output_mismatches;
            return;
        }
        const auto decision = sink.decided.front();
        sink.decided.pop_front();
        if (decision.sent != sent || !same_order(decision.order, journaled) || (!sent && decision.reason != reason)) {
            ++result.output_mismatches;
        }
    };
    while (auto record = reader.next()) {
        if (auto* feed = std::get_if<FeedEvent>(&*record)) {
            engine.on_feed_event(*feed);
            ++result.inputs;
        } else if (auto* gateway = std::get_if<GatewayEvent>(&*record)) {
            engine.on_gateway_event(*gateway);
            ++result.inputs;
        } else if (auto* control = std::get_if<ControlEvent>(&*record)) {
            engine.on_control_event(*control);
            ++result.inputs;
        } else if (auto* out = std::get_if<OrderOutRecord>(&*record)) {
            result.journaled_by_type[static_cast<int>(out->order.type) & 3] += 1;
            check_next(out->order, true, RiskCheckResult::Allowed);
        } else if (auto* reject = std::get_if<RiskRejectRecord>(&*record)) {
            ++result.journaled_rejects;
            check_next(reject->order, false, reject->reason);
        }
    }
    result.truncated_tail = reader.truncated_tail();
    result.journal_engine_version = reader.engine_logic_version();
    result.journal_build_id = reader.build_id();
    // Unjournaled decisions are mismatches, unless a crash cut the journal before they hit disk.
    if (!result.truncated_tail) result.output_mismatches += static_cast<int64_t>(sink.decided.size());
    result.state_hash = engine.state_hash();
    result.ok = reader.error().empty();
    result.error = reader.error();
    if (inspect) inspect(engine, instruments);
    return result;
}

}  // namespace mt
