// Travel NPCs (Phase 6): teleport stones whisk you to another hub for a fee, ferries sail after a short
// boarding wait, and soulbinders set your bind point. Pets that follow their owner between zones
// are summoned again on arrival.
#include <algorithm>
#include <cmath>

#include "game/zone.hpp"

namespace ef::game {

namespace {

std::string dest_name(const Content& c, const std::string& dest) {
  if (dest == "bind") return "Bind your soul here";
  if (const CityDef* city = c.find_city(dest)) return city->name;
  if (const TravelPointDef* t = c.find_travel_point(dest)) return t->name;
  return dest;
}

bool travel_role(const std::string& role) { return role == "teleporter" || role == "ferry" || role == "binder"; }

}  // namespace

void Zone::travel_open(EntityId player, EntityId npc, ZoneSink& sink) {
  PlayerEntity* pp = find(player);
  if (!pp) return;
  PlayerEntity& p = *pp;
  if (p.dead) return send(sink, p, proto::ChatMessage{kChatSystem, "", "You are dead."});
  auto it = npcs_.find(npc);
  if (it == npcs_.end() || !travel_role(it->second.def->role) || !std::binary_search(p.known.begin(), p.known.end(), npc))
    return send(sink, p, proto::ChatMessage{kChatSystem, "", "That offers no passage."});
  const NpcEntity* n = npc_in_reach(p, npc, nullptr);
  if (!n) return send(sink, p, proto::ChatMessage{kChatSystem, "", "You are too far away from " + it->second.def->name + "."});
  proto::TravelOptions o;
  o.npc_id = npc;
  o.title = n->def->name;
  o.role = n->def->role;
  o.text = n->def->greeting;
  for (const auto& t : n->def->travel) {
    proto::TravelChoice ch;
    ch.id = t.dest;
    ch.name = dest_name(content_, t.dest);
    ch.fee = static_cast<std::uint32_t>(std::max(0, t.fee));
    ch.delay_ms = static_cast<std::uint32_t>(std::lround(std::max(0.0f, t.delay) * 1000.0f));
    o.options.push_back(std::move(ch));
    if (o.options.size() >= 16) break;
  }
  send(sink, p, o);
}

void Zone::travel_choose(EntityId player, EntityId npc, std::string_view dest, ZoneSink& sink) {
  PlayerEntity* pp = find(player);
  if (!pp) return;
  PlayerEntity& p = *pp;
  auto say = [&](const std::string& text) { send(sink, p, proto::ChatMessage{kChatSystem, "", text}); };
  if (p.dead) return say("You are dead.");
  if (p.transferring) return;
  if (p.trade != 0) return say("You cannot travel while trading.");
  const NpcEntity* n = npc_in_reach(p, npc, nullptr);
  if (!n || !travel_role(n->def->role)) return say("You are too far away.");
  const TravelOptionDef* opt = nullptr;
  for (const auto& t : n->def->travel)
    if (t.dest == dest) opt = &t;
  if (!opt) return say(n->def->name + " does not go there.");
  if (opt->dest == "bind") {
    p.bind_zone = def_.id;
    p.bind_pos = p.pos;
    p.bind_yaw = p.yaw;
    p.save_dirty = true;
    sink.zone_character_changed(p);
    return say("You feel yourself bound to this place.");
  }
  if (in_combat(p)) return say("You cannot travel while you are fighting.");
  const auto fee = static_cast<std::uint64_t>(std::max(0, opt->fee));
  if (p.coin < fee) return say("The passage costs " + format_coin(fee) + ", which you do not have.");
  Destination to;
  if (!resolve_destination(p, opt->dest, to)) return say("That place cannot be reached.");
  if (opt->delay > 0.0f) {
    // Board now, pay when the ferry sails (if you are still aboard).
    if (p.traveling_npc != 0) return say("You are already waiting for passage.");
    p.traveling_npc = npc;
    departures_.push_back({p.id, npc, opt->dest, opt->fee, tick_ + std::max<std::uint32_t>(1, ticks(opt->delay))});
    return say("You board the ferry for " + dest_name(content_, opt->dest) + ". It sails in " +
               std::to_string(static_cast<int>(std::lround(opt->delay))) + " seconds; stay aboard.");
  }
  p.coin -= fee;
  p.save_dirty = true;
  if (fee > 0) say("You pay " + format_coin(fee) + ".");
  say("The world folds around you...");
  move_player_to(p, to, sink, "travel");
}

void Zone::tick_travel(ZoneSink& sink) {
  if (departures_.empty()) return;
  std::vector<Departure> due;
  std::erase_if(departures_, [&](const Departure& d) {
    if (d.at_tick > tick_) return false;
    due.push_back(d);
    return true;
  });
  for (const auto& d : due) {
    PlayerEntity* p = find(d.player);
    if (!p) continue;
    p->traveling_npc = 0;
    auto say = [&](const std::string& text) { send(sink, *p, proto::ChatMessage{kChatSystem, "", text}); };
    const NpcEntity* n = npc_in_reach(*p, d.npc, "ferry");
    if (!n || p->dead || p->transferring || p->trade != 0) {
      if (!p->transferring) say("The ferry sails without you.");
      continue;
    }
    const auto fee = static_cast<std::uint64_t>(std::max(0, d.fee));
    Destination to;
    if (p->coin < fee || !resolve_destination(*p, d.dest, to)) {
      say("The ferry sails without you.");
      continue;
    }
    p->coin -= fee;
    p->save_dirty = true;
    if (fee > 0) say("You pay the fare of " + format_coin(fee) + ".");
    say("The ferry casts off and carries you across the water.");
    move_player_to(*p, to, sink, "ferry");
  }
}

MobEntity* Zone::spawn_pet_for(PlayerEntity& p, const PetDef& def, int level) {
  MobDef md = make_pet_def(def, std::clamp(level, 1, rules_.max_level), p.name);
  if (const ModelDef* model = content_.find_model(md.model)) md.collision_radius = model->collision_radius;
  // The pet appears two metres to its owner's right.
  const Vec3 f = forward_of(p.yaw);
  const Vec3 at{p.pos.x - f.z * 2.0f, p.pos.y, p.pos.z + f.x * 2.0f};
  const EntityId id = spawn_mob(md, at);
  MobEntity* m = find_mob(id);
  if (!m) return nullptr;
  m->owner = p.id;
  m->owner_char = p.char_id;
  m->pet_def = &def;
  m->pet_mode = PetMode::Follow;
  m->guard_pos = m->pos;
  m->yaw = p.yaw;
  p.pet = id;
  return m;
}

void Zone::restore_pet(EntityId player, const std::string& pet_id, int level, float hp_fraction, ZoneSink& sink) {
  PlayerEntity* p = find(player);
  const PetDef* def = content_.find_pet(pet_id);
  if (!p || !def || p->dead || (p->pet != 0 && find_mob(p->pet))) return;
  MobEntity* m = spawn_pet_for(*p, *def, level);
  if (!m) return;
  m->hp = std::clamp(static_cast<int>(std::lround(static_cast<float>(m->stats.max_hp) * hp_fraction)), 1, m->stats.max_hp);
  combat_text(*p, "Your " + def->name + " follows you.", sink);
  send_pet_status(*p, sink, true);
}

}  // namespace ef::game
