#include "cbserver/script_bridge.hpp"

#include <memory>
#include <string>
#include <utility>
#include <variant>

#include <nlohmann/json.hpp>

namespace cbserver {

namespace {

cbscript::PlayerView viewOf(const PlayerSlot& p) {
  cbscript::PlayerView v;
  v.id = p.id;
  v.name = p.name;
  v.did = p.did;
  v.sub = p.sub;
  v.x = p.state.pos.x;
  v.y = p.state.pos.y;
  v.z = p.state.pos.z;
  v.state = static_cast<uint8_t>(p.state.state);
  return v;
}

bool startsWithCb(const std::string& s) { return s.size() >= 3 && s.compare(0, 3, "cb:") == 0; }

}  // namespace

ScriptBridge::ScriptBridge(World& world, cbscript::ScriptHost& host) : world_(world), host_(host) {
  WorldHooks h;
  h.join = [this](const PlayerSlot& p) { host_.post(cbscript::EvPlayerJoin{viewOf(p)}); };
  h.leave = [this](const PlayerSlot& p, std::string_view reason) {
    host_.post(cbscript::EvPlayerLeave{p.id, p.did, std::string(reason)});
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
    if (!host_.post(cbscript::EvClientEvent{p.id, ev.name, std::string(ev.payload.begin(), ev.payload.end())}))
      ++eventsDropped_;
  };
  world_.setHooks(std::move(h));
}

ScriptBridge::~ScriptBridge() { world_.setHooks({}); }

void ScriptBridge::applyCommands(double now) {
  cmds_.clear();
  host_.drainCommands(cmds_);
  for (auto& c : cmds_) {
    if (auto* e = std::get_if<cbscript::CmdEmit>(&c)) {
      cbmp::Event ev{std::move(e->name), std::vector<uint8_t>(e->json.begin(), e->json.end())};
      if (e->target < 0)
        world_.broadcastEvent(ev);
      else if (e->target <= 65535)
        world_.queueEvent(static_cast<EntityId>(e->target), std::move(ev));
    } else if (auto* k = std::get_if<cbscript::CmdKick>(&c)) {
      const PlayerSlot* p = world_.find(k->id);
      if (p && (k->did.empty() || p->did == k->did)) world_.kick(k->id, std::move(k->reason));
    } else if (auto* t = std::get_if<cbscript::CmdSetPosition>(&c)) {
      const PlayerSlot* p = world_.find(t->id);
      if (!p || (!t->did.empty() && p->did != t->did) || p->kicked) continue;
      cbmp::PlayerState s = p->state;
      s.pos = {t->x, t->y, t->z};
      s.vel = {};
      world_.setState(t->id, s, now);
    }
  }
}

void ScriptBridge::publishPlayers() {
  auto v = std::make_shared<std::vector<cbscript::PlayerView>>();
  v->reserve(world_.playerCount());
  for (EntityId id : world_.activeIds())
    if (const PlayerSlot* p = world_.find(id)) v->push_back(viewOf(*p));
  host_.publishPlayers(std::move(v));
}

}  // namespace cbserver
