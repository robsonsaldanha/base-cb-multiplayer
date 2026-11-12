#include <cbmp/client/ClaimPolicy.hpp>
#include <cbmp/client/Limits.hpp>

namespace cbmp::client {
ClaimDecision DecideClaim(bool isCurrent, bool inWorld, double secondsSinceFirstTick, uint64_t epoch) {
    if (!isCurrent) return ClaimDecision::Superseded;
    if (inWorld) return ClaimDecision::Claim;
    const double delay = epoch > 1 ? limits::kClaimDelaySuccessorSec : limits::kClaimDelayFirstSec;
    return secondsSinceFirstTick >= delay ? ClaimDecision::Claim : ClaimDecision::Wait;
}
}  // namespace cbmp::client
