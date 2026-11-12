import { describe, expect, it } from "vitest";
import { ApiError } from "./api";
import { blocksPlay, describeError, ERROR_TEXT, GENERIC_ERROR } from "./errors";

describe("erros por código", () => {
  it("códigos do contrato têm texto pt-BR próprio", () => {
    const expected: Record<string, RegExp> = {
      banned_global: /banid/i,
      protocol_mismatch: /incompat/i,
      manifest_mismatch: /não corresponde/i,
      mod_altered: /mod.*alterad/i,
      hash_mismatch: /corrompid/i,
      disk_full: /disco cheio/i,
      game_not_found: /Hogwarts Legacy não encontrado/i,
      session_expired: /sessão expirou/i,
      server_unavailable: /Servidor indisponível/i,
    };
    for (const [code, re] of Object.entries(expected)) {
      expect(ERROR_TEXT[code], code).toMatch(re);
      const d = describeError(new ApiError(code, "detalhe do Rust"));
      expect(d.title).toBe(ERROR_TEXT[code]);
      expect(d.code).toBe(code);
    }
  });

  it("erros do mod e do jogo aberto com as mensagens exatas", () => {
    const exact: Record<string, string> = {
      mod_conflict: "Já existe outro carregador de mods (UE4SS) na pasta do jogo.",
      mod_needs_admin: "O launcher não tem permissão para instalar o mod na pasta do jogo.",
      mod_incompatible: "Este mod precisa de uma versão mais nova do launcher. Atualize e tente de novo.",
      mod_activation_failed: "Não foi possível ativar o mod na pasta do jogo.",
      game_running: "O Hogwarts Legacy está aberto. Feche o jogo e tente de novo.",
      mod_logs_missing: "A pasta de logs do mod ainda não existe.",
    };
    for (const [code, text] of Object.entries(exact)) {
      expect(ERROR_TEXT[code], code).toBe(text);
      // A mesma frase vinda do Rust não vira detalhe repetido.
      expect(describeError(new ApiError(code, text)).detail, code).toBeNull();
    }
    // Pastas anteriores do jogo: título próprio, caminho no detalhe do Rust.
    const unreachable = describeError(
      new ApiError("mod_folder_unreachable", "A pasta do jogo D:\\HL não está acessível agora."),
    );
    expect(unreachable.title).not.toBe(GENERIC_ERROR);
    expect(unreachable.detail).toContain("D:\\HL");
    expect(ERROR_TEXT.mod_old_folder).toBeTruthy();
  });

  it("mostra o detalhe do Rust (resource, pasta) quando ele acrescenta algo", () => {
    const d = describeError(
      new ApiError("hash_mismatch", 'Arquivo com hash diferente no resource "mapa" ("client/a.lua").'),
    );
    expect(d.title).toBe(ERROR_TEXT.hash_mismatch);
    expect(d.detail).toContain('"mapa"');
    const same = describeError(new ApiError("disk_full", ERROR_TEXT.disk_full!));
    expect(same.detail).toBeNull();
  });

  it("código desconhecido: mensagem genérica com o código", () => {
    const d = describeError(new ApiError("xyz_novo", "<b>qualquer coisa</b>"));
    expect(d.title).toBe("Ocorreu um erro inesperado.");
    expect(d.detail).toBe("Código: xyz_novo");
  });

  it("banimento bloqueia todos; protocolo incompatível bloqueia o servidor", () => {
    expect(blocksPlay("banned_global")).toBe("all");
    for (const c of ["protocol_mismatch", "manifest_mismatch", "mod_unavailable"]) expect(blocksPlay(c)).toBe("server");
    for (const c of ["hash_mismatch", "cancelled", "disk_full", "server_unavailable"]) expect(blocksPlay(c)).toBeNull();
  });
});
