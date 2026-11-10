#pragma once
// Assinatura de tickets nos testes: mesmo formato do Hub (JSON compacto na ordem v,kid,sub,did,name,aud,iat,exp,jti[,nonce]).
#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include <sodium.h>

#include <cbmp/base64url.hpp>
#include <cbmp/ticket.hpp>

namespace testing_ticket {

struct DevSigner {
  std::array<uint8_t, crypto_sign_PUBLICKEYBYTES> pk{};
  std::array<uint8_t, crypto_sign_SECRETKEYBYTES> sk{};

  DevSigner() {
    if (sodium_init() < 0) throw std::runtime_error("sodium_init");
    std::array<uint8_t, crypto_sign_SEEDBYTES> seed{};
    for (size_t i = 0; i < seed.size(); ++i) seed[i] = static_cast<uint8_t>(0xA0 + i);
    crypto_sign_seed_keypair(pk.data(), sk.data(), seed.data());
  }

  std::string publicKeyB64() const { return cbmp::base64urlEncode(std::vector<uint8_t>(pk.begin(), pk.end())); }
  cbmp::PublicKeys keys(const std::string& kid = "dev") const { return {{kid, pk}}; }

  std::string sign(const std::string& sub, const std::string& jti, int64_t now, const std::string& aud = "srv_dev",
                   const std::string& kid = "dev", const std::string& name = "Jogador") const {
    return signDid(sub, "did-" + sub, jti, now, aud, kid, name);
  }

  // Igual a sign, com o did explícito (tickets de admin: did = id Discord). `nonce` não vazio vira o claim opcional
  // "nonce" no fim do JSON (igual ao Hub com state).
  std::string signDid(const std::string& sub, const std::string& did, const std::string& jti, int64_t now,
                      const std::string& aud, const std::string& kid = "dev", const std::string& name = "Jogador",
                      const std::string& nonce = "") const {
    const std::string json = R"({"v":1,"kid":")" + kid + R"(","sub":")" + sub + R"(","did":")" + did +
                             R"(","name":")" + name + R"(","aud":")" + aud + R"(","iat":)" + std::to_string(now) +
                             R"(,"exp":)" + std::to_string(now + 300) + R"(,"jti":")" + jti + R"(")" +
                             (nonce.empty() ? std::string() : R"(,"nonce":")" + nonce + R"(")") + "}";
    const std::vector<uint8_t> body(json.begin(), json.end());
    std::array<uint8_t, crypto_sign_BYTES> sig{};
    crypto_sign_detached(sig.data(), nullptr, body.data(), body.size(), sk.data());
    return cbmp::base64urlEncode(body) + "." + cbmp::base64urlEncode(std::vector<uint8_t>(sig.begin(), sig.end()));
  }
};

}  // namespace testing_ticket
