#pragma once
// JSON (UTF-8) ↔ valor Lua, para payloads de eventos (cliente ↔ servidor, banco, HTTP).
#include <cstddef>
#include <string>
#include <string_view>

struct lua_State;

namespace cbscript {

struct JsonOptions {
  size_t maxBytes = 4096;  // texto JSON maior é recusado (nos dois sentidos)
  int maxDepth = 32;       // contêineres aninhados ([ ou {); 32 passa, 33 é recusado
  // Referência no registro (luaL_ref) do sentinela de null (cb.null). Com ela, null ↔ sentinela; sem ela (-2 =
  // LUA_NOREF), null vira nil (some de tabelas) e nil no topo vira null.
  int nullRef = -2;
};

// Decodifica `json` e empilha 1 valor em L. Em erro devolve false, não empilha nada e preenche `err`.
// Pode ser chamada pelo host ou de dentro de uma função C chamada pelo script: a montagem da tabela roda sob
// lua_pcall, então erro de memória (teto do estado) vira `false`, nunca longjmp por quadros C++.
// Mapeamento: objeto → tabela com chaves texto; array → sequência 1..n; inteiro → integer; número com ponto/expoente
// → float; inteiro acima de int64 → float; texto → string (UTF-8 já validado); true/false → boolean.
bool jsonToLua(lua_State* L, std::string_view json, std::string& err, const JsonOptions& opt = {});

// Serializa o valor no índice `idx` como JSON compacto. Só acesso cru (sem metamétodos, nenhum código do script roda)
// e nada é alocado no estado Lua. Em erro devolve false e `out` fica indefinido.
// Mapeamento: nil/sentinela → null; boolean; integer → inteiro; float finito → número com '.' ou expoente (1.0 fica
// "1.0", então inteiro vs float sobrevive à ida e volta); NaN/inf → erro; string → texto (precisa ser UTF-8
// válido); tabela com chaves exatamente 1..n → array; tabela vazia → []; outras tabelas → objeto (chaves texto, e
// chaves inteiras viram texto decimal; outras chaves → erro); function/userdata/thread → erro. Ciclo → erro de
// profundidade.
bool luaToJson(lua_State* L, int idx, std::string& out, std::string& err, const JsonOptions& opt = {});

}  // namespace cbscript
