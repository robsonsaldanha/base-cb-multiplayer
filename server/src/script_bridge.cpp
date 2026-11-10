#include "cbserver/script_bridge.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
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

bool ScriptBridge::validEmit(cbscript::CmdEmit& e) {
  if (e.name.empty() || e.name.size() > cbmp::kMaxEventName || e.json.size() > cbmp::kMaxEventPayload) {
    if (commandsRejected_++ % 1000 == 0)
      spdlog::error("scripts: evento '{}' descartado (nome {} bytes, payload {} bytes; limites 64/4096)",
                    e.name.substr(0, 64), e.name.size(), e.json.size());
    return false;
  }
  return true;
}

namespace {
uint64_t prioKey(const cbscript::ScriptToSim& c, int& kind, const std::string*& did) {
  did = nullptr;
  if (const auto* t = std::get_if<cbscript::CmdSetPosition>(&c)) {
    kind = 0;
    return (uint64_t{t->id} << 32) | t->session;
  }
  if (const auto* k = std::get_if<cbscript::CmdKick>(&c)) {
    kind = 1;
    return (uint64_t{k->id} << 32) | k->session;
  }
  if (const auto* d = std::get_if<cbscript::CmdKickDid>(&c)) {
    kind = 2;
    did = &d->did;
    return 0;
  }
  kind = -1;
  return 0;
}
}  // namespace

void ScriptBridge::addPriority(cbscript::ScriptToSim c) {
  // Coalescência também com o que sobrou de ticks anteriores: último teleporte por (id, sessão), primeiro kick.
  int kind = -1;
  const std::string* did = nullptr;
  const uint64_t key = prioKey(c, kind, did);
  if (kind == 0) {
    auto [it, fresh] = prioPos_.try_emplace(key, prioBacklog_.size());
    if (!fresh) {
      prioBacklog_[it->second] = std::move(c);
      return;
    }
  } else if (kind == 1) {
    if (!prioKick_.try_emplace(key, prioBacklog_.size()).second) return;
  } else if (kind == 2) {
    if (!prioKickDid_.try_emplace(*did, prioBacklog_.size()).second) return;
  }
  prioBacklog_.push_back(std::move(c));
}

bool ScriptBridge::postAdmin(cbscript::ScriptToSim c) {
  if (std::holds_alternative<cbscript::CmdEmit>(c)) return false;
  std::lock_guard<std::mutex> lk(adminMu_);
  if (adminInbox_.size() >= kMaxAdminInbox) return false;
  adminInbox_.push_back(std::move(c));
  return true;
}

void ScriptBridge::applyCommands(double now) {
  // 0) Ações do painel admin (outra thread): troca O(1) sob o mutex; entram no canal prioritário antes dos scripts.
  {
    std::lock_guard<std::mutex> lk(adminMu_);
    adminTaken_.swap(adminInbox_);
  }
  for (auto& c : adminTaken_) addPriority(std::move(c));
  adminTaken_.clear();

  // 1) Drena o host todo tick: prioritários (já coalescidos no host) e emits, cada emit na fila da sua origem.
  drained_.clear();
  host_.drainCommands(drained_);
  for (auto& c : drained_) {
    if (auto* e = std::get_if<cbscript::CmdEmit>(&c)) {
      auto& q = bySource_[e->source];
      auto& qb = sourceBytes_[e->source];
      const size_t nb = e->name.size() + e->json.size();
      if (q.size() >= kMaxBacklogPerSource || qb + nb > kMaxBacklogBytesPerSource) {
        ++commandsDropped_;  // atraso desta origem no teto: descarta só dela
        continue;
      }
      qb += nb;
      q.push_back(std::move(*e));
    } else {
      addPriority(std::move(c));
    }
  }
  drained_.clear();

  // 2) Prioritários: antes de qualquer emit, até o orçamento por tick (2 × maxPlayers); o resto fica para o próximo.
  const size_t prioBudget = std::max<size_t>(64, world_.maxPlayers() * 2);
  size_t applied = 0;
  for (; applied < prioBacklog_.size() && applied < prioBudget; ++applied) applyOne(prioBacklog_[applied], now);
  prioBacklog_.erase(prioBacklog_.begin(), prioBacklog_.begin() + static_cast<std::ptrdiff_t>(applied));
  prioPos_.clear();
  prioKick_.clear();
  prioKickDid_.clear();
  for (size_t i = 0; i < prioBacklog_.size(); ++i) {  // reindexa o que sobrou
    int kind = -1;
    const std::string* did = nullptr;
    const uint64_t key = prioKey(prioBacklog_[i], kind, did);
    if (kind == 0) prioPos_[key] = i;
    else if (kind == 1) prioKick_[key] = i;
    else if (kind == 2) prioKickDid_[*did] = i;
  }

  // 3) Emits: rodízio entre as origens, um de cada vez, dentro do orçamento (comandos, bytes, destinatários).
  size_t n = 0, bytes = 0, recipients = 0;
  const size_t players = world_.playerCount();
  bool progress = true, full = false;
  while (progress && !full) {
    progress = false;
    auto it = bySource_.upper_bound(rrLast_);
    for (size_t visited = 0, total = bySource_.size(); visited < total; ++visited) {
      if (it == bySource_.end()) it = bySource_.begin();
      auto& q = it->second;
      const uint32_t src = it->first;
      if (!q.empty()) {
        if (n >= kMaxCommandsPerTick || bytes >= kMaxBytesPerTick) {
          full = true;
          break;
        }
        cbscript::CmdEmit& e = q.front();
        const size_t cost = e.target < 0 ? players : 1;
        if (recipients > 0 && recipients + cost > kMaxEventOutPerTick) {
          full = true;
          break;
        }
        recipients += cost;
        ++n;
        const size_t nb = e.name.size() + e.json.size();
        bytes += nb;
        sourceBytes_[src] -= std::min(sourceBytes_[src], nb);
        cbscript::CmdEmit cmd = std::move(e);
        q.pop_front();
        rrLast_ = src;
        progress = true;
        if (validEmit(cmd)) {
          cbmp::Event ev{std::move(cmd.name), std::vector<uint8_t>(cmd.json.begin(), cmd.json.end())};
          if (cmd.target < 0)
            world_.broadcastEvent(std::move(ev), now);
          else if (cmd.target <= 65535)
            world_.queueEvent(static_cast<EntityId>(cmd.target), cmd.session, std::move(ev), now);
        }
      }
      ++it;
    }
  }
  // Origens sem nada pendente saem do mapa (resources parados não acumulam entradas).
  for (auto it = bySource_.begin(); it != bySource_.end();) {
    if (it->second.empty()) {
      sourceBytes_.erase(it->first);
      it = bySource_.erase(it);
    } else {
      ++it;
    }
  }
}

size_t ScriptBridge::backlog() const {
  size_t n = prioBacklog_.size();
  for (const auto& [src, q] : bySource_) n += q.size();
  return n;
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

cbscript::ApiOptions scriptApiOptions(const ServerConfig& cfg) {
  cbscript::ApiOptions api;
  api.isAdmin = chatAdminCheck(cfg.admin);
  return api;
}

}  // namespace cbserver
