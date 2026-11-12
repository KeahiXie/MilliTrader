#pragma once
// Journal replay: rebuild a session's engine from its journal and check it decides the same.
// Pass 1 loads config and instruments; pass 2 feeds inputs in order to a fresh TradeEngine and
// compares each order/reject with the journaled one. Returns the final state hash.

#include <cstdint>
#include <functional>
#include <string>

#include "engine/journal.hpp"
#include "engine/trade_engine.hpp"

namespace mt {

struct ReplayResult {
    bool ok = false;
    std::string error;
    int64_t inputs = 0;
    int64_t outputs_checked = 0;
    int64_t output_mismatches = 0;
    int64_t journaled_by_type[4] = {};  // New, Cancel, Replace, Reconcile requests in the journal
    int64_t journaled_rejects = 0;
    bool truncated_tail = false;  // the session was killed mid-write; replayed up to the cut
    uint32_t journal_engine_version = 0;  // logic version that wrote the journal; 0 = not recorded
    std::string journal_build_id;
    bool version_known() const { return journal_engine_version != 0; }
    bool version_matches() const { return journal_engine_version == kEngineLogicVersion; }
    uint64_t state_hash = 0;
};

// inspect (optional) runs on the replayed engine at the end, e.g. to dump it into SQLite.
ReplayResult replay_journal(const std::string& path,
                            const std::function<void(const TradeEngine&, const InstrumentTable&)>& inspect = nullptr);

}  // namespace mt
