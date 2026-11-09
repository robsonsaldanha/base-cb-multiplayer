// Alvo de fuzz do decodificador (libFuzzer; Linux/clang com CBMP_BUILD_FUZZ=ON, preset linux-fuzz).
// Regra: decode de bytes arbitrários só pode lançar DecodeError; o que decodifica tem de reencodar, decodificar de
// novo com o mesmo tipo, e o codec tem de ser estável (encode(decode(encode(m))) == encode(m)).
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <vector>

#include <cbmp/messages.hpp>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  cbmp::Message m;
  try {
    m = cbmp::decode(std::span<const uint8_t>(data, size));
  } catch (const cbmp::DecodeError&) {
    return 0;
  }
  // Daqui em diante qualquer exceção (encode recusando o que o decode aceitou) é um achado: deixa escapar.
  const std::vector<uint8_t> once = cbmp::encode(m);
  const cbmp::Message again = cbmp::decode(once);
  if (cbmp::typeOf(again) != cbmp::typeOf(m)) std::abort();
  if (cbmp::encode(again) != once) std::abort();
  return 0;
}
