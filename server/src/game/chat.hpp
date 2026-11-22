// Chat rules (Phase 7), pure and unit-tested: the profanity/spam filter hook, per-channel rate limits
// with repeat (spam) detection, and ignore-list name handling. The game server routes messages.
#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "game/social_types.hpp"
#include "net/rate_limiter.hpp"

namespace ef::game {

// The filter hook: every player-written line (chat, guild names, MOTDs) passes through one. Servers can
// swap in their own (a service call, a bigger list) without touching the routing.
class ChatFilter {
 public:
  virtual ~ChatFilter() = default;
  ChatFilter() = default;
  ChatFilter(const ChatFilter&) = delete;
  ChatFilter& operator=(const ChatFilter&) = delete;
  // The text to deliver (masked where needed).
  virtual std::string clean(std::string_view text) const = 0;
  // True when the text holds something the filter would mask (names must not).
  virtual bool offensive(std::string_view text) const = 0;
};

// Masks every word that starts with a listed word (ASCII, case-insensitive): "Shitty" -> "******", but a
// listed word inside another word ("scrapshit") is left alone, so place names do not trip it.
class WordFilter final : public ChatFilter {
 public:
  WordFilter(std::vector<std::string> words, char mask);
  std::string clean(std::string_view text) const override;
  bool offensive(std::string_view text) const override;

 private:
  bool listed_prefix(std::string_view lower_word) const;
  std::vector<std::string> words_;  // lower case
  char mask_;
};

std::unique_ptr<ChatFilter> make_filter(const SocialRules& rules);

enum class ChatVerdict : std::uint8_t { Ok, RateLimited, Repeated };

// One player's chat budget: a token bucket per channel, and a short memory of what they said lately
// (a line said repeat_limit times within repeat_window_seconds is refused after that, whatever the channel).
class ChatGuard {
 public:
  explicit ChatGuard(const SocialRules& rules);
  // Records the message when it is allowed. `now` is monotonic seconds.
  ChatVerdict check(ChatChannel channel, std::string_view text, double now);

 private:
  struct Recent {
    std::string text;  // lower-cased, spaces collapsed
    double at = 0.0;
  };
  std::vector<net::TokenBucket> buckets_;  // by channel id
  std::deque<Recent> recent_;
  double window_;
  int limit_;
};

// What a refusal tells the player.
const char* chat_refusal(ChatVerdict v);

// "  hello   THERE " -> "hello there" (for repeat detection).
std::string chat_fingerprint(std::string_view text);

// Character names on lists (friends, ignore) are stored as the game shows them: "aLDRIC" -> "Aldric".
// Returns "" when the text cannot be a character name.
std::string list_name(std::string_view text);

// Adds or removes `name` from a sorted list (friends, ignore). Returns false when adding would pass `max`.
// `added` says which way it went.
bool toggle_name(std::vector<std::string>& list, const std::string& name, int max, bool& added);
bool has_name(const std::vector<std::string>& list, std::string_view name);

// One character's friends, ignore list and the channels they turned off (saved with the character).
struct SocialLists {
  std::vector<std::string> friends;  // sorted
  std::vector<std::string> ignored;  // sorted
  std::uint16_t muted = 0;           // chat_bit per channel
  bool ignores(std::string_view name) const { return has_name(ignored, name); }
  bool hears(ChatChannel c) const { return (muted & chat_bit(c)) == 0; }
  std::string to_json() const;
  // Unreadable or malformed text gives empty lists; names that are not character names are dropped.
  static SocialLists from_json(std::string_view text, int max_friends, int max_ignored);
};

}  // namespace ef::game
