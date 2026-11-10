#pragma once
// Utilitários de teste para World + SnapshotBuilder (sem rede).
#include <string>
#include <variant>
#include <vector>

#include "cbserver/config.hpp"
#include "cbserver/pool.hpp"
#include "cbserver/snapshots.hpp"
#include "cbserver/world.hpp"

namespace testing_world {

struct Harness {
  explicit Harness(cbserver::ServerConfig c = {}, unsigned threads = 1)
      : cfg(std::move(c)), pool(threads), builder(pool), world(cfg) {}

  cbserver::ServerConfig cfg;
  cbserver::TaskPool pool;
  cbserver::SnapshotBuilder builder;
  cbserver::World world;
  std::vector<cbserver::Outgoing> out;
  std::vector<cbserver::KickOrder> kicks;
  double now = 0;

  void step(double dt = 0.05) {
    now += dt;
    out.clear();
    kicks.clear();
    world.tick(now, builder, out, kicks);
  }

  cbserver::EntityId add(const std::string& name, cbmp::Vec3i pos = {}) {
    cbmp::PlayerState s;
    s.pos = pos;
    auto id = world.addPlayer("sub-" + name, "did-" + name, name, now, s);
    return *id;
  }

  template <class T>
  std::vector<T> msgs(cbserver::EntityId to) const {
    std::vector<T> r;
    for (const auto& o : out)
      if (o.to == to)
        if (const T* p = std::get_if<T>(&o.message())) r.push_back(*p);
    return r;
  }

  // Partes do snapshot deste tick para o destinatário, na ordem.
  std::vector<cbmp::Snapshot> snaps(cbserver::EntityId to) const { return msgs<cbmp::Snapshot>(to); }

  const cbmp::SnapshotEntity* entityIn(cbserver::EntityId to, cbserver::EntityId id) const {
    for (const auto& o : out)
      if (o.to == to)
        if (const auto* s = std::get_if<cbmp::Snapshot>(&o.msg))
          for (const auto& e : s->ents)
            if (e.id == id) return &e;
    return nullptr;
  }
};

inline cbmp::Input input(uint32_t seq, cbmp::Vec3i pos, cbmp::MoveState st = cbmp::MoveState::Idle) {
  cbmp::Input in;
  in.seq = seq;
  in.s.pos = pos;
  in.s.state = st;
  return in;
}

}  // namespace testing_world
