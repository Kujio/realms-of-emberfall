#include "game/chat.hpp"

#include <algorithm>

#include <nlohmann/json.hpp>

#include "auth/names.hpp"

namespace ef::game {

namespace {

bool is_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
char lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

}  // namespace

WordFilter::WordFilter(std::vector<std::string> words, char mask) : words_(std::move(words)), mask_(mask) {
  for (auto& w : words_) std::transform(w.begin(), w.end(), w.begin(), lower);
  std::erase_if(words_, [](const std::string& w) { return w.empty(); });
}

bool WordFilter::listed_prefix(std::string_view word) const {
  return std::any_of(words_.begin(), words_.end(), [&](const std::string& w) { return word.starts_with(w); });
}

std::string WordFilter::clean(std::string_view text) const {
  std::string out(text);
  std::string word;
  std::size_t i = 0;
  while (i < out.size()) {
    if (!is_alpha(out[i])) {
      ++i;
      continue;
    }
    const std::size_t start = i;
    word.clear();
    while (i < out.size() && is_alpha(out[i])) word.push_back(lower(out[i++]));
    if (listed_prefix(word)) std::fill(out.begin() + static_cast<std::ptrdiff_t>(start), out.begin() + static_cast<std::ptrdiff_t>(i), mask_);
  }
  return out;
}

bool WordFilter::offensive(std::string_view text) const {
  // Names are run together ("Shitlords"): any listed word anywhere in the letters counts.
  std::string letters;
  for (char c : text)
    if (is_alpha(c)) letters.push_back(lower(c));
  return std::any_of(words_.begin(), words_.end(), [&](const std::string& w) { return letters.find(w) != std::string::npos; });
}

std::unique_ptr<ChatFilter> make_filter(const SocialRules& rules) {
  return std::make_unique<WordFilter>(rules.filter_words, rules.filter_mask.empty() ? '*' : rules.filter_mask.front());
}

ChatGuard::ChatGuard(const SocialRules& rules)
    : window_(static_cast<double>(rules.repeat_window_seconds)), limit_(std::max(1, rules.repeat_limit)) {
  buckets_.reserve(kChatChannelCount);
  for (const auto& r : rules.rates) buckets_.emplace_back(r.rate, r.burst);
}

ChatVerdict ChatGuard::check(ChatChannel channel, std::string_view text, double now) {
  const std::string fp = chat_fingerprint(text);
  while (!recent_.empty() && now - recent_.front().at > window_) recent_.pop_front();
  const auto same = std::count_if(recent_.begin(), recent_.end(), [&](const Recent& r) { return r.text == fp; });
  if (same >= limit_) return ChatVerdict::Repeated;
  const auto id = static_cast<std::size_t>(channel);
  if (id >= buckets_.size() || !buckets_[id].consume(1.0, now)) return ChatVerdict::RateLimited;
  recent_.push_back({fp, now});
  if (recent_.size() > 32) recent_.pop_front();
  return ChatVerdict::Ok;
}

const char* chat_refusal(ChatVerdict v) {
  switch (v) {
    case ChatVerdict::Ok: return "";
    case ChatVerdict::RateLimited: return "You are sending messages too quickly.";
    case ChatVerdict::Repeated: return "You have said that too many times. Say something else.";
  }
  return "";
}

std::string chat_fingerprint(std::string_view text) {
  std::string out;
  bool space = false;
  for (char c : text) {
    if (c == ' ' || c == '\t') {
      space = !out.empty();
      continue;
    }
    if (space) out.push_back(' ');
    space = false;
    out.push_back(lower(c));
  }
  return out;
}

std::string list_name(std::string_view text) {
  std::string n = auth::normalize_character_name(text);
  if (auth::validate_character_name(n)) return {};
  return n;
}

bool has_name(const std::vector<std::string>& list, std::string_view name) {
  return std::binary_search(list.begin(), list.end(), name, std::less<>());
}

bool toggle_name(std::vector<std::string>& list, const std::string& name, int max, bool& added) {
  const auto it = std::lower_bound(list.begin(), list.end(), name);
  if (it != list.end() && *it == name) {
    list.erase(it);
    added = false;
    return true;
  }
  if (static_cast<int>(list.size()) >= max) return false;
  list.insert(it, name);
  added = true;
  return true;
}

std::string SocialLists::to_json() const {
  nlohmann::json j;
  j["friends"] = friends;
  j["ignore"] = ignored;
  j["muted"] = muted;
  return j.dump();
}

SocialLists SocialLists::from_json(std::string_view text, int max_friends, int max_ignored) {
  SocialLists out;
  const auto j = nlohmann::json::parse(text, nullptr, false);
  if (!j.is_object()) return out;
  auto read = [&](const char* key, std::vector<std::string>& into, int max) {
    if (!j.contains(key) || !j.at(key).is_array()) return;
    for (const auto& v : j.at(key)) {
      if (!v.is_string()) continue;
      const std::string n = list_name(v.get<std::string>());
      bool added = false;
      if (!n.empty() && !has_name(into, n)) toggle_name(into, n, max, added);
    }
  };
  read("friends", out.friends, max_friends);
  read("ignore", out.ignored, max_ignored);
  if (j.contains("muted") && j.at("muted").is_number_unsigned())
    out.muted = static_cast<std::uint16_t>(j.at("muted").get<std::uint64_t>() & 0xFFFFu);
  return out;
}

}  // namespace ef::game

