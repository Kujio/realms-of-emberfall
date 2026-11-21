// emberfall-server: authoritative headless game server.
//
// Threads: main = fixed-rate game loop (network I/O + simulation), DB worker = SQLite +
// Argon2id, health = HTTP stats endpoint. SIGTERM/SIGINT trigger a graceful shutdown that
// saves every character before exiting.

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

#include <spdlog/spdlog.h>

#include "auth/auth_service.hpp"
#include "auth/password.hpp"
#include "auth/tickets.hpp"
#include "core/config.hpp"
#include "core/logging.hpp"
#include "db/db_worker.hpp"
#include "db/sqlite_repository.hpp"
#include "game/content.hpp"
#include "game/game_server.hpp"
#include "http/health_server.hpp"
#include "net/enet_transport.hpp"
#include "net/protocol.gen.hpp"
#include "net/tls.hpp"
#include "net/tls_login_server.hpp"

namespace {

volatile std::sig_atomic_t g_stop_signal = 0;

extern "C" void on_signal(int sig) { g_stop_signal = sig; }

void install_signal_handlers() {
  struct sigaction sa {};
  sa.sa_handler = on_signal;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGTERM, &sa, nullptr);
  sigaction(SIGINT, &sa, nullptr);
  std::signal(SIGPIPE, SIG_IGN);
}

void print_usage() {
  std::puts(
      "Usage: emberfall-server [--config <file.json>] [--version]\n"
      "Environment overrides: EMBERFALL_BIND, EMBERFALL_PORT, EMBERFALL_MAX_PLAYERS, EMBERFALL_LOGIN_BIND,\n"
      "  EMBERFALL_LOGIN_PORT, EMBERFALL_TLS_CERT, EMBERFALL_TLS_KEY, EMBERFALL_HEALTH_BIND,\n"
      "  EMBERFALL_HEALTH_PORT, EMBERFALL_DB_PATH, EMBERFALL_CONTENT_PATH, EMBERFALL_AUTOSAVE_SECONDS,\n"
      "  EMBERFALL_TICK_RATE, EMBERFALL_LOG_DIR, EMBERFALL_LOG_LEVEL, EMBERFALL_ADMIN_PASSWORD, EMBERFALL_CONFIG");
}

double seconds_since(std::chrono::steady_clock::time_point origin) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - origin).count();
}

}  // namespace

int main(int argc, char** argv) {
  std::string config_path;
  if (const char* env = std::getenv("EMBERFALL_CONFIG")) config_path = env;  // NOLINT(concurrency-mt-unsafe)
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--config" && i + 1 < argc) {
      config_path = argv[++i];
    } else if (arg == "--version") {
      std::printf("emberfall-server %s (protocol v%u)\n", EF_VERSION, static_cast<unsigned>(ef::proto::kProtocolVersion));
      return 0;
    } else if (arg == "--help" || arg == "-h") {
      print_usage();
      return 0;
    } else {
      std::fprintf(stderr, "unknown argument: %s\n", arg.c_str());
      print_usage();
      return 2;
    }
  }

  ef::Config cfg;
  try {
    cfg = ef::load_config(config_path, ef::process_env());
    ef::init_logging(cfg);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "fatal: %s\n", e.what());
    return 1;
  }
  spdlog::info("Realms of Emberfall server {} starting (protocol v{}, {} Hz)", EF_VERSION, ef::proto::kProtocolVersion, cfg.tick_rate);

  try {
    if (!ef::auth::init_crypto()) throw std::runtime_error("libsodium initialization failed");

    const ef::game::Content content = ef::game::load_content(cfg.content_path);
    std::size_t spawn_groups = 0;
    for (const auto& z : content.zones) spawn_groups += z.spawns.size();
    spdlog::info("content loaded from '{}': {} races, {} classes, {} cities, {} zones, {} mobs in {} spawn groups, "
                 "{} abilities, {} items, {} loot tables",
                 cfg.content_path, content.races.size(), content.classes.size(), content.cities.size(), content.zones.size(),
                 content.mobs.size(), spawn_groups, content.abilities.size(), content.items.size(), content.loot_tables.size());

    auto repo = std::make_unique<ef::db::SqliteRepository>(cfg.db_path);
    spdlog::info("database: {}", cfg.db_path);
    ef::db::DbWorker db(std::move(repo), ef::db::DbWorker::Mode::Threaded);

    ef::net::EnetConfig ncfg;
    ncfg.bind_address = cfg.bind_address;
    ncfg.port = cfg.port;
    ncfg.max_peers = cfg.max_players + 16;
    ncfg.max_packet_size = cfg.max_packet_size;
    ncfg.max_connections_per_ip = cfg.max_connections_per_ip;
    ncfg.channels = ef::proto::kChannelCount;
    ef::net::EnetTransport transport(ncfg);
    spdlog::info("listening on udp://{}:{}", cfg.bind_address, cfg.port);

    const ef::net::TlsIdentity identity = ef::net::load_or_create_tls_identity(cfg.tls_cert, cfg.tls_key);
    spdlog::info("TLS certificate {} SHA-256 fingerprint {}", cfg.tls_cert, identity.fingerprint);
    spdlog::info("  players verify this fingerprint the first time they connect; publish it where they can see it");

    ef::auth::TicketStore tickets;
    ef::auth::AuthService auth_service(db, tickets, ef::auth::PasswordParams::interactive());
    ef::net::TlsLoginConfig lcfg;
    lcfg.bind_address = cfg.login_bind;
    lcfg.port = cfg.login_port;
    ef::net::TlsLoginServer login_server(identity, auth_service, lcfg);
    login_server.start();

    ef::game::GameServer server(cfg, content, transport, db, tickets, login_server.port(), identity.cert_pem);

    ef::http::HealthStats stats;
    std::unique_ptr<ef::http::HealthServer> health;
    if (cfg.health_port != 0) {
      health = std::make_unique<ef::http::HealthServer>(stats, cfg.tick_rate, identity.fingerprint);
      health->start(cfg.health_bind, cfg.health_port);
    }

    install_signal_handlers();

    using clock = std::chrono::steady_clock;
    const auto origin = clock::now();
    const auto tick_len = std::chrono::nanoseconds(1'000'000'000LL / static_cast<long long>(cfg.tick_rate));
    auto next_tick = clock::now() + tick_len;
    std::uint64_t window_us = 0;
    std::uint32_t window_max_us = 0;
    std::uint32_t window_ticks = 0;

    while (g_stop_signal == 0) {
      const auto now = clock::now();
      if (now < next_tick) {
        const auto wait_ms = std::chrono::duration_cast<std::chrono::milliseconds>(next_tick - now).count();
        server.set_time(seconds_since(origin));
        transport.poll(static_cast<std::uint32_t>(std::max<long long>(0, wait_ms)), server);
        continue;
      }

      const auto t0 = clock::now();
      server.set_time(seconds_since(origin));
      transport.poll(0, server);
      server.tick();
      transport.flush();
      const auto us = static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::microseconds>(clock::now() - t0).count());

      window_us += us;
      window_max_us = std::max(window_max_us, us);
      if (++window_ticks == cfg.tick_rate) {
        stats.tick_us_avg = static_cast<std::uint32_t>(window_us / window_ticks);
        stats.tick_us_max = window_max_us;
        window_us = 0;
        window_max_us = 0;
        window_ticks = 0;
      }
      stats.players_online = static_cast<std::uint32_t>(server.players_in_world());
      stats.sessions = static_cast<std::uint32_t>(server.session_count());
      stats.server_tick = server.server_tick();
      stats.db_queue = static_cast<std::uint32_t>(db.queued_jobs());

      next_tick += tick_len;
      const auto behind = clock::now() - next_tick;
      if (behind > tick_len * 5) {
        spdlog::warn("tick loop fell {} ms behind; skipping ahead",
                     std::chrono::duration_cast<std::chrono::milliseconds>(behind).count());
        next_tick = clock::now() + tick_len;
      }
    }

    spdlog::info("received signal {} ({}); shutting down gracefully", static_cast<int>(g_stop_signal),
                 g_stop_signal == SIGTERM ? "SIGTERM" : "SIGINT");
    stats.shutting_down = true;
    login_server.stop();
    server.shutdown("The server is shutting down. Your character has been saved.");
    transport.flush();
    // Give clients a moment to receive the kick and disconnect cleanly.
    const auto deadline = clock::now() + std::chrono::milliseconds(1500);
    while (clock::now() < deadline && transport.peer_count() > 0) transport.poll(50, server);
    db.stop_and_flush();
    db.drain();
    if (health) health->stop();
    spdlog::info("shutdown complete; all characters saved");
  } catch (const std::exception& e) {
    spdlog::critical("fatal: {}", e.what());
    spdlog::shutdown();
    return 1;
  }
  spdlog::shutdown();
  return 0;
}
