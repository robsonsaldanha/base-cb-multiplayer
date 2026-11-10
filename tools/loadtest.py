#!/usr/bin/env python3
"""Teste de carga e de segurança do cb-server com cb-bots (sem dependências fora da biblioteca padrão).

Carga: sobe o cb-server em modo dev (chave derivada de uma seed aleatória, hub null, sim.spawn devTrustFirstInput,
maxPlayers = count + 16, resource Lua resources.example/echo copiado para a pasta de resources), espera /info, roda o cb-bots, lê /metrics a cada 5 s, encerra e avalia os limites da spec:
  tick p99 <= 25 ms, banda média por jogador <= 64 kbps, RSS <= 512 MB, nenhum bot caiu/recusado/expulso,
  nenhum bot > 1 s sem snapshot.
Banda: o cb-server conta os bytes de payload das mensagens GNS enviadas a cada conexão de jogador (sem cabeçalhos
IP/UDP/GNS) ÷ tempo da conexão; avgKbpsPerPlayer é a média entre os jogadores. Vale a última amostra com jogadores
conectados. A estimativa "no fio" do GNS (wireAvgKbpsPerPlayer) é só informativa.
Com --assert, uma violação na carga repete a rodada 1x antes de falhar.

Segurança (curtos, cada um com servidor próprio):
  teleporte  bot 0 trapaceiro persistente (teleporta a cada 0,5 s e ignora Correction) → Correction e Kick em <= 15 s
  flood      bot 0 manda 500 eventos/s → Kick
  seed errada  tickets assinados com outra seed → todos recusados

Uso: python3 tools/loadtest.py --count 256 --duration 120 --cluster 0.3 --assert
"""
from __future__ import annotations

import argparse
import json
import os
import random
import secrets
import shutil
import signal
import socket
import subprocess
import sys
import threading
import time
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
IS_WIN = os.name == "nt"
EXE = ".exe" if IS_WIN else ""

LIMITS = {
    "tickP99Ms": 25.0,
    "avgKbpsPerPlayer": 64.0,
    "rssMB": 512.0,
    "maxSnapshotGapMs": 1000.0,
}
ECHO_RESOURCE = ROOT / "resources.example" / "echo"
SERVER_ID = "srv_dev"  # serverId do cb-server em modo dev (sem hub)
# Todos os bots saem do mesmo IP e a rampa (50/s) abre ~2–3 conexões por tick; o padrão de 4 pendentes por IP
# recusaria parte delas entre dois pumps. O limite por IP é testado nos testes unitários do net.
PENDING_PER_IP = 64
KICK_REASONS = {"teleport": "movimento inválido", "flood": "flood de eventos"}


def log(msg: str) -> None:
    print(f"[loadtest] {msg}", flush=True)


def find_binary(name: str, explicit: str | None) -> Path:
    if explicit:
        p = Path(explicit)
        if not p.is_file():
            sys.exit(f"binário não encontrado: {p}")
        return p
    sub = "server" if name == "cb-server" else "bots"
    found = [p for p in (ROOT / "build-native").glob(f"*/{sub}/**/{name}{EXE}") if p.is_file()]
    if not found:
        sys.exit(f"{name}{EXE} não encontrado em build-native/*/{sub}; compile antes ou use --{sub}-bin")
    return max(found, key=lambda p: p.stat().st_mtime)


def free_port() -> int:
    for _ in range(200):
        port = random.randint(20000, 32767)
        try:
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as t, socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as u:
                t.bind(("0.0.0.0", port))
                u.bind(("0.0.0.0", port))
            return port
        except OSError:
            continue
    sys.exit("nenhuma porta livre encontrada")


def http_json(port: int, path: str, timeout: float = 2.0):
    with urllib.request.urlopen(f"http://127.0.0.1:{port}{path}", timeout=timeout) as r:
        return json.loads(r.read().decode("utf-8"))


def dev_key(bots_bin: Path, seed: str) -> str:
    out = subprocess.run([str(bots_bin), "--dev-seed", seed, "--print-dev-key"], capture_output=True, text=True, check=True)
    return out.stdout.strip()


class Server:
    def __init__(self, server_bin: Path, workdir: Path, pubkey: str, max_players: int):
        self.workdir = workdir
        self.port = free_port()
        workdir.mkdir(parents=True, exist_ok=True)
        (workdir / "resources").mkdir(exist_ok=True)
        # O eco dos bots é feito em Lua pelo resource de exemplo (não há mais eco em C++ no servidor).
        shutil.copytree(ECHO_RESOURCE, workdir / "resources" / "echo", dirs_exist_ok=True)
        cfg = {
            "name": "loadtest",
            "description": "",
            "tags": ["teste"],
            "locale": "pt-BR",
            "maxPlayers": max_players,
            "port": self.port,
            "hub": None,
            "auth": {"devPublicKey": pubkey},
            "net": {"pendingPerIp": PENDING_PER_IP, "pendingGlobal": 256, "maxOutKbpsPerClient": 256},
            "sim": {"tickHz": 20, "streamRadiusM": 300, "maxVisible": 128, "spawn": {"mode": "devTrustFirstInput"}},
            "metrics": {"token": None},
            "resourcesDir": str(workdir / "resources"),
            "logsDir": str(workdir / "logs"),
        }
        cfg_path = workdir / "server.cfg"
        cfg_path.write_text(json.dumps(cfg, indent=2), encoding="utf-8")
        self.out = open(workdir / "server.out.log", "wb")
        kw = {"creationflags": subprocess.CREATE_NEW_PROCESS_GROUP} if IS_WIN else {}
        self.proc = subprocess.Popen([str(server_bin), "--config", str(cfg_path)], cwd=workdir, stdout=self.out,
                                     stderr=subprocess.STDOUT, **kw)
        self.samples: list[dict] = []
        self._stop = threading.Event()
        self._poller: threading.Thread | None = None

    def wait_ready(self, timeout: float = 20.0) -> dict:
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            if self.proc.poll() is not None:
                raise RuntimeError(f"cb-server saiu com código {self.proc.returncode} (ver {self.workdir / 'server.out.log'})")
            try:
                return http_json(self.port, "/info")
            except Exception:
                time.sleep(0.2)
        raise RuntimeError("cb-server não respondeu /info a tempo")

    def start_polling(self, every: float = 5.0) -> None:
        t0 = time.monotonic()

        def loop():
            while not self._stop.wait(every):
                try:
                    m = http_json(self.port, "/metrics")
                    m["t"] = round(time.monotonic() - t0, 1)
                    self.samples.append(m)
                except Exception as e:  # amostra perdida não derruba o teste
                    log(f"falha ao ler /metrics: {e}")

        self._poller = threading.Thread(target=loop, daemon=True)
        self._poller.start()

    def metrics_now(self) -> dict | None:
        try:
            return http_json(self.port, "/metrics")
        except Exception:
            return None

    def stop(self) -> int | None:
        self._stop.set()
        if self._poller:
            self._poller.join(timeout=5)
        if self.proc.poll() is None:
            try:
                self.proc.send_signal(signal.CTRL_BREAK_EVENT if IS_WIN else signal.SIGTERM)
                self.proc.wait(timeout=15)
            except Exception:
                self.proc.kill()
                self.proc.wait(timeout=5)
        self.out.close()
        return self.proc.returncode


def run_bots(bots_bin: Path, port: int, seed: str, report: Path, count: int, duration: float, cluster: float,
             area: float, extra: list[str] | None = None) -> dict:
    cmd = [str(bots_bin), "--server", f"127.0.0.1:{port}", "--dev-seed", seed, "--server-id", SERVER_ID,
           "--count", str(count), "--area", str(area), "--cluster", str(cluster), "--duration", str(duration),
           "--report", str(report)] + (extra or [])
    with open(report.with_suffix(".out.log"), "wb") as out:
        rc = subprocess.run(cmd, stdout=out, stderr=subprocess.STDOUT, timeout=duration + 120).returncode
    if rc != 0:
        raise RuntimeError(f"cb-bots saiu com código {rc} (ver {report.with_suffix('.out.log')})")
    return json.loads(report.read_text(encoding="utf-8"))


# ---------------------------------------------------------------------------------------------------- carga

def load_round(args, server_bin: Path, bots_bin: Path, out_dir: Path, attempt: int) -> dict:
    seed = secrets.token_hex(32)
    work = out_dir / f"load-{attempt}"
    shutil.rmtree(work, ignore_errors=True)
    srv = Server(server_bin, work, dev_key(bots_bin, seed), args.count + 16)
    try:
        info = srv.wait_ready()
        log(f"carga #{attempt}: servidor '{info.get('name')}' na porta {srv.port}; {args.count} bots por {args.duration} s")
        srv.start_polling(5.0)
        bots = run_bots(bots_bin, srv.port, seed, work / "bots-report.json", args.count, args.duration, args.cluster,
                        args.area)
    finally:
        rc = srv.stop()
    samples = srv.samples
    live = [s for s in samples if s.get("players", 0) > 0]
    last = live[-1] if live else {}
    res = {
        "attempt": attempt,
        "port": srv.port,
        "serverExit": rc,
        "bots": bots,
        "samples": samples,
        "tickP99Ms": max((s["tickP99Ms"] for s in live), default=0.0),
        "tickP50Ms": max((s["tickP50Ms"] for s in live), default=0.0),
        "tickMaxMs": max((s["tickMaxMs"] for s in live), default=0.0),
        "avgKbpsPerPlayer": last.get("avgKbpsPerPlayer", 0.0),
        "maxKbpsPerPlayer": last.get("maxKbpsPerPlayer", 0.0),
        "wireAvgKbpsPerPlayer": last.get("wireAvgKbpsPerPlayer", 0.0),
        "wireMaxKbpsPerPlayer": last.get("wireMaxKbpsPerPlayer", 0.0),
        "rssMB": max((s["rssBytes"] for s in samples), default=0) / (1024 * 1024),
        "cpuPercentMax": max((s.get("cpuPercent", 0.0) for s in live), default=0.0),
        "playersMax": max((s.get("players", 0) for s in samples), default=0),
        "serverKicks": last.get("kicks", 0),
        "serverCorrections": last.get("corrections", 0),
        "sendFailures": last.get("sendFailures", 0),
    }
    res["violations"] = load_violations(res, args.count)
    return res


def load_violations(r: dict, count: int) -> list[str]:
    v = []
    b = r["bots"]
    if not r["samples"]:
        v.append("nenhuma amostra de /metrics")
    if r["tickP99Ms"] > LIMITS["tickP99Ms"]:
        v.append(f"tick p99 {r['tickP99Ms']:.2f} ms > {LIMITS['tickP99Ms']}")
    if r["avgKbpsPerPlayer"] > LIMITS["avgKbpsPerPlayer"]:
        v.append(f"banda média {r['avgKbpsPerPlayer']:.1f} kbps > {LIMITS['avgKbpsPerPlayer']}")
    if r["rssMB"] > LIMITS["rssMB"]:
        v.append(f"RSS {r['rssMB']:.0f} MB > {LIMITS['rssMB']}")
    if b["connected"] != count or b["failedToConnect"]:
        v.append(f"conectados {b['connected']}/{count} (falhas {b['failedToConnect']}, recusados {b['rejected']})")
    if b["disconnected"]:
        v.append(f"{b['disconnected']} bot(s) caíram")
    if b["kicked"]:
        v.append(f"{b['kicked']} bot(s) expulsos: {b.get('kickReasons')}")
    sent, got = b.get("echoSent", 0), b.get("echoReplies", 0)
    if got <= 0 or got < 0.5 * sent:
        v.append(f"respostas de eco {got}/{sent} (< 50%: o resource Lua 'echo' não está respondendo)")
    if b.get("botsWithoutSnapshots", 0):
        v.append(f"{b['botsWithoutSnapshots']} bot(s) sem nenhum snapshot")
    if r.get("serverExit") not in (0, None):
        v.append(f"cb-server saiu com código {r['serverExit']}")
    if b["maxSnapshotGapMs"] > LIMITS["maxSnapshotGapMs"]:
        v.append(f"bot sem snapshot por {b['maxSnapshotGapMs']:.0f} ms > {LIMITS['maxSnapshotGapMs']:.0f}")
    return v


def print_load(r: dict) -> None:
    b = r["bots"]
    rows = [
        ("tick p50 / p99 / máx (ms)", f"{r['tickP50Ms']:.2f} / {r['tickP99Ms']:.2f} / {r['tickMaxMs']:.2f}", f"p99 <= {LIMITS['tickP99Ms']}"),
        ("banda média / máx por jogador (kbps)", f"{r['avgKbpsPerPlayer']:.1f} / {r['maxKbpsPerPlayer']:.1f}", f"média <= {LIMITS['avgKbpsPerPlayer']}"),
        ("banda no fio média / máx (kbps, info)", f"{r['wireAvgKbpsPerPlayer']:.1f} / {r['wireMaxKbpsPerPlayer']:.1f}", "-"),
        ("RSS máx (MB)", f"{r['rssMB']:.1f}", f"<= {LIMITS['rssMB']:.0f}"),
        ("CPU máx do servidor (%)", f"{r['cpuPercentMax']:.0f}", "-"),
        ("bots conectados / falhas / caíram / expulsos", f"{b['connected']} / {b['failedToConnect']} / {b['disconnected']} / {b['kicked']}", "todos / 0 / 0 / 0"),
        ("intervalo entre snapshots máx / p99 (ms)", f"{b['maxSnapshotGapMs']:.0f} / {b['p99SnapshotGapMs']:.0f}", f"máx <= {LIMITS['maxSnapshotGapMs']:.0f}"),
        ("RTT médio do eco (ms) / respostas", f"{b['avgRttMs']:.1f} / {b.get('echoReplies', 0)}", "respostas > 0"),
        ("banda recebida média por bot (kbps)", f"{b['avgKbpsInPerBot']:.1f}", "-"),
        ("correções / kicks no servidor", f"{r['serverCorrections']} / {r['serverKicks']}", "-"),
    ]
    w = max(len(x[0]) for x in rows)
    print()
    print(f"{'métrica'.ljust(w)}  {'valor':<28} limite")
    for name, val, lim in rows:
        print(f"{name.ljust(w)}  {val:<28} {lim}")
    print("resultado:", "OK" if not r["violations"] else "VIOLAÇÃO — " + "; ".join(r["violations"]))
    print()


# ----------------------------------------------------------------------------------------------- segurança

def security_scenario(name: str, server_bin: Path, bots_bin: Path, out_dir: Path, count: int, duration: float,
                      extra: list[str], wrong_seed: bool = False) -> dict:
    seed = secrets.token_hex(32)
    bots_seed = secrets.token_hex(32) if wrong_seed else seed
    work = out_dir / f"sec-{name}"
    shutil.rmtree(work, ignore_errors=True)
    srv = Server(server_bin, work, dev_key(bots_bin, seed), count + 16)
    try:
        srv.wait_ready()
        bots = run_bots(bots_bin, srv.port, bots_seed, work / "bots-report.json", count, duration, 1.0, 200, extra)
        metrics = srv.metrics_now() or {}
    finally:
        rc = srv.stop()
    return {"name": name, "bots": bots, "serverExit": rc, "serverKicks": metrics.get("kicks"),
            "serverCorrections": metrics.get("corrections"), "droppedEvents": metrics.get("droppedEvents")}


def check_security(r: dict) -> list[str]:
    v = []
    if r.get("error"):
        return [r["error"]]
    b = r["bots"]
    if r.get("serverExit") not in (0, None):
        v.append(f"cb-server saiu com código {r['serverExit']}")
    others = b["count"] - (1 if "scenarioBot" in b else 0)
    if r["name"] in ("teleport", "flood"):
        sb = b.get("scenarioBot", {})
        expected = KICK_REASONS[r["name"]]
        if not sb.get("welcomed"):
            v.append("bot 0 não entrou")
        if not sb.get("kicked"):
            v.append("bot 0 não foi expulso")
        elif sb.get("reason") != expected:
            v.append(f"motivo do kick '{sb.get('reason')}' (esperado '{expected}')")
        if r["name"] == "teleport":
            if sb.get("correctionsReceived", 0) < 1:
                v.append("bot 0 não recebeu Correction")
            if sb.get("kicked") and not (0 <= sb.get("kickAfterS", -1) <= 15.0):
                v.append(f"kick em {sb.get('kickAfterS'):.1f} s (> 15 s)")
        if b["connected"] != others or b["kicked"] or b["disconnected"]:
            v.append(f"bots honestos afetados (conectados {b['connected']}/{others}, expulsos {b['kicked']}, caíram {b['disconnected']})")
    elif r["name"] == "wrong-seed":
        if b["connected"] != 0:
            v.append(f"{b['connected']} bot(s) entraram com seed errada")
        if b["rejected"] != b["count"]:
            v.append(f"recusados {b['rejected']}/{b['count']}")
    return v


def run_security(server_bin: Path, bots_bin: Path, out_dir: Path) -> list[dict]:
    scenarios = [
        ("teleport", 4, 20.0, ["--teleport-every", "0.5"], False),
        ("flood", 4, 8.0, ["--flood"], False),
        ("wrong-seed", 8, 4.0, [], True),
    ]
    results = []
    for name, count, duration, extra, wrong in scenarios:
        log(f"segurança: {name}")
        try:
            r = security_scenario(name, server_bin, bots_bin, out_dir, count, duration, extra, wrong)
        except Exception as e:  # registra e segue para os próximos cenários
            r = {"name": name, "error": f"erro: {e}", "bots": {}}
        r["violations"] = check_security(r)
        if r.get("error"):
            log(f"  {name}: FALHOU — {r['error']}")
            results.append(r)
            continue
        sb = r["bots"].get("scenarioBot")
        detail = (f"bot 0: expulso={sb['kicked']} motivo='{sb['reason']}' correções={sb['correctionsReceived']} "
                  f"kick após {sb['kickAfterS']:.2f} s") if sb else (
                  f"conectados {r['bots']['connected']}, recusados {r['bots']['rejected']} {r['bots']['rejectReasons']}")
        log(f"  {name}: {'OK' if not r['violations'] else 'FALHOU — ' + '; '.join(r['violations'])} ({detail})")
        results.append(r)
    return results


def main() -> int:
    sys.stdout.reconfigure(errors="replace")
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--count", type=int, default=64)
    ap.add_argument("--duration", type=float, default=30)
    ap.add_argument("--cluster", type=float, default=0.3)
    ap.add_argument("--area", type=float, default=2000)
    ap.add_argument("--assert", dest="do_assert", action="store_true", help="sai 1 se algum limite for violado")
    ap.add_argument("--server-bin")
    ap.add_argument("--bots-bin")
    ap.add_argument("--out-dir", default=str(ROOT / "build-native" / "loadtest"))
    ap.add_argument("--skip-load", action="store_true")
    ap.add_argument("--skip-security", action="store_true")
    args = ap.parse_args()

    server_bin = find_binary("cb-server", args.server_bin)
    bots_bin = find_binary("cb-bots", args.bots_bin)
    out_dir = Path(args.out_dir).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)
    log(f"cb-server: {server_bin}")
    log(f"cb-bots:   {bots_bin}")

    report: dict = {"args": vars(args), "limits": LIMITS, "load": [], "security": []}
    path = out_dir / "report.json"
    ok = False
    try:
        ok = True
        if not args.skip_load:
            for attempt in (1, 2):
                try:
                    r = load_round(args, server_bin, bots_bin, out_dir, attempt)
                    print_load(r)
                except Exception as e:  # erro na rodada (servidor não subiu, bots falharam...) conta como violação
                    r = {"attempt": attempt, "error": f"erro: {e}", "violations": [f"erro: {e}"]}
                    log(f"carga #{attempt}: {r['error']}")
                report["load"].append(r)
                if not r["violations"] or not args.do_assert or attempt == 2:
                    break
                log("violação na carga: repetindo 1x")
            ok = ok and not r["violations"]
        if not args.skip_security:
            sec = run_security(server_bin, bots_bin, out_dir)
            report["security"] = sec
            ok = ok and all(not s["violations"] for s in sec)
    except BaseException as e:
        ok = False
        report["error"] = f"{type(e).__name__}: {e}"
        log(f"erro: {report['error']}")
    finally:
        report["ok"] = ok
        path.write_text(json.dumps(report, indent=2, ensure_ascii=False, default=str), encoding="utf-8")
    log(f"relatório: {path}")
    log("RESULTADO: " + ("OK" if ok else "FALHOU"))
    return 1 if (args.do_assert and not ok) else 0


if __name__ == "__main__":
    sys.exit(main())
