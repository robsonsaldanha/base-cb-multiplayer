import { describe, expect, it } from "vitest";
import { ApiError } from "./api";
import { blocksPlay, describeError, ERROR_TEXT } from "./errors";

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
