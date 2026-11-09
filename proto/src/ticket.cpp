// Porta de verifyTicket de packages/contracts/src/ticket.ts. Ordem das checagens (igual ao TS):
// formato/charset → base64url canônico → JSON (válido, compacto, sem chaves duplicadas) → forma (inteiros
// seguros, literais inteiros simples) → versão → kid → assinatura → audience → iat futuro → expiração.
#include <cbmp/ticket.hpp>

#include <cbmp/base64url.hpp>

#include <nlohmann/json.hpp>
#include <sodium.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <optional>
#include <span>
#include <system_error>
#include <vector>

namespace cbmp {
namespace {
constexpr int64_t kMaxSafeInteger = 9007199254740991;  // Number.MAX_SAFE_INTEGER
constexpr std::string_view kPayloadKeys[] = {"v", "kid", "sub", "did", "name", "aud", "iat", "exp", "jti"};

bool isDigit(char c) { return c >= '0' && c <= '9'; }

// /^[A-Za-z0-9_-]+$/
bool isB64urlText(std::string_view s) {
  if (s.empty()) return false;
  for (const char c : s) {
    const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || isDigit(c) || c == '-' || c == '_';
    if (!ok) return false;
  }
  return true;
}

// Buffer.toString('utf8'): decodificador UTF-8 do WHATWG; cada subparte máxima inválida vira U+FFFD.
std::string decodeUtf8Lossy(std::span<const uint8_t> b) {
  static constexpr char kReplacement[] = "\xEF\xBF\xBD";
  std::string out;
  out.reserve(b.size());
  size_t i = 0;
  while (i < b.size()) {
    const uint8_t c = b[i];
    if (c < 0x80) { out += static_cast<char>(c); ++i; continue; }
    size_t need = 0;
    uint8_t lower = 0x80, upper = 0xBF;
    if (c >= 0xC2 && c <= 0xDF) need = 1;
    else if (c >= 0xE0 && c <= 0xEF) { need = 2; if (c == 0xE0) lower = 0xA0; if (c == 0xED) upper = 0x9F; }
    else if (c >= 0xF0 && c <= 0xF4) { need = 3; if (c == 0xF0) lower = 0x90; if (c == 0xF4) upper = 0x8F; }
    else { out += kReplacement; ++i; continue; }
    size_t j = i + 1;
    size_t got = 0;
    while (got < need && j < b.size()) {
      const uint8_t lo = got == 0 ? lower : uint8_t{0x80};
      const uint8_t hi = got == 0 ? upper : uint8_t{0xBF};
      if (b[j] < lo || b[j] > hi) break;
      ++j; ++got;
    }
    if (got == need) out.append(reinterpret_cast<const char*>(b.data() + i), j - i);
    else out += kReplacement;  // o byte que quebrou a sequência é reprocessado
    i = j;
  }
  return out;
}

// isCompactJson: nenhum espaço/tab/quebra de linha fora de strings.
bool isCompactJson(std::string_view t) {
  bool inStr = false;
  for (size_t i = 0; i < t.size(); ++i) {
    const char c = t[i];
    if (inStr) {
      if (c == '\\') ++i;
      else if (c == '"') inStr = false;
    } else if (c == '"') inStr = true;
    else if (c == ' ' || c == '\t' || c == '\n' || c == '\r') return false;
  }
  return true;
}

std::string quotedKey(std::string_view k) { return "\"" + std::string(k) + "\":"; }

size_t countOccurrences(std::string_view t, std::string_view needle) {
  size_t n = 0;
  for (size_t p = t.find(needle); p != std::string_view::npos; p = t.find(needle, p + needle.size())) ++n;
  return n;
}

// noDuplicateKeys: text.split(`"<chave>":`).length === 2 para cada chave do payload.
bool noDuplicateKeys(std::string_view t) {
  for (const auto k : kPayloadKeys)
    if (countOccurrences(t, quotedKey(k)) != 1) return false;
  return true;
}

// plainIntegerLiterals: /"k":-?\d+[,}]/ casa em algum lugar e /"k":-?\d+[.eE]/ em nenhum, para v, iat, exp.
bool plainIntegerLiterals(std::string_view t) {
  for (const std::string_view k : {std::string_view("v"), std::string_view("iat"), std::string_view("exp")}) {
    const std::string needle = quotedKey(k);
    bool plain = false, nonPlain = false;
    for (size_t p = t.find(needle); p != std::string_view::npos; p = t.find(needle, p + 1)) {
      size_t q = p + needle.size();
      if (q < t.size() && t[q] == '-') ++q;
      const size_t digits = q;
      while (q < t.size() && isDigit(t[q])) ++q;
      if (q == digits || q >= t.size()) continue;
      if (t[q] == ',' || t[q] == '}') plain = true;
      if (t[q] == '.' || t[q] == 'e' || t[q] == 'E') nonPlain = true;
    }
    if (!plain || nonPlain) return false;
  }
  return true;
}

bool isHex4(std::string_view t, size_t at) {
  if (at + 4 > t.size()) return false;
  for (size_t i = at; i < at + 4; ++i) {
    const char c = t[i];
    if (!(isDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
  }
  return true;
}

unsigned hex4(std::string_view t, size_t at) {
  unsigned v = 0;
  std::from_chars(t.data() + at, t.data() + at + 4, v, 16);
  return v;
}

// Um número JSON (gramática já validada) que JSON.parse leria como ±Infinity.
bool overflowsDouble(std::string_view tok) {
  double d = 0;
  const auto [ptr, ec] = std::from_chars(tok.data(), tok.data() + tok.size(), d);
  if (ec == std::errc{} && ptr == tok.data() + tok.size()) return std::isinf(d);
  if (ec != std::errc::result_out_of_range) return false;
  // Fora de faixa: overflow ou underflow. Decide pela ordem de grandeza decimal.
  size_t i = tok[0] == '-' ? 1 : 0;
  int64_t mag = 0;
  bool nonZero = false;
  if (tok[i] != '0') {
    const size_t s = i;
    while (i < tok.size() && isDigit(tok[i])) ++i;
    mag = static_cast<int64_t>(i - s) - 1;
    nonZero = true;
  } else {
    ++i;
  }
  if (i < tok.size() && tok[i] == '.') {
    ++i;
    for (int64_t k = 1; i < tok.size() && isDigit(tok[i]); ++i, ++k)
      if (!nonZero && tok[i] != '0') { mag = -k; nonZero = true; }
  }
  if (!nonZero) return false;
  int64_t e = 0;
  if (i < tok.size() && (tok[i] == 'e' || tok[i] == 'E')) {
    ++i;
    bool neg = false;
    if (tok[i] == '+' || tok[i] == '-') { neg = tok[i] == '-'; ++i; }
    for (; i < tok.size(); ++i) e = std::min<int64_t>(e * 10 + (tok[i] - '0'), 1'000'000'000);
    if (neg) e = -e;
  }
  return mag + e > 0;
}

// O nlohmann recusa duas coisas que o JSON.parse aceita: \u de surrogate solto e números que estouram
// double. Reescreve só esses tokens (o resto do texto passa igual), mantendo o resultado de JSON.parse:
// surrogate solto → U+FFFD (o mais próximo representável em UTF-8); ±Infinity → ±1e300 (também não é
// inteiro seguro nem igual a 1). Fora disso a validade do JSON não muda.
std::string toParserInput(std::string_view t) {
  std::string out;
  out.reserve(t.size());
  bool inStr = false;
  size_t i = 0;
  while (i < t.size()) {
    const char c = t[i];
    if (inStr) {
      if (c == '\\') {
        if (i + 1 < t.size() && t[i + 1] == 'u' && isHex4(t, i + 2)) {
          const unsigned cp = hex4(t, i + 2);
          if (cp >= 0xD800 && cp <= 0xDBFF) {
            if (i + 7 < t.size() && t[i + 6] == '\\' && t[i + 7] == 'u' && isHex4(t, i + 8) &&
                hex4(t, i + 8) >= 0xDC00 && hex4(t, i + 8) <= 0xDFFF) {
              out.append(t.substr(i, 12));
              i += 12;
            } else {
              out += "\\uFFFD";
              i += 6;
            }
          } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            out += "\\uFFFD";
            i += 6;
          } else {
            out.append(t.substr(i, 6));
            i += 6;
          }
        } else {
          out.append(t.substr(i, 2));
          i += 2;
        }
      } else {
        if (c == '"') inStr = false;
        out += c;
        ++i;
      }
      continue;
    }
    if (c == '"') { inStr = true; out += c; ++i; continue; }
    if (c != '-' && !isDigit(c)) { out += c; ++i; continue; }
    // number = -? (0 | [1-9]\d*) (. \d+)? ([eE] [+-]? \d+)?
    size_t j = i;
    if (t[j] == '-') ++j;
    if (j < t.size() && t[j] == '0') ++j;
    else if (j < t.size() && isDigit(t[j])) { while (j < t.size() && isDigit(t[j])) ++j; }
    else { out += c; ++i; continue; }
    if (j + 1 < t.size() && t[j] == '.' && isDigit(t[j + 1])) {
      j += 2;
      while (j < t.size() && isDigit(t[j])) ++j;
    }
    if (j < t.size() && (t[j] == 'e' || t[j] == 'E')) {
      size_t k = j + 1;
      if (k < t.size() && (t[k] == '+' || t[k] == '-')) ++k;
      if (k < t.size() && isDigit(t[k])) {
        while (k < t.size() && isDigit(t[k])) ++k;
        j = k;
      }
    }
    const std::string_view tok = t.substr(i, j - i);
    if (overflowsDouble(tok)) out += c == '-' ? "-1e300" : "1e300";
    else out.append(tok);
    i = j;
  }
  return out;
}

// typeof x === 'number'
bool isNumber(const nlohmann::json& j) { return j.is_number(); }

// Number.isSafeInteger(x)
std::optional<int64_t> safeInteger(const nlohmann::json& j) {
  if (j.is_number_unsigned()) {
    const auto u = j.get<uint64_t>();
    if (u > static_cast<uint64_t>(kMaxSafeInteger)) return std::nullopt;
    return static_cast<int64_t>(u);
  }
  if (j.is_number_integer()) {
    const auto s = j.get<int64_t>();
    if (s > kMaxSafeInteger || s < -kMaxSafeInteger) return std::nullopt;
    return s;
  }
  if (j.is_number_float()) {
    const double d = j.get<double>();
    if (!std::isfinite(d) || std::trunc(d) != d || std::fabs(d) > static_cast<double>(kMaxSafeInteger)) return std::nullopt;
    return static_cast<int64_t>(d);
  }
  return std::nullopt;
}

// payload.v === 1
bool isVersionOne(const nlohmann::json& j) {
  if (j.is_number_unsigned()) return j.get<uint64_t>() == 1;
  if (j.is_number_integer()) return j.get<int64_t>() == 1;
  return j.get<double>() == 1.0;
}

const std::string* stringField(const nlohmann::json& o, std::string_view k) {
  const auto it = o.find(k);
  if (it == o.end() || !it->is_string()) return nullptr;
  return it->get_ptr<const std::string*>();
}

bool sodiumReady() {
  static const bool ok = sodium_init() >= 0;
  return ok;
}
}  // namespace

std::string_view toString(TicketError e) {
  switch (e) {
    case TicketError::Malformed: return "malformed";
    case TicketError::BadVersion: return "bad_version";
    case TicketError::UnknownKid: return "unknown_kid";
    case TicketError::BadSignature: return "bad_signature";
    case TicketError::WrongAudience: return "wrong_audience";
    case TicketError::Expired: return "expired";
    case TicketError::NotYetValid: return "not_yet_valid";
  }
  return "malformed";
}

std::expected<TicketPayload, TicketError> verifyTicket(std::string_view ticket, const PublicKeys& keys,
                                                       std::string_view audience, int64_t nowSeconds) {
  using std::unexpected;
  // tamanho: antes de qualquer cópia (o Hello já limita a 2048; isto protege outros chamadores)
  if (ticket.size() > kMaxTicketBytes) return unexpected(TicketError::Malformed);
  // formato/charset
  const size_t dot = ticket.find('.');
  if (dot == std::string_view::npos || ticket.find('.', dot + 1) != std::string_view::npos)
    return unexpected(TicketError::Malformed);
  const std::string_view bodyText = ticket.substr(0, dot);
  const std::string_view sigText = ticket.substr(dot + 1);
  if (!isB64urlText(bodyText) || !isB64urlText(sigText)) return unexpected(TicketError::Malformed);
  // base64url canônico
  const auto body = base64urlDecodeCanonical(bodyText);
  const auto sig = base64urlDecodeCanonical(sigText);
  if (!body || !sig) return unexpected(TicketError::Malformed);
  // JSON: válido, compacto, sem chaves duplicadas
  const std::string text = decodeUtf8Lossy(*body);
  // JSON.parse não aceita BOM nem NUL cru em lugar nenhum; o nlohmann pula o BOM e trata NUL como fim da entrada.
  if (text.starts_with("\xEF\xBB\xBF") || text.find('\0') != std::string::npos) return unexpected(TicketError::Malformed);
  const auto json = nlohmann::json::parse(toParserInput(text), nullptr, /*allow_exceptions=*/false);
  if (json.is_discarded()) return unexpected(TicketError::Malformed);
  if (!isCompactJson(text) || !noDuplicateKeys(text)) return unexpected(TicketError::Malformed);
  // forma: tipos, inteiros seguros, literais inteiros simples
  if (!json.is_object()) return unexpected(TicketError::Malformed);
  const auto vIt = json.find("v");
  const auto iatIt = json.find("iat");
  const auto expIt = json.find("exp");
  if (vIt == json.end() || !isNumber(*vIt)) return unexpected(TicketError::Malformed);
  const std::string* fields[6] = {};
  const std::string_view names[6] = {"kid", "sub", "did", "name", "aud", "jti"};
  for (size_t i = 0; i < 6; ++i)
    if ((fields[i] = stringField(json, names[i])) == nullptr) return unexpected(TicketError::Malformed);
  if (iatIt == json.end() || expIt == json.end()) return unexpected(TicketError::Malformed);
  const auto iat = safeInteger(*iatIt);
  const auto exp = safeInteger(*expIt);
  if (!iat || !exp || !plainIntegerLiterals(text)) return unexpected(TicketError::Malformed);
  // versão
  if (!isVersionOne(*vIt)) return unexpected(TicketError::BadVersion);
  const std::string& kid = *fields[0];
  const std::string& aud = *fields[4];
  // kid
  const auto key = keys.find(kid);
  if (key == keys.end()) return unexpected(TicketError::UnknownKid);
  // assinatura
  if (sig->size() != crypto_sign_BYTES || !sodiumReady() ||
      crypto_sign_verify_detached(sig->data(), body->data(), body->size(), key->second.data()) != 0)
    return unexpected(TicketError::BadSignature);
  // audience
  if (aud != audience) return unexpected(TicketError::WrongAudience);
  // iat > now + skew (comparado como iat - skew > now para não estourar com now extremo)
  if (*iat - kClockSkewSeconds > nowSeconds) return unexpected(TicketError::NotYetValid);
  // exp + skew < now
  if (*exp + kClockSkewSeconds < nowSeconds) return unexpected(TicketError::Expired);
  return TicketPayload{1, kid, *fields[1], *fields[2], *fields[3], aud, *fields[5], *iat, *exp};
}

bool JtiCache::seenOrInsert(const std::string& jti, int64_t expiresAt, int64_t now) {
  const auto it = entries_.find(jti);
  if (it != entries_.end() && now <= it->second + kClockSkewSeconds) return true;
  if (it != entries_.end()) it->second = expiresAt;
  else entries_.emplace(jti, expiresAt);
  if (++insertsSincePurge_ >= 1024) purge(now);
  return false;
}

void JtiCache::purge(int64_t now) {
  insertsSincePurge_ = 0;
  std::erase_if(entries_, [now](const auto& e) { return now > e.second + kClockSkewSeconds; });
}
}  // namespace cbmp
