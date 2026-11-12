#pragma once
// Quem resgata o connect.json (spec, "Dois processos na abertura").
#include <cstdint>

namespace cbmp::client {
enum class ClaimDecision { Wait, Claim, Superseded };

// !isCurrent → Superseded; inWorld → Claim; senão Claim quando secondsSinceFirstTick ≥ 45 (época 1) ou ≥ 5
// (época > 1); senão Wait.
ClaimDecision DecideClaim(bool isCurrent, bool inWorld, double secondsSinceFirstTick, uint64_t epoch);
}  // namespace cbmp::client
