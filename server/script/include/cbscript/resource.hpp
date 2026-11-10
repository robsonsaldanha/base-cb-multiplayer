#pragma once
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <sol/sol.hpp>

namespace cbscript {

// Conteúdo validado de <resource>/resource.json.
struct ResourceManifest {
  std::string name, version;
  std::vector<std::string> serverScripts, dependencies;  // "server": [...], "dependencies": [...]
  nlohmann::json config = nlohmann::json::object();       // "config": livre (cb.config)
  std::vector<std::string> httpAllow;                     // "permissions": {"http": [...]}
  bool db = false;                                        // "permissions": {"db": true}
};

// Lê e valida <dir>/resource.json. Lança std::runtime_error (mensagem em PT) se: JSON inválido; nome fora de
// [A-Za-z0-9._-] (ou "."/".."/longo demais); script de "server" que não seja caminho seguro "server/....lua";
// dependência com nome inválido; "permissions" malformado.
ResourceManifest loadManifest(const std::filesystem::path& dir);

// Texto de script para uma linha de log (ver cbmp::escapeLogText): controles, separadores de linha Unicode e
// controles bidi viram escapes, para um script não forjar linhas no log do servidor.
std::string escapeLogText(std::string_view s);

struct Limits {
  size_t memoryBytes = size_t{256} << 20;
  std::chrono::milliseconds warnAfter{50}, abortAfter{5000};
};

// Um resource = um lua_State sandbox (ver resource.cpp para a lista exata do que fica exposto).
// Não é thread-safe: todo uso acontece na thread de scripts.
class Resource {
 public:
  // `warn` recebe avisos (vigia) e a saída de `print` do script; pode ser vazio.
  Resource(ResourceManifest m, std::filesystem::path dir, Limits lim, std::function<void(std::string)> warn);
  ~Resource();
  Resource(const Resource&) = delete;
  Resource& operator=(const Resource&) = delete;

  // Estado sandbox já preparado (globals = ambiente sandbox). Código do host deve usar acesso cru/refs no registro,
  // não lua()["x"] (que passa por metamétodos de _G controláveis pelo script).
  sol::state& lua();
  const ResourceManifest& manifest() const { return manifest_; }

  // Executa os serverScripts em ordem (cada um sob a vigia, como call()); o 1º valor retornado pelo último vira
  // exports(). Em erro: false e `err` = "[<nome>] ..." com traceback; `timedOut` (se dado) diz se foi aborto da vigia.
  bool runMain(std::string& err, bool* timedOut = nullptr);
  sol::object exports() const { return exports_; }

  // Tempo de vida: `value` e `thread` são referências no registro deste estado Lua; precisam ser destruídos (ou
  // reatribuídos) antes do Resource. Nunca guarde CallResult além da vida do Resource que o produziu.
  struct CallResult {
    bool ok = false;        // terminou (ou cedeu) sem erro
    bool timedOut = false;  // abortado pela vigia (ok=false)
    bool yielded = false;   // fn chamou coroutine.yield no nível de cima: retome com resume(thread, ...)
    std::string error;      // "[<nome>] <mensagem>\nstack traceback: ..." quando ok=false
    sol::object value;      // 1º valor retornado (ou cedido)
    sol::thread thread;     // a corrotina (válida quando yielded)
  };
  // Roda fn numa corrotina nova, com vigia (aviso após warnAfter, aborto após abortAfter) e limite de memória.
  CallResult call(const sol::reference& fn, std::span<const sol::object> args = {});
  template <class... A>
  CallResult callWith(const sol::reference& fn, A&&... a) {
    std::vector<sol::object> v{sol::make_object(lua_, std::forward<A>(a))...};
    return call(fn, v);
  }
  // Retoma uma corrotina que cedeu (CallResult::yielded); cada retomada tem seu próprio tempo de vigia.
  CallResult resume(const sol::thread& co, std::span<const sol::object> args = {});

  size_t memoryUsed() const;

  struct Shared;  // alocador + vigia (endereço estável; usado pelos callbacks C do Lua)

 private:
  CallResult resumeThread(lua_State* co, int nargs, sol::thread th);
  int loadFile(lua_State* L, const std::string& rel, std::string& err);  // empilha o chunk (modo "t")

  ResourceManifest manifest_;
  std::filesystem::path dir_;
  std::unique_ptr<Shared> sh_;  // antes de lua_: destruído depois do estado
  sol::state lua_;
  sol::object exports_;

  friend int luaRequire(lua_State* L);
  friend int prepareModule(Resource* self, lua_State* L);
};

}  // namespace cbscript
