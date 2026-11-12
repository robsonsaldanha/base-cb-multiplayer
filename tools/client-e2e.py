#!/usr/bin/env python3
"""Integração do núcleo do cliente (mod) com o cb-server real, sem o jogo (só biblioteca padrão).

Sobe o cb-server em modo dev (chave derivada de uma seed aleatória, hub null, spawn na origem e o bloco `movement` do
servidor de teste, com `apparate` alto) e roda clientes `cbmp-headless` (núcleo real + GnsTransport + ScriptedGame).
Cada cliente recebe o ticket só por um connect.json gravado numa pasta temporária (ticket gerado por
`cb-bots --print-ticket --index i`, launchId aleatório), nunca pela linha de comando.

Casos:
  walk         A em 100000,50000,0 anda 20 m em +X a 300 cm/s depois de 6 s (16 s no total); B parado em
               100500,50000,0 (34 s). Passa se os dois chegaram a Online; B tem exatamente 1 boneco cujo caminho, a
               partir do primeiro ponto em que se moveu, fica a <= 60 cm do trecho do caminho local de A entre
               t - 0,3 s e t - 0,1 s; nenhum passo do boneco > 40 cm entre pontos consecutivos depois do primeiro;
               o boneco some em <= 15 s depois do fim de A; nenhum log do servidor contém "movimento inválido";
               A enviou >= 150 Input.
  loss         o mesmo com perda falsa de 20% (GNS) nos dois clientes.
  server-down  um cliente (40 s); 8 s depois de Online o servidor é morto; passa se o relatório termina com
               reason == "conexao_perdida".

Em falha imprime o motivo; relatórios e logs ficam em --out-dir (padrão build-native/client-e2e).

Uso: python tools/client-e2e.py --case all
"""
from __future__ import annotations

import argparse
import bisect
import json
import math
import os
import random
import secrets
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
IS_WIN = os.name == "nt"
EXE = ".exe" if IS_WIN else ""

SERVER_ID = "srv_dev"  # serverId do cb-server em modo dev (sem hub)
PROTOCOL = 1           # cbmp::kProtocolVersion
# Mesmo bloco do servidor de teste (tools/teste-online/server.cfg.json, Global Constraints do sub5a).
MOVEMENT = {"maxSpeedCmS": {"idle": 50, "walk": 600, "run": 1500, "broom": 12000, "swim": 1500, "fall": 8000,
                            "apparate": 100000000}, "tolerance": 0.2}
# Os clientes saem do mesmo IP; o padrão de pendentes por IP do servidor é baixo (mesma chave do loadtest.py).
PENDING_PER_IP = 64

START_A = (100000, 50000, 0)
START_B = (100500, 50000, 0)
MAX_PUPPET_DIST_CM = 60.0
LAG_MIN_S, LAG_MAX_S = 0.1, 0.3
MAX_STEP_CM = 40.0
MAX_DESTROY_DELAY_S = 15.0
MIN_INPUTS_A = 150
KILL_AFTER_ONLINE_S = 8.0
INVALID_MOVE = "movimento inválido"


def log(msg: str) -> None:
    print(f"[client-e2e] {msg}", flush=True)


# ------------------------------------------------------------------------------------------- binários e servidor

def find_binary(name: str, sub: str, explicit: str | None) -> Path:
    if explicit:
        p = Path(explicit)
        if not p.is_file():
            raise FileNotFoundError(f"{name} não encontrado: {p}")
        return p
    found = [p for p in (ROOT / "build-native").glob(f"*/{sub}/**/{name}{EXE}") if p.is_file()]
    if not found:
        raise FileNotFoundError(f"{name} não encontrado em build-native/*/{sub}; compile antes ou passe o caminho")
    return max(found, key=lambda p: p.stat().st_mtime)


def free_port() -> int:
    for _ in range(200):
        port = random.randint(20000, 32767)
        try:
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as t, \
                    socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as u:
                t.bind(("0.0.0.0", port))
                u.bind(("0.0.0.0", port))
            return port
        except OSError:
            continue
    raise RuntimeError("nenhuma porta livre encontrada")


def http_json(port: int, path: str, timeout: float = 2.0):
    with urllib.request.urlopen(f"http://127.0.0.1:{port}{path}", timeout=timeout) as r:
        return json.loads(r.read().decode("utf-8"))


def dev_key(bots_bin: Path, seed: str) -> str:
    out = subprocess.run([str(bots_bin), "--dev-seed", seed, "--print-dev-key"], capture_output=True, text=True,
                         check=True, timeout=30)
    return out.stdout.strip()


def make_ticket(bots_bin: Path, seed: str, index: int) -> str:
    out = subprocess.run([str(bots_bin), "--dev-seed", seed, "--server-id", SERVER_ID, "--print-ticket", "--index",
                          str(index)], capture_output=True, text=True, check=True, timeout=30)
    ticket = out.stdout.strip()
    if ticket.count(".") != 1:
        raise RuntimeError(f"cb-bots --print-ticket devolveu algo que não é ticket ({len(ticket)} bytes)")
    return ticket


class Server:
    def __init__(self, server_bin: Path, workdir: Path, pubkey: str):
        self.workdir = workdir
        self.port = free_port()
        workdir.mkdir(parents=True, exist_ok=True)
        (workdir / "resources").mkdir(exist_ok=True)
        cfg = {
            "name": "e2e",
            "port": self.port,
            "hub": None,
            "auth": {"devPublicKey": pubkey},
            "maxPlayers": 8,
            "net": {"pendingPerIp": PENDING_PER_IP},
            "resourcesDir": str(workdir / "resources"),
            "logsDir": str(workdir / "logs"),
            "sim": {"tickHz": 20, "streamRadiusM": 300, "maxVisible": 128, "spawn": {"mode": "origin"}},
            "movement": MOVEMENT,
        }
        cfg_path = workdir / "server.cfg"
        cfg_path.write_text(json.dumps(cfg, indent=2), encoding="utf-8")
        self.out = open(workdir / "server.out.log", "wb")
        kw = {"creationflags": subprocess.CREATE_NEW_PROCESS_GROUP} if IS_WIN else {}
        self.proc = subprocess.Popen([str(server_bin), "--config", str(cfg_path)], cwd=workdir, stdout=self.out,
                                     stderr=subprocess.STDOUT, **kw)

    def wait_ready(self, timeout: float = 30.0) -> dict:
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            if self.proc.poll() is not None:
                raise RuntimeError(f"cb-server saiu com código {self.proc.returncode} "
                                   f"(ver {self.workdir / 'server.out.log'})")
            try:
                return http_json(self.port, "/info")
            except Exception:
                time.sleep(0.2)
        raise RuntimeError("cb-server não respondeu /info a tempo")

    def kill(self) -> None:
        if self.proc.poll() is None:
            self.proc.kill()
            self.proc.wait(timeout=10)

    def stop(self) -> int | None:
        if self.proc.poll() is None:
            try:
                self.proc.send_signal(signal.CTRL_BREAK_EVENT if IS_WIN else signal.SIGTERM)
                self.proc.wait(timeout=15)
            except Exception:
                self.proc.kill()
                self.proc.wait(timeout=5)
        self.out.close()
        return self.proc.returncode

    def log_texts(self) -> dict[str, str]:
        texts = {}
        for p in [self.workdir / "server.out.log"] + sorted((self.workdir / "logs").glob("*.log")):
            try:
                texts[str(p)] = p.read_text(encoding="utf-8", errors="replace")
            except OSError:
                pass
        return texts


def fresh_dir(path: Path) -> None:
    """Pasta do caso vazia (no Windows a pasta pode sobrar por um instante depois do rmtree: os arquivos somem)."""
    shutil.rmtree(path, ignore_errors=True)
    path.mkdir(parents=True, exist_ok=True)
    left = [p for p in path.rglob("*") if p.is_file()]
    if left:
        raise RuntimeError(f"não foi possível limpar {path} ({len(left)} arquivo(s) presos)")


# ------------------------------------------------------------------------------------------------- clientes

class Client:
    """Um cbmp-headless. O connect.json (com o ticket) fica numa pasta temporária que o próprio cliente apaga ao
    resgatar o arquivo; a pasta é removida no fim."""

    def __init__(self, headless_bin: Path, name: str, workdir: Path, port: int, ticket: str, start: tuple,
                 duration: float, extra: list[str]):
        self.name = name
        self.report_path = workdir / f"{name}.report.json"
        self.out_path = workdir / f"{name}.out.log"
        self.tmp = Path(tempfile.mkdtemp(prefix=f"cbmp-e2e-{name}-"))
        launch_id = secrets.token_hex(16)
        connect = self.tmp / "connect.json"
        connect.write_text(json.dumps({
            "host": "127.0.0.1", "port": port, "ticket": ticket, "serverId": SERVER_ID,
            "contentDir": str(self.tmp / "content"), "protocol": PROTOCOL, "contentReadOnly": True,
            "launchId": launch_id,
        }), encoding="utf-8")
        cmd = [str(headless_bin), "--connect", str(connect), "--launch-id", launch_id,
               "--start", ",".join(str(v) for v in start), "--duration", str(duration),
               "--report", str(self.report_path)] + extra
        self.out = open(self.out_path, "wb")
        self.proc = subprocess.Popen(cmd, stdout=self.out, stderr=subprocess.STDOUT)
        self.timeout = duration + 60

    def wait(self) -> int:
        try:
            return self.proc.wait(timeout=self.timeout)
        finally:
            self.close()

    def output(self) -> str:
        try:
            return self.out_path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            return ""

    def report(self) -> dict:
        try:
            return json.loads(self.report_path.read_text(encoding="utf-8"))
        except (OSError, ValueError) as e:
            raise RuntimeError(f"{self.name}: relatório ausente ou inválido ({e}; ver {self.out_path})")

    def close(self) -> None:
        if self.proc.poll() is None:
            self.proc.kill()
            self.proc.wait(timeout=10)
        if not self.out.closed:
            self.out.close()
        shutil.rmtree(self.tmp, ignore_errors=True)


# ------------------------------------------------------------------------------------------------- geometria

def pos_at(path: list, times: list, t: float) -> tuple:
    """Posição no caminho [[t,x,y,z],...] (ordenado por t) no instante t, interpolando linearmente; fora do
    intervalo, o ponto da ponta."""
    if t <= times[0]:
        return tuple(path[0][1:4])
    if t >= times[-1]:
        return tuple(path[-1][1:4])
    i = bisect.bisect_right(times, t)
    a, b = path[i - 1], path[i]
    span = b[0] - a[0]
    f = 0.0 if span <= 0 else (t - a[0]) / span
    return tuple(a[k] + (b[k] - a[k]) * f for k in (1, 2, 3))


def dist(a: tuple, b: tuple) -> float:
    return math.sqrt(sum((a[k] - b[k]) ** 2 for k in range(3)))


def dist_to_segment(p: tuple, a: tuple, b: tuple) -> float:
    ab = [b[k] - a[k] for k in range(3)]
    ap = [p[k] - a[k] for k in range(3)]
    den = sum(v * v for v in ab)
    f = 0.0 if den <= 0 else max(0.0, min(1.0, sum(ab[k] * ap[k] for k in range(3)) / den))
    return dist(p, tuple(a[k] + ab[k] * f for k in range(3)))


def dist_to_window(p: tuple, path: list, times: list, t0: float, t1: float) -> float:
    """Distância de p ao trecho do caminho entre t0 e t1 (polilinha: ponto em t0, pontos internos, ponto em t1)."""
    pts = [pos_at(path, times, t0)]
    lo, hi = bisect.bisect_right(times, t0), bisect.bisect_left(times, t1)
    pts += [tuple(q[1:4]) for q in path[lo:hi]]
    pts.append(pos_at(path, times, t1))
    if len(pts) == 1:
        return dist(p, pts[0])
    return min(dist_to_segment(p, pts[i], pts[i + 1]) for i in range(len(pts) - 1))


# ------------------------------------------------------------------------------------------------- critérios

def check_walk(a: dict, b: dict, server_logs: dict[str, str]) -> list[str]:
    v = []
    if not a.get("online"):
        v.append(f"A não chegou a Online (estado {a.get('state')}, motivo '{a.get('reason')}')")
    if not b.get("online"):
        v.append(f"B não chegou a Online (estado {b.get('state')}, motivo '{b.get('reason')}')")
    for path, text in server_logs.items():
        if INVALID_MOVE in text:
            v.append(f"log do servidor contém '{INVALID_MOVE}' ({path})")
    if a.get("inputs", 0) < MIN_INPUTS_A:
        v.append(f"A enviou {a.get('inputs', 0)} Input (< {MIN_INPUTS_A})")
    puppets = b.get("puppets", [])
    if len(puppets) != 1:
        v.append(f"B tem {len(puppets)} boneco(s) (esperado exatamente 1)")
        return v
    pup = puppets[0]
    ppath = pup.get("path", [])
    local = a.get("local", [])
    if len(local) < 2:
        v.append("caminho local de A vazio")
        return v
    if len(ppath) < 2:
        v.append("caminho do boneco vazio")
        return v
    ltimes = [q[0] for q in local]
    origin = tuple(ppath[0][1:4])
    first = next((i for i, q in enumerate(ppath) if dist(tuple(q[1:4]), origin) > 1.0), None)
    if first is None:
        v.append("o boneco nunca se moveu")
    else:
        worst, worst_t = 0.0, 0.0
        for q in ppath[first:]:
            d = dist_to_window(tuple(q[1:4]), local, ltimes, q[0] - LAG_MAX_S, q[0] - LAG_MIN_S)
            if d > worst:
                worst, worst_t = d, q[0]
        if worst > MAX_PUPPET_DIST_CM:
            v.append(f"boneco a {worst:.1f} cm do caminho de A atrasado {LAG_MIN_S}–{LAG_MAX_S} s "
                     f"(> {MAX_PUPPET_DIST_CM}; t = {worst_t - a.get('startedAt', 0):.2f} s do início de A)")
        moved = dist(tuple(ppath[-1][1:4]), origin)
        if moved < 1500:
            v.append(f"boneco andou só {moved:.0f} cm (A andou 2000)")
    big = 0.0
    for i in range(2, len(ppath)):
        big = max(big, dist(tuple(ppath[i - 1][1:4]), tuple(ppath[i][1:4])))
    if big > MAX_STEP_CM:
        v.append(f"passo do boneco de {big:.1f} cm (> {MAX_STEP_CM})")
    destroyed = pup.get("destroyedAt")
    if destroyed is None:
        v.append("o boneco de A não foi destruído em B")
    elif destroyed - a.get("endedAt", 0) > MAX_DESTROY_DELAY_S:
        v.append(f"boneco destruído {destroyed - a['endedAt']:.1f} s depois do fim de A (> {MAX_DESTROY_DELAY_S})")
    return v


def walk_summary(a: dict, b: dict) -> str:
    pups = b.get("puppets", [])
    extra = ""
    if pups:
        p = pups[0]
        d = p.get("destroyedAt")
        extra = (f", boneco com {len(p.get('path', []))} pontos, destruído "
                 + (f"{d - a.get('endedAt', 0):.2f} s depois do fim de A" if d is not None else "nunca"))
    return (f"A: inputs={a.get('inputs')} snapshots={a.get('snapshots')}; B: snapshots={b.get('snapshots')} "
            f"acks={b.get('acks')} bonecos={len(pups)}{extra}")


# ------------------------------------------------------------------------------------------------- casos

class Env:
    def __init__(self, args, server_bin: Path, bots_bin: Path, headless_bin: Path, out_dir: Path):
        self.args = args
        self.server_bin = server_bin
        self.bots_bin = bots_bin
        self.headless_bin = headless_bin
        self.out_dir = out_dir


def case_walk(env: Env, name: str, loss: int) -> list[str]:
    work = env.out_dir / name
    fresh_dir(work)
    seed = secrets.token_hex(32)
    srv = Server(env.server_bin, work / "server", dev_key(env.bots_bin, seed))
    clients: list[Client] = []
    try:
        srv.wait_ready()
        loss_args = ["--loss", str(loss)] if loss else []
        tickets = [make_ticket(env.bots_bin, seed, i) for i in (0, 1)]
        log(f"{name}: servidor na porta {srv.port}; A anda 20 m depois de 6 s (16 s), B parado (34 s)"
            + (f"; perda falsa de {loss}%" if loss else ""))
        a = Client(env.headless_bin, "A", work, srv.port, tickets[0], START_A, 16,
                   ["--walk-after", "6", "--walk-cm", "2000", "--speed", "300"] + loss_args)
        clients.append(a)
        b = Client(env.headless_bin, "B", work, srv.port, tickets[1], START_B, 34, ["--walk-cm", "0"] + loss_args)
        clients.append(b)
        codes = [c.wait() for c in clients]
    finally:
        for c in clients:
            c.close()
        rc = srv.stop()
    ra, rb = clients[0].report(), clients[1].report()
    v = check_walk(ra, rb, srv.log_texts())
    for c, code in zip(clients, codes):
        if code != 0:
            v.append(f"{c.name} saiu com código {code} (ver {c.out_path})")
    if rc not in (0, None):
        v.append(f"cb-server saiu com código {rc}")
    log(f"{name}: {walk_summary(ra, rb)}")
    return v


def wait_online(client: Client, timeout: float) -> bool:
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if "Welcome playerId=" in client.output():
            return True
        if client.proc.poll() is not None:
            return False
        time.sleep(0.1)
    return False


def case_server_down(env: Env) -> list[str]:
    name = "server-down"
    work = env.out_dir / name
    fresh_dir(work)
    seed = secrets.token_hex(32)
    srv = Server(env.server_bin, work / "server", dev_key(env.bots_bin, seed))
    v: list[str] = []
    c: Client | None = None
    try:
        srv.wait_ready()
        log(f"{name}: servidor na porta {srv.port}; um cliente (40 s); servidor morto {KILL_AFTER_ONLINE_S:.0f} s "
            "depois de Online")
        c = Client(env.headless_bin, "C", work, srv.port, make_ticket(env.bots_bin, seed, 0), START_A, 40,
                   ["--walk-cm", "0"])
        if not wait_online(c, 30):
            v.append(f"cliente não chegou a Online (ver {c.out_path})")
        else:
            time.sleep(KILL_AFTER_ONLINE_S)
            srv.kill()
            log(f"{name}: servidor morto")
        code = c.wait()
        if code != 0 and not v:
            v.append(f"cliente saiu com código {code} (ver {c.out_path})")
    finally:
        if c:
            c.close()
        srv.stop()
    r = c.report()
    log(f"{name}: estado={r.get('state')} motivo='{r.get('reason')}'")
    if r.get("reason") != "conexao_perdida":
        v.append(f"motivo '{r.get('reason')}' (esperado 'conexao_perdida')")
    return v


def main() -> int:
    sys.stdout.reconfigure(errors="replace")
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--server-bin")
    ap.add_argument("--bots-bin")
    ap.add_argument("--headless-bin")
    ap.add_argument("--out-dir", default=str(ROOT / "build-native" / "client-e2e"))
    ap.add_argument("--case", choices=["walk", "loss", "server-down", "all"], default="all")
    args = ap.parse_args()

    try:
        server_bin = find_binary("cb-server", "server", args.server_bin)
        bots_bin = find_binary("cb-bots", "bots", args.bots_bin)
        headless_bin = find_binary("cbmp-headless", "client/headless", args.headless_bin)
    except FileNotFoundError as e:
        log(f"FALHOU: {e}")
        return 1
    out_dir = Path(args.out_dir).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)
    log(f"cb-server:     {server_bin}")
    log(f"cb-bots:       {bots_bin}")
    log(f"cbmp-headless: {headless_bin}")
    env = Env(args, server_bin, bots_bin, headless_bin, out_dir)

    cases = ["walk", "loss", "server-down"] if args.case == "all" else [args.case]
    results = {}
    for case in cases:
        t0 = time.monotonic()
        try:
            if case == "walk":
                v = case_walk(env, "walk", 0)
            elif case == "loss":
                v = case_walk(env, "loss", 20)
            else:
                v = case_server_down(env)
        except Exception as e:  # erro de execução conta como falha do caso
            v = [f"erro: {type(e).__name__}: {e}"]
        results[case] = v
        log(f"{case}: {'OK' if not v else 'FALHOU — ' + '; '.join(v)} ({time.monotonic() - t0:.0f} s)")
    ok = all(not v for v in results.values())
    (out_dir / "result.json").write_text(json.dumps({"ok": ok, "cases": results}, indent=2, ensure_ascii=False),
                                         encoding="utf-8")
    log(f"relatórios e logs em {out_dir}")
    log("RESULTADO: " + ("OK" if ok else "FALHOU"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
