import { describe, expect, it } from "vitest";
import { SERVERS } from "../test-utils";
import { applyFilters, EMPTY_FILTERS, localesOf, tagsOf } from "./filters";
import { formatBytes, formatSpeed } from "./format";

const ids = (f: Partial<typeof EMPTY_FILTERS>) => applyFilters(SERVERS, { ...EMPTY_FILTERS, ...f }).map((s) => s.id);

describe("filtros", () => {
  it("sem filtro mostra todos na ordem do Hub", () => {
    expect(ids({})).toEqual(["cb", "arena", "hl", "duel", "sp", "en"]);
  });

  it("busca por nome ignora maiúsculas e acentos", () => {
    expect(ids({ query: "ARENA" })).toEqual(["arena", "duel"]);
    expect(ids({ query: "escola de magia" })).toEqual(["sp"]);
    expect(ids({ query: "hógsmeade" })).toEqual(["hl"]);
  });

  it("tags exigem todas as escolhidas", () => {
    expect(ids({ tags: ["pvp"] })).toEqual(["arena", "duel"]);
    expect(ids({ tags: ["pvp", "duelo"] })).toEqual(["duel"]);
  });

  it("idioma, com vagas e tudo combinado", () => {
    expect(ids({ locale: "en-US" })).toEqual(["arena", "en"]);
    expect(ids({ freeSlots: true })).toEqual(["cb", "hl", "duel", "en"]);
    expect(ids({ tags: ["rp"], locale: "pt-BR", freeSlots: true })).toEqual(["cb", "hl"]);
    expect(ids({ tags: ["rp"], locale: "pt-BR", freeSlots: true, query: "hog" })).toEqual(["hl"]);
  });

  it("tags e idiomas disponíveis vêm da lista (mais comuns primeiro)", () => {
    expect(tagsOf(SERVERS)[0]).toBe("rp");
    expect(tagsOf(SERVERS)).toContain("pvp");
    expect(localesOf(SERVERS)).toEqual(["en-US", "pt-BR"]);
  });
});

describe("formatação", () => {
  it("bytes e velocidade em pt-BR", () => {
    expect(formatBytes(214 * 1024 * 1024)).toBe("214,0 MB");
    expect(formatBytes(512 * 1024)).toBe("0,5 MB");
    expect(formatBytes(3 * 1024 * 1024 * 1024)).toBe("3,00 GB");
    expect(formatSpeed(3.2 * 1024 * 1024)).toBe("3,2 MB/s");
    expect(formatSpeed(0)).toBe("0,0 MB/s");
  });
});
