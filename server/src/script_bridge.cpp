#include "cbserver/script_bridge.hpp"

#include <atomic>
#include <memory>
#include <string>
#include <utility>
#include <variant>

#include <cbmp/messages.hpp>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

namespace cbserver {

namespace {

void fillView(cbscript::PlayerView& v, const PlayerSlot& p) {
  v.id = p.id;
  v.name.assign(p.name);  // assign reaproveita a capacidade das strings do buffer
  v.did.assign(p.did);
  v.sub.assign(p.sub);
  v.x = p.state.pos.x;
  v.y = p.state.pos.y;
  v.z = p.state.pos.z;
  v.state = static_cast<uint8_t>(p.state.state);
  v.session = p.session;
}

cbscript::PlayerView viewOf(const PlayerSlot& p) {
  cbscript::PlayerView v;
  fillView(v, p);
  return v;
}

bool startsWithCb(const std::string& s) { return s.size() >= 3 && s.compare(0, 3, "cb:") == 0; }

size_t payloadBytes(const cbscript::ScriptToSim& c) {
  if (const auto* e = std::get_if<cbscript::CmdEmit>(&c)) return e->name.size() + e->json.size();
  return 0;
}

}  // namespace

ScriptBridge::ScriptBridge(World& world, cbscript::ScriptHost& host) : world_(world), host_(host) {
  WorldHooks h;
  h.join = [this](const PlayerSlot& p) {
    if (!host_.post(cbscript::EvPlayerJoin{viewOf(p)})) {
      ++joinLeaveDropped_;
      spdlog::error("scripts: entrada do jogador {} (sessão {}) não entregue (fila no teto ou host parado)", p.id,
                    p.session);
    }
  };
  h.leave = [this](const PlayerSlot& p, std::string_view reason) {
    if (!host_.post(cbscript::EvPlayerLeave{p.id, p.did, std::string(reason), p.session})) {
      ++joinLeaveDropped_;
      spdlog::error("scripts: saída do jogador {} (sessão {}) não entregue (fila no teto ou host parado)", p.id,
                    p.session);
    }
  };
  h.event = [this](const PlayerSlot& p, const cbmp::Event& ev) {
    if (ev.name == "cb:command") {
      // Linha digitada no chat: o payload é um texto JSON ("/comando args").
      auto j = nlohmann::json::parse(ev.payload.begin(), ev.payload.end(), nullptr, false);
      if (j.is_discarded() || !j.is_string()) return;
      cbscript::EvCommand c;
      c.from = p.id;
      c.line = j.get<std::string>();
      if (!host_.post(std::move(c))) ++eventsDropped_;
      return;
    }
    if (startsWithCb(ev.name)) return;  // reservados ao motor
    if (!host_.post(cbscript::EvClientEvent{p.id, ev.name, std::string(ev.payload.begin(), ev.payload.end()),
                                            p.session}))
      ++eventsDropped_;
  };
  world_.setHooks(std::move(h));
}

ScriptBridge::~ScriptBridge() { world_.setHooks({}); }

void ScriptBridge::applyOne(cbscript::ScriptToSim& c, double now) {
  if (auto* k = std::get_if<cbscript::CmdKick>(&c)) {
    world_.kick(k->id, std::move(k->reason), k->session);
  } else if (auto* kd = std::get_if<cbscript::CmdKickDid>(&c)) {
    world_.kickDid(kd->did, kd->reason);
  } else if (auto* t = std::get_if<cbscript::CmdSetPosition>(&c)) {
    const PlayerSlot* p = world_.find(t->id);
    if (!p || (t->session != 0 && p->session != t->session) || p->kicked) return;
    cbmp::PlayerState s = p->state;
    s.pos = {t->x, t->y, t->z};
    s.vel = {};
    world_.setState(t->id, s, now);
  }
}

void ScriptBridge::applyCommands(double now) {
  // 1) Canal prioritário (kick/teleporte): sempre, todo tick, antes de qualquer emit e sem passar pelo atraso.
  drained_.clear();
  host_.drainPriority(drained_);
  for (auto& c : drained_) applyOne(c, now);
  drained_.clear();
  // 2) Emits (e prioritários que vierem junto no dreno geral), com orçamento por tick.
  if (backlog_.empty()) {
    host_.drainCommands(drained_);
    for (auto& c : drained_) {
      if (std::holds_alternative<cbscript::CmdEmit>(c))
        backlog_.push_back(std::move(c));
      else
        applyOne(c, now);
    }
    drained_.clear();
  }
  size_t n = 0, bytes = 0, recipients = 0;
  const size_t players = world_.playerCount();
  while (!backlog_.empty() && n < kMaxCommandsPerTick && bytes < kMaxBytesPerTick) {
    // Orçamento de destinatários: um broadcast conta como N eventos de saída.
    if (const auto* e = std::get_if<cbscript::CmdEmit>(&backlog_.front())) {
      const size_t cost = e->target < 0 ? players : 1;
      if (recipients > 0 && recipients + cost > kMaxEventOutPerTick) break;
      recipients += cost;
    }
    cbscript::ScriptToSim c = std::move(backlog_.front());
    backlog_.pop_front();
    ++n;
    bytes += payloadBytes(c);
    if (auto* e = std::get_if<cbscript::CmdEmit>(&c)) {
      if (e->name.empty() || e->name.size() > cbmp::kMaxEventName || e->json.size() > cbmp::kMaxEventPayload) {
        if (commandsRejected_++ % 1000 == 0)
          spdlog::error("scripts: evento '{}' descartado (nome {} bytes, payload {} bytes; limites 64/4096)",
                        e->name.substr(0, 64), e->name.size(), e->json.size());
        continue;
      }
      cbmp::Event ev{std::move(e->name), std::vector<uint8_t>(e->json.begin(), e->json.end())};
      if (e->target < 0)
        world_.broadcastEvent(std::move(ev), now);
      else if (e->target <= 65535)
        world_.queueEvent(static_cast<EntityId>(e->target), e->session, std::move(ev), now);
    } else {
      applyOne(c, now);
    }
  }
}

void ScriptBridge::publishPlayers() {
  // Reaproveita um buffer que só a ponte ainda segura (a thread de scripts já trocou/soltou a foto).
  std::shared_ptr<std::vector<cbscript::PlayerView>> buf;
  for (auto& b : snapPool_)
    if (b.use_count() == 1) {
      std::atomic_thread_fence(std::memory_order_acquire);  // pareia com a liberação da última cópia
      buf = b;
      break;
    }
  if (!buf) {
    buf = std::make_shared<std::vector<cbscript::PlayerView>>();
    if (snapPool_.size() < 4) snapPool_.push_back(buf);
  }
  const auto& ids = world_.activeIds();
  buf->resize(ids.size());
  size_t i = 0;
  for (EntityId id : ids)
    if (const PlayerSlot* p = world_.find(id)) fillView((*buf)[i++], *p);
  buf->resize(i);
  host_.publishPlayers(std::move(buf));
}

}  // namespace cbserver
