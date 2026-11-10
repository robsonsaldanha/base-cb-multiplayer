// Exemplos de docs/RESOURCES.md: todo bloco ```lua compila (modo texto), todo bloco ```json é JSON válido (os que têm
// "name" passam por loadManifest), e os blocos marcados sobem num ScriptHost real com a API e um banco falso:
//   -- resource: <nome>          o bloco é o server/main.lua do resource <nome>
//   -- manifest: {...}           (2ª linha, opcional) campos extras do resource.json (dependencies, config, permissions)
//   -- arquivo: <nome>/<caminho> o bloco é outro arquivo do resource (ex. textos/server/util/texto.lua)
#include "api_env.hpp"
#include "fake_db.hpp"

#include <map>
#include <set>

#include "cbscript/db.hpp"

#ifndef CBMP_RESOURCES_DOC
#error "CBMP_RESOURCES_DOC (caminho de docs/RESOURCES.md) precisa ser definido pelo CMake"
#endif

namespace {

struct Block {
  std::string lang;
  int line = 0;  // linha do primeiro conteúdo (1-based)
  std::string code;
};

std::vector<Block> docBlocks() {
  std::ifstream f(CBMP_RESOURCES_DOC, std::ios::binary);
  REQUIRE(f.good());
  std::vector<Block> out;
  std::string line;
  int n = 0;
  Block* cur = nullptr;
  while (std::getline(f, line)) {
    ++n;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (cur == nullptr) {
      if (line.rfind("```", 0) == 0) {  // abre (a linguagem pode faltar)
        out.push_back(Block{line.substr(3), n + 1, {}});
        cur = &out.back();
      }
    } else if (line == "```") {
      cur = nullptr;
    } else {
      cur->code += line + "\n";
    }
  }
  REQUIRE(cur == nullptr);  // bloco sem fechamento
  return out;
}

// Valor depois de "-- <tag>: " numa linha; vazio se a linha não for a tag.
std::string tagValue(const std::string& code, size_t lineIdx, const std::string& tag) {
  size_t b = 0;
  for (size_t i = 0; i < lineIdx; ++i) {
    b = code.find('\n', b);
    if (b == std::string::npos) return {};
    ++b;
  }
  const size_t e = code.find('\n', b);
  const std::string l = code.substr(b, e == std::string::npos ? std::string::npos : e - b);
  const std::string prefix = "-- " + tag + ": ";
  return l.rfind(prefix, 0) == 0 ? l.substr(prefix.size()) : std::string{};
}

}  // namespace

TEST_CASE("docs: every lua block of RESOURCES.md compiles and every json block parses") {
  const auto blocks = docBlocks();
  int lua = 0, json = 0, manifests = 0;
  for (const auto& b : blocks) {
    INFO("RESOURCES.md:" << b.line << " (" << b.lang << ")");
    if (b.lang == "lua") {
      ++lua;
      lua_State* L = luaL_newstate();
      REQUIRE(L != nullptr);
      const std::string name = "=RESOURCES.md:" + std::to_string(b.line);
      const int st = luaL_loadbufferx(L, b.code.data(), b.code.size(), name.c_str(), "t");
      std::string err = st == LUA_OK ? std::string() : std::string(lua_tostring(L, -1));
      lua_close(L);
      INFO(err);
      CHECK(st == LUA_OK);
    } else if (b.lang == "json") {
      ++json;
      auto j = nlohmann::json::parse(b.code, nullptr, false);
      REQUIRE_FALSE(j.is_discarded());
      if (j.is_object() && j.contains("name")) {
        ++manifests;
        TmpDir d;
        put(d.p / "resource.json", b.code);
        CHECK_NOTHROW(cbscript::loadManifest(d.p));
      }
    }
  }
  CHECK(lua >= 15);
  CHECK(json >= 1);
  CHECK(manifests >= 1);
}

TEST_CASE("docs: the resource examples of RESOURCES.md start and behave as described") {
  struct Res {
    std::string code;
    nlohmann::json extra = nlohmann::json::object();
  };
  std::map<std::string, Res> res;
  std::vector<std::pair<std::string, std::string>> files;  // caminho relativo a res/, conteúdo
  for (const auto& b : docBlocks()) {
    if (b.lang != "lua") continue;
    if (auto name = tagValue(b.code, 0, "resource"); !name.empty()) {
      INFO("RESOURCES.md:" << b.line);
      REQUIRE_FALSE(res.count(name));
      Res r{b.code};
      if (auto m = tagValue(b.code, 1, "manifest"); !m.empty()) {
        r.extra = nlohmann::json::parse(m, nullptr, false);
        REQUIRE(r.extra.is_object());
      }
      res.emplace(name, std::move(r));
    } else if (auto path = tagValue(b.code, 0, "arquivo"); !path.empty()) {
      files.emplace_back(path, b.code);
    }
  }
  // Os exemplos que o guia descreve com comportamento conferido abaixo.
  for (const char* n : {"ola", "textos", "saudacao", "feiticos", "missoes", "comandos", "relogio", "loja", "economia",
                        "recompensa", "perfis", "banco-galeoes", "avisos-discord"})
    REQUIRE(res.count(n));

  struct DocEnv : ApiEnv {
    std::shared_ptr<FakeDb> fdb = std::make_shared<FakeDb>();
    DocEnv() {
      fdb->handler = [](const DbRequest& r) {
        DbResult out;
        out.ok = true;
        if (r.sql.rfind("SELECT visitas", 0) == 0) {
          out.columns = {"visitas", "apelido"};
          out.rows.push_back({DbValue::integer(10), DbValue::null()});
        }
        return out;
      };
      api.db = std::make_shared<DbPool>(fakeCfg(2), fakeFactory(fdb), fastPoolOpt());
    }
    ~DocEnv() {
      fdb->release();
      if (host)
        guarded("destroy", [this] {
          host.reset();
          return 0;
        });
    }
  } env;
  for (auto& [name, r] : res) {
    std::vector<std::string> deps;
    if (r.extra.contains("dependencies")) deps = r.extra["dependencies"].get<std::vector<std::string>>();
    env.addRes(name, r.code, deps, r.extra.value("config", nlohmann::json::object()),
               r.extra.contains("permissions") ? r.extra["permissions"] : nlohmann::json(nullptr));
  }
  for (auto& [path, code] : files) put(env.dir.p / "res" / path, code);
  env.boot();

  for (auto& [name, r] : res) {
    INFO(name << "\n" << env.allLogs());
    CHECK(env.state(name) == State::Running);
  }
  // Script principal e resourceStart: textos usou o módulo via require; loja leu cb.config; perfis criou a tabela.
  CHECK(env.waitLog("[textos] Olá, mundo"));
  CHECK(env.waitLog("[loja] à venda: varinha 100"));
  CHECK(waitUntil([&] { return env.fdb->count("CREATE TABLE IF NOT EXISTS perfis") == 1; }));

  // Entrada de jogador: boas-vindas (ola), crédito via export (recompensa → economia), perfil do banco (perfis).
  env.join(7, "ana");
  auto emitted = [&](const std::string& ev, const std::string& sub) {
    return env.waitCmd<CmdEmit>([&](const CmdEmit& c) { return c.name == ev && contains(c.json, sub); }) != nullptr;
  };
  CHECK(emitted("boasVindas", "Bem-vindo, ana!"));
  CHECK(emitted("saldo", "{\"valor\":10}"));
  CHECK(emitted("perfil", "\"bonus\":true"));
  CHECK(emitted("perfil", "\"apelido\":\"ana\""));
  CHECK(env.waitLog("[ola] ana entrou (id 7)"));

  // Evento de cliente validado (feiticos) e evento local adiado (missoes).
  env.event(7, "lancarFeitico", R"({"nome":"lumos"})");
  env.event(7, "lancarFeitico", R"({"nome":"avada"})");
  CHECK(emitted("feitico", "\"nome\":\"lumos\""));
  env.command(std::nullopt, "concluir ovo");
  CHECK(waitUntil([&] { return env.hasReply("só jogadores"); }));

  // Comandos com resposta.
  env.command(std::nullopt, "dado 20");
  env.command(std::nullopt, "dado 1");
  env.command(std::nullopt, "comprar varinha");
  CHECK(waitUntil([&] { return env.hasReply("uso: /dado [2..1000]"); }));
  CHECK(waitUntil([&] { return env.hasReply("comprou varinha por 100 galeoes"); }));
  CHECK(waitUntil([&] {
    std::lock_guard<std::mutex> lk(env.mu);
    for (auto& s : env.replies)
      if (s.rfind("dado: ", 0) == 0 && contains(s, "(de 20)")) return true;
    return false;
  }));

  env.leave(7, "saiu");
  CHECK(env.waitLog("[ola] ana saiu: saiu"));
  env.sync();
  {
    INFO(env.allLogs());
    std::lock_guard<std::mutex> lk(env.mu);
    for (auto& [lv, m] : env.logs) CHECK((lv != LogLevel::Error && lv != LogLevel::Critical));
  }
  CHECK_FALSE(env.hasReply("erro ao executar 'dado' (ver log)"));
}
