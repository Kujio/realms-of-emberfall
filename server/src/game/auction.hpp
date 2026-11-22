// The player auction board: listings in memory, owned by the game server and mirrored to the database
// (every change is saved together with the character it affects). Pure: no I/O and no entities.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "game/content.hpp"

namespace ef::game {

enum class ListingState : std::uint8_t { Active = 0, Sold = 1, Expired = 2 };

struct AuctionListing {
  std::int64_t id = 0;
  std::int64_t seller_char = 0;
  std::string seller_name;
  std::string item_id;
  int count = 1;
  std::uint64_t price = 0;     // copper, for the whole stack
  std::int64_t listed_at = 0;  // unix seconds
  std::int64_t expires_at = 0;
  ListingState state = ListingState::Active;
  std::string buyer;  // sold listings
};

struct AuctionQuery {
  std::string text;       // case-insensitive part of the item name ("" = any)
  std::string kind;       // item kind ("" = any)
  int min_level = 0;      // item req_level range
  int max_level = 255;
  int page = 0;
  std::int64_t mine = 0;  // non-zero: this seller's listings in every state, instead of the open board
};

struct AuctionPage {
  std::vector<const AuctionListing*> rows;
  std::size_t total = 0;  // matches across every page
};

class AuctionHouse {
 public:
  void load(std::vector<AuctionListing> listings);
  AuctionListing* find(std::int64_t id);
  const AuctionListing* find(std::int64_t id) const;
  // Listings a character has on the board in any state (they count against the cap until collected).
  int count_for(std::int64_t seller) const;
  AuctionListing& add(AuctionListing l);
  void remove(std::int64_t id);
  // Active listings whose time ran out by `now` become Expired; returns them.
  std::vector<AuctionListing*> expire(std::int64_t now);
  // Sold and expired listings waiting for their seller.
  std::vector<AuctionListing*> collectable(std::int64_t seller);
  // Open listings matching the query (or the seller's own), cheapest first, one page.
  AuctionPage search(const AuctionQuery& q, const Content& c, int page_size) const;
  std::size_t size() const { return listings_.size(); }

 private:
  std::map<std::int64_t, AuctionListing> listings_;
};

}  // namespace ef::game
