// Tabela única código → texto pt-BR. O Rust também manda uma mensagem pt-BR, que vira o detalhe quando acrescenta
// algo (resource, pasta). Código desconhecido: texto genérico com o código.
import type { ApiError } from "./api";

export const ERROR_TEXT: Readonly<Record<string, string>> = {
  // Hub e sessão
  banned_global: "Sua conta está banida da plataforma e não pode entrar em servidores.",
  session_expired: "Sua sessão expirou. Entre de novo.",
  not_logged_in: "Entre com o Discord para continuar.",
  login_denied: "Login cancelado no Discord.",
  login_failed: "Não foi possível concluir o login. Tente de novo.",
  browser_failed: "Não foi possível abrir o navegador.",
  vault_error: "Não foi possível acessar o cofre de credenciais do Windows.",
  hub_unreachable: "Não foi possível falar com o Hub. Verifique sua conexão.",
  hub_busy: "O Hub está ocupado. Tente daqui a pouco.",
  hub_error: "O Hub respondeu com erro.",
  hub_bad_response: "O Hub respondeu algo inesperado.",
  hub_rejected: "O Hub recusou o pedido do launcher.",
  rate_limited: "Muitas tentativas seguidas. Aguarde um pouco.",
  forbidden: "Acesso negado pelo Hub.",
  not_found: "Servidor não encontrado.",
  // Servidor e conteúdo
  server_unavailable: "Servidor indisponível no momento.",
  invalid_server: "Identificador de servidor inválido.",
  protocol_mismatch: "Este servidor usa uma versão incompatível com o seu launcher.",
  manifest_mismatch: "O conteúdo anunciado não corresponde a este servidor (versão ou protocolo incompatível).",
  invalid_manifest: "O servidor enviou uma lista de conteúdo inválida.",
  content_too_large: "O conteúdo do servidor passa do limite configurado.",
  content_missing: "Conteúdo do servidor incompleto. Tente entrar de novo.",
  hash_mismatch: "Um arquivo baixado do servidor veio corrompido. A entrada foi interrompida.",
  size_mismatch: "Um arquivo baixado do servidor veio com tamanho diferente do anunciado.",
  download_slow: "A conexão está lenta demais ou caiu durante o download.",
  cancelled: "Entrada no servidor cancelada.",
  busy: "Já existe uma entrada em servidor em andamento.",
  // Disco
  disk_full: "Disco cheio. Libere espaço e tente de novo.",
  permission_denied: "Sem permissão para gravar na pasta.",
  io_error: "Falha ao gravar no disco.",
  // Jogo e mod
  game_not_found: "Hogwarts Legacy não encontrado. Escolha a pasta do jogo.",
  game_invalid: "A pasta escolhida não é do Hogwarts Legacy.",
  launch_failed: "Não foi possível abrir o jogo.",
  mod_unavailable: "Ainda não há mod publicado para a versão deste servidor.",
  mod_invalid: "O pacote do mod é inválido e foi recusado.",
  mod_signature_invalid: "O mod baixado não tem assinatura válida e foi recusado.",
  mod_altered: "Os arquivos do mod foram alterados e não puderam ser reparados.",
  // Configurações e atualização
  invalid_settings: "Configuração inválida.",
  settings_unreadable: "Não foi possível ler as configurações.",
  updates_disabled: "Atualizações indisponíveis no momento.",
  update_check_failed: "Não foi possível procurar atualização.",
  update_failed: "Não foi possível instalar a atualização.",
  signature_invalid: "A atualização baixada não tem assinatura válida e foi recusada.",
  no_update: "Nenhuma atualização disponível.",
  internal_error: "Falha interna do launcher.",
};

export const GENERIC_ERROR = "Ocorreu um erro inesperado.";

export interface ErrorView {
  code: string;
  title: string;
  /** Mensagem do Rust quando ela acrescenta algo ao título; `Código: x` para código desconhecido. */
  detail: string | null;
}

export function describeError(e: Pick<ApiError, "code" | "message">): ErrorView {
  const title = ERROR_TEXT[e.code];
  if (!title) return { code: e.code, title: GENERIC_ERROR, detail: `Código: ${e.code}` };
  const msg = e.message.trim();
  return { code: e.code, title, detail: msg && msg !== title ? msg : null };
}

/** Erros que desativam o Jogar: em todos os servidores (banimento) ou só no servidor (protocolo incompatível). */
export function blocksPlay(code: string): "all" | "server" | null {
  if (code === "banned_global") return "all";
  if (code === "protocol_mismatch" || code === "manifest_mismatch" || code === "mod_unavailable") return "server";
  return null;
}
