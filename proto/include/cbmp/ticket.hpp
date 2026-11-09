#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <unordered_map>

namespace cbmp {
constexpr int64_t kClockSkewSeconds = 60;

// Mesmos códigos e mesma ordem de checagem de packages/contracts/src/ticket.ts.
enum class TicketError { Malformed, BadVersion, UnknownKid, BadSignature, WrongAudience, Expired, NotYetValid };
std::string_view toString(TicketError e);

struct TicketPayload {
  int v = 0;
  std::string kid, sub, did, name, aud, jti;
  int64_t iat = 0, exp = 0;
};

// kid → chave pública Ed25519 (32 bytes raw).
using PublicKeys = std::unordered_map<std::string, std::array<uint8_t, 32>>;

std::expected<TicketPayload, TicketError> verifyTicket(std::string_view ticket, const PublicKeys& keys,
                                                       std::string_view audience, int64_t nowSeconds);

// jti já usados; um jti não pode ser reutilizado até exp + kClockSkewSeconds.
class JtiCache {
public:
  // true se o jti ainda está vivo no cache (replay); senão registra e devolve false.
  bool seenOrInsert(const std::string& jti, int64_t expiresAt, int64_t now);
  size_t size() const { return entries_.size(); }
private:
  void purge(int64_t now);
  std::unordered_map<std::string, int64_t> entries_;
  size_t insertsSincePurge_ = 0;
};
}  // namespace cbmp
