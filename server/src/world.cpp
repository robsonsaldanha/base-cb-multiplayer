#include "cbserver/world.hpp"

#include <algorithm>
#include <utility>

#include "cbserver/snapshots.hpp"

namespace cbserver {

namespace {
constexpr double kWindowSec = 10.0;      // janela de correções/violações
constexpr size_t kMaxCorrections = 10;   // > 10 em 10 s → kick
constexpr double kMaxMoveDtSec = 1.0;    // dt do validador nunca passa disso
constexpr double kBaselineMaxAgeSec = 1.0;
constexpr double kCorrectionWindowSec = 1.0;  // após uma Correction, não envia/conta outra por até 1 s

void trimWindow(std::deque<double>& q, double now) {
  while (!q.empty() && q.front() < now - kWindowSec) q.pop_front();
}

// Corta em kMaxName bytes sem quebrar um caractere UTF-8.
void clampName(std::string& name) {
  if (name.size() <= cbmp::kMaxName) return;
  size_t n = cbmp::kMaxName;
  while (n > 0 && (static_cast<unsigned char>(name[n]) & 0xC0) == 0x80) --n;
  name.resize(n);
}

template <class T>
void eraseSorted(std::vector<T>& v, const T& x) {
  auto it = std::lower_bound(v.begin(), v.end(), x);
  if (it != v.end() && *it == x) v.erase(it);
}
}  // namespace

World::World(const ServerConfig& cfg)
    : cfg_(cfg), minDt_(1.0 / std::max(1, cfg.tickHz)), grid_(6400) {
  limits_.maxSpeedCmS = {cfg.speedIdle, cfg.speedWalk, cfg.speedRun, cfg.speedBroom,
                         cfg.speedSwim, cfg.speedFall, cfg.speedApparate};
  limits_.tolerance = cfg.tolerance;
  vis_.enterCm = static_cast<int32_t>(cfg.streamRadiusM * 100.0);
  vis_.exitCm = static_cast<int32_t>(cfg.streamRadiusM * 110.0);  // histerese de 10% (300 → 330 m)
  vis_.maxVisible = std::min<size_t>(static_cast<size_t>(std::max(0, cfg.maxVisible)), cbmp::kMaxSnapshotEntities);
  const int maxPlayers = std::clamp(cfg.maxPlayers, 0, 65535);
  slots_.resize(static_cast<size_t>(maxPlayers) + 1);
}

World::~World() = default;

PlayerSlot* World::slot(EntityId id) { return id < slots_.size() ? slots_[id].get() : nullptr; }

const PlayerSlot* World::find(EntityId id) const { return id < slots_.size() ? slots_[id].get() : nullptr; }

std::optional<EntityId> World::addPlayer(std::string sub, std::string did, std::string name, double now,
                                         const cbmp::PlayerState& spawn) {
  for (size_t i = 1; i < slots_.size(); ++i) {
    if (slots_[i]) continue;
    auto p = std::make_unique<PlayerSlot>();
    p->id = static_cast<EntityId>(i);
    p->sub = std::move(sub);
    p->did = std::move(did);
    p->name = std::move(name);
    clampName(p->name);
    p->state = spawn;
    p->lastInputTime = now;
    grid_.upsert(p->id, p->state.pos);
    sendCorrection(*p, 0, now);
    active_.insert(std::lower_bound(active_.begin(), active_.end(), p->id), p->id);
    slots_[i] = std::move(p);
    return static_cast<EntityId>(i);
  }
  return std::nullopt;
}

void World::forget(PlayerSlot& viewer, EntityId gone) {
  for (auto& rec : viewer.sent)
    std::erase_if(rec.ents, [gone](const SentEntity& e) { return e.id == gone; });
}

void World::removePlayer(EntityId id) {
  if (!slot(id)) return;
  grid_.remove(id);
  eraseSorted(active_, id);
  slots_[id].reset();
  std::erase_if(pending_, [id](const Outgoing& o) { return o.to == id; });
  std::erase_if(pendingKicks_, [id](const KickOrder& k) { return k.who == id; });
  for (EntityId other : active_) {
    PlayerSlot& v = *slots_[other];
    if (std::binary_search(v.visible.ids.begin(), v.visible.ids.end(), id)) {
      eraseSorted(v.visible.ids, id);
      forget(v, id);
      pending_.push_back(Outgoing{other, cbmp::EntityLeave{id}});
    }
  }
}

void World::setState(EntityId id, const cbmp::PlayerState& s, double now) {
  PlayerSlot* p = slot(id);
  if (!p) return;
  p->state = s;
  p->lastInputTime = now;
  grid_.upsert(id, s.pos);
  sendCorrection(*p, p->lastInputSeq, now);
}

void World::sendCorrection(PlayerSlot& p, uint32_t seq, double now) {
  pending_.push_back(Outgoing{p.id, cbmp::Correction{seq, p.state.pos, p.state.vel}});
  p.correcting = true;
  p.correctionSince = now;
}

void World::orderKick(PlayerSlot& s, const char* reason) {
  if (s.kicked) return;
  s.kicked = true;
  pendingKicks_.push_back(KickOrder{s.id, reason});
}

void World::onInput(EntityId id, const cbmp::Input& in, double now) {
  PlayerSlot* p = slot(id);
  if (!p || p->kicked || in.seq <= p->lastInputSeq) return;
  p->lastInputSeq = in.seq;
  if (p->correcting) {
    const auto slack = static_cast<int64_t>(limits_.slackCm);
    if (now - p->correctionSince >= kCorrectionWindowSec || distSq(in.s.pos, p->state.pos) <= slack * slack)
      p->correcting = false;  // cliente aplicou a correção (ou a janela venceu): valida normalmente
    else
      return;  // input ainda baseado na posição antiga (latência): ignorado, sem novo incidente
  }
  const double dt = std::clamp(now - p->lastInputTime, minDt_, kMaxMoveDtSec);
  if (validateMove(p->state, in.s, dt, limits_) == MoveVerdict::Ok) {
    p->state = in.s;
    p->lastInputTime = now;
    grid_.upsert(id, in.s.pos);
    return;
  }
  sendCorrection(*p, in.seq, now);
  p->correctionTimes.push_back(now);
  trimWindow(p->correctionTimes, now);
  if (p->correctionTimes.size() > kMaxCorrections) orderKick(*p, "movimento inválido");
}

bool World::onEvent(EntityId id, const cbmp::Event& ev, double now) {
  PlayerSlot* p = slot(id);
  if (!p || p->kicked) return false;
  auto it = p->eventBuckets.find(ev.name);
  if (it == p->eventBuckets.end() && p->eventBuckets.size() < kMaxEventNamesPerPlayer)
    it = p->eventBuckets.try_emplace(ev.name, cfg_.eventsPerSecond, cfg_.eventsBurst).first;
  const bool ok = it != p->eventBuckets.end() && it->second.take(now);
  if (!ok) {
    p->eventViolations.push_back(now);
    trimWindow(p->eventViolations, now);
    if (p->eventViolations.size() > static_cast<size_t>(3 * std::max(1, cfg_.eventsBurst)))
      orderKick(*p, "flood de eventos");
    return false;
  }
  // Sem "dono do jogo" no subprojeto 2: só o eco volta ao remetente; o resto é descartado.
  if (ev.name == "echo") pending_.push_back(Outgoing{id, ev});
  return true;
}

void World::onSnapshotAck(EntityId id, uint32_t tick, double now) {
  PlayerSlot* p = slot(id);
  if (!p || tick <= p->lastAckTick || tick > tick_) return;
  if (p->sent[tick % kSnapshotRing].tick != tick) return;  // nunca enviado ou já saiu do anel
  p->lastAckTick = tick;
  p->lastAckTime = now;
}

void World::tick(double now, SnapshotBuilder& builder, std::vector<Outgoing>& out,
                 std::vector<KickOrder>& kicks) {
  ++tick_;
  for (auto& o : pending_) out.push_back(std::move(o));
  pending_.clear();
  for (auto& k : pendingKicks_) kicks.push_back(std::move(k));
  pendingKicks_.clear();

  auto posOf = [this](EntityId e) -> const cbmp::Vec3i& { return slots_[e]->state.pos; };
  activeSlots_.clear();
  for (EntityId id : active_) {
    PlayerSlot& s = *slots_[id];
    VisibleSet next = computeVisible(id, s.state.pos, s.visible, grid_, posOf, vis_, scratch_);
    const auto& prev = s.visible.ids;
    size_t i = 0, j = 0;
    while (i < prev.size() || j < next.ids.size()) {
      if (j == next.ids.size() || (i < prev.size() && prev[i] < next.ids[j])) {
        const EntityId gone = prev[i++];
        forget(s, gone);  // ao reentrar, volta com kMaskAll
        out.push_back(Outgoing{id, cbmp::EntityLeave{gone}});
      } else if (i == prev.size() || next.ids[j] < prev[i]) {
        const PlayerSlot& o = *slots_[next.ids[j]];
        out.push_back(Outgoing{id, cbmp::EntityEnter{o.id, o.state, o.name}});
        ++j;
      } else {
        ++i;
        ++j;
      }
    }
    s.visible = std::move(next);
    s.baselineFresh = s.lastAckTick != 0 && now - s.lastAckTime <= kBaselineMaxAgeSec;
    activeSlots_.push_back(&s);
  }

  builder.build(tick_, activeSlots_, [this](EntityId e) -> const cbmp::PlayerState& { return slots_[e]->state; },
                out);
}

}  // namespace cbserver
