#include "cbscript/json_lua.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Lua compilado como C++ (vcpkg-overlays/lua, feature cpp): cabeçalhos sem extern "C".
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
#include <nlohmann/json.hpp>

// Decisões (ver também json_lua.hpp):
// - JSON → Lua: o nlohmann valida a sintaxe e o UTF-8; o resultado é achatado numa "fita" de nós simples e só então
//   a tabela é montada, dentro de lua_pcall, por uma função C cujo quadro só tem tipos triviais. Um erro de memória
//   (teto do estado durante uma chamada do script) vira um retorno false, sem erro do Lua atravessando quadros C++ com destrutor.
// - Lua → JSON: só leitura crua (lua_next/lua_rawgeti), nada é alocado no estado e nenhuma API usada aqui lança erro
//   Lua, então nenhum erro do Lua sai desta função; exceções C++ (bad_alloc) são capturadas.
// - O tamanho é conferido durante a escrita (para cedo), e tabelas com mais entradas que bytes permitidos são
//   recusadas sem varrer o resto.

namespace cbscript {

namespace {

using json = nlohmann::json;

enum class Kind : uint8_t { Null, False, True, Int, Float, Str, Arr, Obj };

struct Node {
  Kind kind = Kind::Null;
  uint32_t count = 0;  // Arr/Obj: filhos diretos (Obj: pares chave+valor, chave em `s`)
  int64_t i = 0;
  double d = 0;
  const char* s = nullptr;  // Str: texto; filho de Obj: a chave fica no próprio nó do valor (ks/klen)
  size_t len = 0;
  const char* ks = nullptr;
  size_t klen = 0;
};

bool tooDeep(const json& j, int depth, int maxDepth) {
  if (!j.is_structured()) return false;
  if (depth + 1 > maxDepth) return true;
  for (const auto& e : j)
    if (tooDeep(e, depth + 1, maxDepth)) return true;
  return false;
}

// Número que não cabe em double (ex.: 1E400 vira inf no parser).
bool nonFinite(const json& j) {
  if (j.is_number_float()) return !std::isfinite(j.get<double>());
  if (!j.is_structured()) return false;
  for (const auto& e : j)
    if (nonFinite(e)) return true;
  return false;
}

void flatten(const json& j, std::vector<Node>& tape, const char* key, size_t klen) {
  Node n;
  n.ks = key;
  n.klen = klen;
  switch (j.type()) {
    case json::value_t::boolean:
      n.kind = j.get<bool>() ? Kind::True : Kind::False;
      break;
    case json::value_t::number_integer:
      n.kind = Kind::Int;
      n.i = j.get<json::number_integer_t>();
      break;
    case json::value_t::number_unsigned: {
      auto u = j.get<json::number_unsigned_t>();
      if (u <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
        n.kind = Kind::Int;
        n.i = static_cast<int64_t>(u);
      } else {
        n.kind = Kind::Float;
        n.d = static_cast<double>(u);
      }
      break;
    }
    case json::value_t::number_float:
      n.kind = Kind::Float;
      n.d = j.get<double>();
      break;
    case json::value_t::string: {
      const auto& s = j.get_ref<const json::string_t&>();
      n.kind = Kind::Str;
      n.s = s.data();
      n.len = s.size();
      break;
    }
    case json::value_t::array:
      n.kind = Kind::Arr;
      n.count = static_cast<uint32_t>(j.size());
      tape.push_back(n);
      for (const auto& e : j) flatten(e, tape, nullptr, 0);
      return;
    case json::value_t::object:
      n.kind = Kind::Obj;
      n.count = static_cast<uint32_t>(j.size());
      tape.push_back(n);
      for (auto it = j.begin(); it != j.end(); ++it) flatten(it.value(), tape, it.key().data(), it.key().size());
      return;
    default:
      n.kind = Kind::Null;
      break;
  }
  tape.push_back(n);
}

struct PushCtx {
  const Node* tape;
  size_t pos;
  int nullRef;
};

// Quadros só com tipos triviais: o Lua pode sair daqui com erro de memória (exceção do Lua).
void pushNode(lua_State* L, PushCtx* c) {
  const Node& n = c->tape[c->pos++];
  luaL_checkstack(L, 3, "json");
  switch (n.kind) {
    case Kind::Null:
      if (c->nullRef != LUA_NOREF)
        lua_rawgeti(L, LUA_REGISTRYINDEX, c->nullRef);
      else
        lua_pushnil(L);
      return;
    case Kind::False:
      lua_pushboolean(L, 0);
      return;
    case Kind::True:
      lua_pushboolean(L, 1);
      return;
    case Kind::Int:
      lua_pushinteger(L, static_cast<lua_Integer>(n.i));
      return;
    case Kind::Float:
      lua_pushnumber(L, static_cast<lua_Number>(n.d));
      return;
    case Kind::Str:
      lua_pushlstring(L, n.s, n.len);
      return;
    case Kind::Arr:
      lua_createtable(L, static_cast<int>(n.count), 0);
      for (uint32_t k = 0; k < n.count; ++k) {
        pushNode(L, c);
        if (lua_isnil(L, -1))
          lua_pop(L, 1);  // null sem sentinela: buraco
        else
          lua_rawseti(L, -2, static_cast<lua_Integer>(k) + 1);
      }
      return;
    case Kind::Obj:
      lua_createtable(L, 0, static_cast<int>(n.count));
      for (uint32_t k = 0; k < n.count; ++k) {
        const Node& child = c->tape[c->pos];
        lua_pushlstring(L, child.ks, child.klen);
        pushNode(L, c);
        if (lua_isnil(L, -1))
          lua_pop(L, 2);
        else
          lua_rawset(L, -3);
      }
      return;
  }
}

int pushProtected(lua_State* L) {
  auto* c = static_cast<PushCtx*>(lua_touserdata(L, 1));
  pushNode(L, c);
  return 1;
}

// ---- Lua → JSON ----

bool validUtf8(const char* p, size_t n) {
  const auto* s = reinterpret_cast<const unsigned char*>(p);
  size_t i = 0;
  while (i < n) {
    unsigned char c = s[i];
    if (c < 0x80) {
      ++i;
      continue;
    }
    size_t len;
    uint32_t cp;
    if (c >= 0xC2 && c <= 0xDF) {
      len = 2;
      cp = c & 0x1Fu;
    } else if (c >= 0xE0 && c <= 0xEF) {
      len = 3;
      cp = c & 0x0Fu;
    } else if (c >= 0xF0 && c <= 0xF4) {
      len = 4;
      cp = c & 0x07u;
    } else {
      return false;
    }
    if (n - i < len) return false;
    for (size_t k = 1; k < len; ++k) {
      if ((s[i + k] & 0xC0) != 0x80) return false;
      cp = (cp << 6) | (s[i + k] & 0x3Fu);
    }
    if ((len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000) || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
      return false;
    i += len;
  }
  return true;
}

struct Enc {
  lua_State* L;
  std::string& out;
  std::string& err;
  const JsonOptions& o;
  int nullIdx;  // índice absoluto do sentinela na pilha, ou 0
  // Cada tabela é varrida no máximo uma vez por chamada: revarrer uma tabela compartilhada faria lua_next percorrer
  // de novo todas as posições vazias da parte de array (milhões por visita, numa única chamada C sem vigia).
  // Tabela já serializada → o trecho de `out` é copiado (memo); tabela ainda aberta na pilha → ciclo → erro.
  struct Memo {
    size_t off, len;
    int height;  // níveis de contêiner do próprio valor (para o limite de profundidade no novo lugar)
  };
  std::unordered_map<const void*, Memo> done;
  std::unordered_set<const void*> open;
  int lastHeight = 0;  // altura do último valor escrito (0 = escalar)

  bool over() {
    if (out.size() <= o.maxBytes) return false;
    err = "JSON passa de " + std::to_string(o.maxBytes) + " bytes";
    return true;
  }

  bool str(const char* p, size_t n) {
    if (n > o.maxBytes || n + 2 > o.maxBytes - std::min(out.size(), o.maxBytes)) {
      err = "JSON passa de " + std::to_string(o.maxBytes) + " bytes";
      return false;
    }
    if (!validUtf8(p, n)) {
      err = "texto não é UTF-8 válido";
      return false;
    }
    out.push_back('"');
    for (size_t k = 0; k < n; ++k) {
      unsigned char c = static_cast<unsigned char>(p[k]);
      switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
          if (c < 0x20) {
            static const char hex[] = "0123456789abcdef";
            out += "\\u00";
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0xF]);
          } else {
            out.push_back(static_cast<char>(c));
          }
      }
    }
    out.push_back('"');
    return !over();
  }

  void integer(lua_Integer v) {
    char buf[32];
    auto r = std::to_chars(buf, buf + sizeof buf, static_cast<long long>(v));
    out.append(buf, r.ptr);
  }

  bool number(int idx) {
    if (lua_isinteger(L, idx)) {
      integer(lua_tointegerx(L, idx, nullptr));
      return !over();
    }
    double d = static_cast<double>(lua_tonumberx(L, idx, nullptr));
    if (!std::isfinite(d)) {
      err = "número não finito (NaN/inf) não cabe em JSON";
      return false;
    }
    char buf[64];
    auto r = std::to_chars(buf, buf + sizeof buf, d);
    std::string_view sv(buf, static_cast<size_t>(r.ptr - buf));
    out.append(sv);
    if (sv.find_first_of(".eE") == std::string_view::npos) out += ".0";  // float continua float na volta
    return !over();
  }

  bool value(int idx, int depth) {
    lastHeight = 0;
    // Sentinela de null (cb.null é lightuserdata; qualquer tipo serve).
    if (nullIdx != 0 && lua_type(L, idx) != LUA_TNIL && lua_rawequal(L, idx, nullIdx)) {
      out += "null";
      return !over();
    }
    switch (lua_type(L, idx)) {
      case LUA_TNIL:
        out += "null";
        return !over();
      case LUA_TBOOLEAN:
        out += lua_toboolean(L, idx) ? "true" : "false";
        return !over();
      case LUA_TNUMBER:
        return number(idx);
      case LUA_TSTRING: {
        size_t n = 0;
        const char* p = lua_tolstring(L, idx, &n);
        return str(p, n);
      }
      case LUA_TTABLE:
        return table(idx, depth + 1);
      default:
        err = std::string("tipo não suportado em JSON: ") + lua_typename(L, lua_type(L, idx));
        return false;
    }
  }

  bool table(int idx, int depth) {
    if (depth > o.maxDepth) {
      err = "profundidade máxima (" + std::to_string(o.maxDepth) + ") excedida";
      return false;
    }
    const void* self = lua_topointer(L, idx);
    if (open.count(self)) {
      err = "ciclo de tabelas (uma tabela contém a si mesma)";
      return false;
    }
    if (auto m = done.find(self); m != done.end()) {
      const Memo mm = m->second;
      if (depth + mm.height - 1 > o.maxDepth) {
        err = "profundidade máxima (" + std::to_string(o.maxDepth) + ") excedida";
        return false;
      }
      if (mm.len > o.maxBytes - std::min(out.size(), o.maxBytes)) {
        err = "JSON passa de " + std::to_string(o.maxBytes) + " bytes";
        return false;
      }
      const std::string copy = out.substr(mm.off, mm.len);
      out += copy;
      lastHeight = mm.height;
      return !over();
    }
    open.insert(self);
    const size_t start = out.size();
    int h = 0;
    auto finish = [&] {
      open.erase(self);
      done[self] = Memo{start, out.size() - start, h + 1};
      lastHeight = h + 1;
      return !over();
    };
    if (!lua_checkstack(L, 4)) {
      err = "pilha do Lua esgotada";
      return false;
    }
    // 1ª passada: forma da tabela (sequência 1..n ou objeto) e tipos das chaves.
    size_t count = 0;
    lua_Integer maxK = 0;
    bool seq = true;
    lua_pushnil(L);
    while (lua_next(L, idx) != 0) {
      lua_pop(L, 1);  // valor
      if (++count > o.maxBytes) {
        lua_pop(L, 1);
        err = "JSON passa de " + std::to_string(o.maxBytes) + " bytes";
        return false;
      }
      int kt = lua_type(L, -1);
      if (kt == LUA_TNUMBER && lua_isinteger(L, -1)) {
        lua_Integer k = lua_tointegerx(L, -1, nullptr);
        if (k < 1)
          seq = false;
        else if (k > maxK)
          maxK = k;
      } else if (kt == LUA_TSTRING) {
        seq = false;
      } else {
        const char* tn = kt == LUA_TNUMBER ? "número não inteiro" : lua_typename(L, kt);
        lua_pop(L, 1);
        err = std::string("chave de tabela não suportada em JSON: ") + tn;
        return false;
      }
    }
    if (seq && static_cast<size_t>(maxK) == count) {
      out.push_back('[');
      for (size_t k = 1; k <= count; ++k) {
        if (k > 1) out.push_back(',');
        lua_rawgeti(L, idx, static_cast<lua_Integer>(k));
        bool ok = value(lua_gettop(L), depth);
        h = std::max(h, lastHeight);
        lua_pop(L, 1);
        if (!ok) return false;
      }
      out.push_back(']');
      return finish();
    }
    out.push_back('{');
    bool first = true;
    lua_pushnil(L);
    while (lua_next(L, idx) != 0) {
      if (!first) out.push_back(',');
      first = false;
      bool ok;
      if (lua_type(L, -2) == LUA_TSTRING) {
        size_t n = 0;
        const char* p = lua_tolstring(L, -2, &n);
        ok = str(p, n);
      } else {  // inteiro (validado na 1ª passada); nunca lua_tolstring numa chave numérica (quebra o lua_next)
        out.push_back('"');
        integer(lua_tointegerx(L, -2, nullptr));
        out.push_back('"');
        ok = !over();
      }
      if (ok) {
        out.push_back(':');
        ok = value(lua_gettop(L), depth);
        h = std::max(h, lastHeight);
      }
      lua_pop(L, 1);
      if (!ok) {
        lua_pop(L, 1);
        return false;
      }
    }
    out.push_back('}');
    return finish();
  }
};

}  // namespace

bool jsonToLua(lua_State* L, std::string_view text, std::string& err, const JsonOptions& opt) {
  try {
    if (text.size() > opt.maxBytes) {
      err = "JSON passa de " + std::to_string(opt.maxBytes) + " bytes";
      return false;
    }
    json j;
    try {
      j = json::parse(text.begin(), text.end());
    } catch (const json::out_of_range&) {  // 406: número que estoura o double (1E400)
      err = "número fora do intervalo (não cabe em double)";
      return false;
    } catch (const json::exception&) {
      err = "JSON malformado (ou texto não UTF-8)";
      return false;
    }
    if (tooDeep(j, 0, opt.maxDepth)) {
      err = "profundidade máxima (" + std::to_string(opt.maxDepth) + ") excedida";
      return false;
    }
    if (nonFinite(j)) {
      err = "número fora do intervalo (não cabe em double)";
      return false;
    }
    std::vector<Node> tape;
    flatten(j, tape, nullptr, 0);
    if (!lua_checkstack(L, 4)) {
      err = "pilha do Lua esgotada";
      return false;
    }
    PushCtx ctx{tape.data(), 0, opt.nullRef};
    lua_pushcfunction(L, pushProtected);
    lua_pushlightuserdata(L, &ctx);
    const int st = lua_pcall(L, 1, 1, 0);
    if (st != LUA_OK) {
      const char* m = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "erro não textual";
      if (st == LUA_ERRMEM)
        err = std::string("sem memória para montar o valor JSON (") + m + ")";
      else  // p.ex. aborto da vigia (tempo esgotado) chegando durante a montagem
        err = std::string("falha ao montar o valor JSON: ") + m;
      lua_pop(L, 1);
      return false;
    }
    return true;
  } catch (const std::exception& e) {
    err = std::string("falha ao decodificar JSON: ") + e.what();
    return false;
  }
}

bool luaToJson(lua_State* L, int idx, std::string& out, std::string& err, const JsonOptions& opt) {
  const int base = lua_gettop(L);
  try {
    out.clear();
    idx = lua_absindex(L, idx);
    int nullIdx = 0;
    if (opt.nullRef != LUA_NOREF) {
      if (!lua_checkstack(L, 1)) {
        err = "pilha do Lua esgotada";
        return false;
      }
      lua_rawgeti(L, LUA_REGISTRYINDEX, opt.nullRef);
      nullIdx = lua_gettop(L);
    }
    Enc e{L, out, err, opt, nullIdx, {}, {}, 0};
    bool ok = e.value(idx, 0);
    lua_settop(L, base);
    return ok;
  } catch (const std::exception& e) {
    lua_settop(L, base);
    err = std::string("falha ao gerar JSON: ") + e.what();
    return false;
  }
}

}  // namespace cbscript
