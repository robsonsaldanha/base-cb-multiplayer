#pragma once
// HTTP de saída dos scripts (cb.http; seção "API cb.*" da spec e Global Constraints).
//
// HttpOutPool: threads próprias (padrão 4) que fazem os pedidos. Quem pede (thread de scripts) nunca espera:
// submit() valida e enfileira; `done` é chamado depois, numa thread do pool.
//
// Regras (fora do modo de teste):
// - só https; certificado e nome do host sempre verificados (CAs do sistema: repositório ROOT/CA do Windows, caminhos
//   padrão do OpenSSL no Linux; HttpOutOptions::caFile troca por um arquivo PEM);
// - host exato da lista do resource (permissions.http): só nomes (nada de IP literal na URL nem na lista), minúsculas,
//   sem curinga; porta 443, ou outra se a entrada da lista for "host:porta";
// - DNS resolvido uma vez; TODOS os endereços precisam ser públicos (httpAddressRefusal) e a conexão vai para um
//   desses endereços já conferidos (SNI e Host = o nome original; sem nova resolução → sem DNS rebinding);
// - redirecionamentos: até 3, cada um com as mesmas regras (https, lista, endereços); https → http nunca;
// - pedido: só GET/POST; URL ≤ 2048 bytes; cabeçalhos sem CR/LF/NUL, sem Host/Content-Length/Transfer-Encoding/
//   Connection (e outros controlados pelo pool), ≤ 8 KiB no total; corpo ≤ 1 MiB;
// - resposta: corpo ≤ 1 MiB (a leitura para quando passa → erro), cabeçalhos ≤ 64 KiB, prazo total de 10 s (conexão +
//   TLS + envio + leitura); sem compressão (Accept-Encoding: identity);
// - por dono (resource): até 64 pedidos no pool e 4 MiB de corpos (pedido + resposta recebida) em andamento; a fila é
//   repartida em rodízio entre os donos (a vez de outro dono vem antes do 2º pedido de quem acabou de ser atendido);
//   espera máxima na fila: queueTimeout.
// - DNS: endereços IPv4 são tentados antes dos IPv6; com mais endereços na reserva, cada conexão tem no máximo 3 s.
// - logs: nunca corpo nem cabeçalhos; a URL vai sem a query (httpUrlForLog).
//
// Modo de teste (só pelo construtor com InsecureLoopbackForTests): aceita http:// e endereços de loopback
// (127.0.0.0/8, ::1). Todo o resto (lista, IPs privados, limites) continua valendo.
//
// Encerramento: o destrutor falha os pedidos na fila, fecha os sockets dos que estão em andamento e espera as threads
// por `shutdownWait`; uma thread presa onde não dá para cancelar (getaddrinfo) é desanexada e termina sozinha depois
// (o estado é compartilhado), sem chamar `done` nem registrar log.
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cbscript {

using HttpHeaders = std::vector<std::pair<std::string, std::string>>;

struct HttpUrl {
  std::string scheme;  // "https" (ou "http" no modo de teste)
  std::string host;    // minúsculas
  int port = 443;
  bool explicitPort = false;
  std::string target = "/";  // caminho + query (sem o fragmento)
  std::string forLog() const;  // scheme://host[:porta]/caminho (sem a query)
};

// Analisa a URL. `allowHttp` só no modo de teste. Recusa: esquema que não seja https, usuário/senha, IP literal
// (inclusive formas numéricas como 2130706433 ou 0x7f.1), host fora de [a-z0-9.-] (IDN só em punycode), porta
// inválida, caracteres de controle/espaço/não ASCII, mais de 2048 bytes.
bool parseHttpUrl(std::string_view url, HttpUrl& out, std::string& err, bool allowHttp = false);

// Entrada de permissions.http: "host" ou "host:porta". Só minúsculas, sem curinga, sem IP, sem esquema/caminho.
bool parseHttpAllowEntry(std::string_view entry, std::string& normalized, std::string& err);

// A URL bate com a lista? "host" vale só para a porta 443 (ou 80 em http no modo de teste); "host:porta" só para ela.
bool httpAllowed(const std::vector<std::string>& allow, const HttpUrl& u);

// Destino de um redirecionamento (Location absoluto, "//host/..." ou relativo) a partir de `base`. Recusa https → http.
bool resolveHttpRedirect(const HttpUrl& base, std::string_view location, HttpUrl& out, std::string& err,
                         bool allowHttp = false);

// Endereço IP em texto: "" se for público; senão o motivo (loopback, privado, link-local, multicast, reservado,
// documentação, IPv6 com IPv4 embutido proibido...). `allowLoopback` só no modo de teste. Texto que não é IP → motivo.
std::string httpAddressRefusal(std::string_view ip, bool allowLoopback = false);

// URL sem query nem fragmento (para logs e mensagens de erro).
std::string httpUrlForLog(std::string_view url);

enum class HttpMethod : uint8_t { Get, Post };

struct HttpOutRequest {
  HttpMethod method = HttpMethod::Get;
  std::string url;
  HttpHeaders headers;  // do script (validados no submit)
  std::string body;     // POST
  std::string contentType;  // POST; vazio = application/octet-stream
  std::vector<std::string> allow;  // permissions.http do resource (entradas já normalizadas)
  uint64_t owner = 0;              // dono (resource) para cotas e rodízio
};

struct HttpOutResult {
  bool ok = false;        // houve resposta HTTP (qualquer status)
  bool timedOut = false;  // prazo esgotado (fila ou execução)
  std::string error;      // sem corpo, cabeçalhos ou query
  int status = 0;
  HttpHeaders headers;  // nomes em minúsculas, na ordem recebida
  std::string body;

  static HttpOutResult fail(std::string e, bool timeout = false) {
    HttpOutResult r;
    r.error = std::move(e);
    r.timedOut = timeout;
    return r;
  }
};

struct HttpOutOptions {
  int threads = 4;
  std::chrono::milliseconds timeout{10000};       // execução de um pedido (todos os saltos de redirecionamento)
  std::chrono::milliseconds queueTimeout{10000};  // espera máxima na fila
  size_t maxUrl = 2048;
  size_t maxRequestHeaderBytes = 8 * 1024;
  size_t maxRequestBody = size_t{1} << 20;
  size_t maxResponseBody = size_t{1} << 20;
  size_t maxResponseHeaderBytes = 64 * 1024;
  int maxRedirects = 3;
  size_t maxPendingPerOwner = 64;
  size_t maxBytesPerOwner = size_t{4} << 20;  // corpos de pedido + respostas recebidas, em andamento
  std::chrono::milliseconds shutdownWait{2000};
  std::string caFile;  // PEM; vazio = CAs do sistema
  // Avisos do pool (thread presa no encerramento...). Chamado das threads do pool. Vazio = spdlog.
  std::function<void(bool error, const std::string&)> log;
};

struct HttpOutMetrics {
  uint64_t requests = 0, errors = 0, timeouts = 0, refused = 0;
  int threads = 0, busy = 0;
  size_t queued = 0;
};

namespace detail {
struct HttpPoolState;
}

class HttpOutPool {
 public:
  using Done = std::function<void(HttpOutResult)>;
  struct InsecureLoopbackForTests {};

  explicit HttpOutPool(HttpOutOptions opt = {});
  // Só testes: http:// e loopback liberados (allowInsecureLoopbackForTests).
  HttpOutPool(HttpOutOptions opt, InsecureLoopbackForTests);
  ~HttpOutPool();
  HttpOutPool(const HttpOutPool&) = delete;
  HttpOutPool& operator=(const HttpOutPool&) = delete;

  // Valida (URL, lista, cabeçalhos, tamanhos, cotas do dono) e enfileira; devolve o id (> 0). 0 + err se recusado.
  // `done` é chamado uma vez numa thread do pool, exceto se o pedido for cancelado.
  uint64_t submit(HttpOutRequest r, Done done, std::string& err);
  // Tira da fila ou interrompe (fecha o socket) sem chamar `done`.
  void cancel(uint64_t id);

  HttpOutMetrics metrics() const;
  const HttpOutOptions& options() const;
  bool testMode() const;

 private:
  void init(HttpOutOptions opt, bool test);
  std::shared_ptr<detail::HttpPoolState> st_;
};

}  // namespace cbscript
