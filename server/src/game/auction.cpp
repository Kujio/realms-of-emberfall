#include "game/auction.hpp"

#include <algorithm>
#include <cctype>

namespace ef::game {

namespace {

std::string lower(std::string_view s) {
  std::string out(s);
  for (char& ch : out) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  return out;
}

}  // namespace

void AuctionHouse::load(std::vector<AuctionListing> listings) {
  listings_.clear();
  for (auto& l : listings) {
    const std::int64_t id = l.id;
    listings_.emplace(id, std::move(l));
  }
}

AuctionListing* AuctionHouse::find(std::int64_t id) {
  auto it = listings_.find(id);
  return it == listings_.end() ? nullptr : &it->second;
}

const AuctionListing* AuctionHouse::find(std::int64_t id) const {
  auto it = listings_.find(id);
  return it == listings_.end() ? nullptr : &it->second;
}

int AuctionHouse::count_for(std::int64_t seller) const {
  return static_cast<int>(std::count_if(listings_.begin(), listings_.end(), [&](const auto& kv) { return kv.second.seller_char == seller; }));
}

AuctionListing& AuctionHouse::add(AuctionListing l) {
  const std::int64_t id = l.id;
  return listings_.insert_or_assign(id, std::move(l)).first->second;
}

void AuctionHouse::remove(std::int64_t id) { listings_.erase(id); }

std::vector<AuctionListing*> AuctionHouse::expire(std::int64_t now) {
  std::vector<AuctionListing*> out;
  for (auto& [id, l] : listings_)
    if (l.state == ListingState::Active && l.expires_at <= now) {
      l.state = ListingState::Expired;
      out.push_back(&l);
    }
  return out;
}

std::vector<AuctionListing*> AuctionHouse::collectable(std::int64_t seller) {
  std::vector<AuctionListing*> out;
  for (auto& [id, l] : listings_)
    if (l.seller_char == seller && l.state != ListingState::Active) out.push_back(&l);
  return out;
}

AuctionPage AuctionHouse::search(const AuctionQuery& q, const Content& c, int page_size) const {
  AuctionPage page;
  const std::string needle = lower(q.text);
  std::vector<const AuctionListing*> hits;
  for (const auto& [id, l] : listings_) {
    if (q.mine != 0) {
      if (l.seller_char == q.mine) hits.push_back(&l);
      continue;
    }
    if (l.state != ListingState::Active) continue;
    const ItemDef* d = c.find_item(l.item_id);
    if (!d) continue;
    if (!q.kind.empty() && d->kind != q.kind) continue;
    if (d->req_level < q.min_level || d->req_level > q.max_level) continue;
    if (!needle.empty() && lower(d->name).find(needle) == std::string::npos) continue;
    hits.push_back(&l);
  }
  std::stable_sort(hits.begin(), hits.end(), [&](const AuctionListing* a, const AuctionListing* b) {
    if (q.mine != 0) return a->id < b->id;
    return a->price != b->price ? a->price < b->price : a->id < b->id;
  });
  page.total = hits.size();
  const std::size_t size = static_cast<std::size_t>(std::max(1, page_size));
  const std::size_t first = static_cast<std::size_t>(std::max(0, q.page)) * size;
  for (std::size_t i = first; i < hits.size() && page.rows.size() < size; ++i) page.rows.push_back(hits[i]);
  return page;
}

}  // namespace ef::game
