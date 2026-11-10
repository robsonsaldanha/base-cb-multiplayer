#pragma once
#include <cstdint>
#include <expected>
#include <functional>
#include <string>
#include <string_view>

#include <cbmp/ticket.hpp>

namespace cbserver {

// Fonte das chaves públicas que assinam tickets (Hub ou chave dev).
class KeyProvider {
 public:
  virtual ~KeyProvider();
  virtual cbmp::PublicKeys keys() = 0;
  // Pede recarga (kid desconhecido). O provider limita a frequência (Hub: no máximo 1x/min).
  virtual void requestRefresh() = 0;
};

// Chaves fixas (modo dev / testes). requestRefresh não faz nada.
class StaticKeys : public KeyProvider {
 public:
  explicit StaticKeys(cbmp::PublicKeys keys) : keys_(std::move(keys)) {}
  cbmp::PublicKeys keys() override { return keys_; }
  void requestRefresh() override {}

  static constexpr const char* kDevKid = "dev";
  // auth.devPublicKey (base64url canônico de 32 bytes) → {"dev": chave}. Lança std::runtime_error se inválida.
  static cbmp::PublicKeys fromDevKey(std::string_view b64);

 private:
  cbmp::PublicKeys keys_;
};

// Valida o ticket do Hello (verifyTicket + jti não reutilizado). Não é thread-safe (JtiCache): uso só na thread
// da simulação.
class Authenticator {
 public:
  Authenticator(KeyProvider& keys, std::string serverId, std::function<int64_t()> nowSec);
  // Erro = razão em texto para o Reject.
  std::expected<cbmp::TicketPayload, std::string> check(std::string_view ticket);

 private:
  KeyProvider& keys_;
  std::string serverId_;
  std::function<int64_t()> nowSec_;
  cbmp::JtiCache jtis_;
};

}  // namespace cbserver
