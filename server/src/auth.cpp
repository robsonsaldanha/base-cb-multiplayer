#include "cbserver/auth.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>

#include <cbmp/base64url.hpp>

namespace cbserver {

KeyProvider::~KeyProvider() = default;

cbmp::PublicKeys StaticKeys::fromDevKey(std::string_view b64) {
  auto raw = cbmp::base64urlDecodeCanonical(b64);
  if (!raw || raw->size() != 32) throw std::runtime_error("auth.devPublicKey deve ser base64url canônico de 32 bytes");
  std::array<uint8_t, 32> k{};
  std::copy(raw->begin(), raw->end(), k.begin());
  return {{kDevKid, k}};
}

Authenticator::Authenticator(KeyProvider& keys, std::string serverId, std::function<int64_t()> nowSec)
    : keys_(keys), serverId_(std::move(serverId)), nowSec_(std::move(nowSec)) {}

std::expected<cbmp::TicketPayload, std::string> Authenticator::check(std::string_view ticket) {
  const int64_t now = nowSec_();
  auto r = cbmp::verifyTicket(ticket, keys_.keys(), serverId_, now);
  if (!r) {
    if (r.error() == cbmp::TicketError::UnknownKid) keys_.requestRefresh();
    return std::unexpected("ticket inválido (" + std::string(cbmp::toString(r.error())) + ")");
  }
  // Só um ticket inteiramente válido consome o jti.
  if (jtis_.seenOrInsert(r->jti, r->exp, now))
    return std::unexpected(std::string("ticket já usado"));
  return std::move(*r);
}

}  // namespace cbserver
