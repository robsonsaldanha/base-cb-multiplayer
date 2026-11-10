#pragma once
#include <algorithm>

namespace cbserver {

// Token bucket: `perSecond` fichas por segundo até `burst`; começa cheio.
class TokenBucket {
 public:
  TokenBucket(double perSecond, double burst) : rate_(perSecond), burst_(burst), tokens_(burst) {}
  bool take(double nowSec) {
    if (last_ >= 0) tokens_ = std::min(burst_, tokens_ + (nowSec - last_) * rate_);
    last_ = nowSec;
    if (tokens_ < 1.0) return false;
    tokens_ -= 1.0;
    return true;
  }

 private:
  double rate_, burst_, tokens_;
  double last_ = -1;
};

}  // namespace cbserver
