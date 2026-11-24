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
#include <ctime>
#include <memory>
#include <string>

#include <sys/resource.h>

#include <spdlog/spdlog.h>

#include "auth/auth_service.hpp"
#include "auth/names.hpp"
#include "auth/password.hpp"
#include "auth/tickets.hpp"
#include "core/config.hpp"
#include "core/logging.hpp"
#include "core/tick_stats.hpp"
#include "db/db_worker.hpp"
#include "db/sqlite_repository.hpp"
#include "game/content.hpp"
#include "game/game_server.hpp"
#include "http/health_server.hpp"
#include "net/enet_transport.hpp"
#include "net/secure_transport.hpp"
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
      "       emberfall-server [--config <file.json>] --grant-admin USERNAME | --revoke-admin USERNAME\n"
      "         (sets or clears an account's game master flag in the configured database, then exits)\n"
      "       emberfall-server [--config <file.json>] --backup DEST.db\n"
      "         (an online, checked copy of the database; safe while the server runs)\n"
      "       emberfall-server [--config <file.json>] --check-db\n"
      "         (a full integrity check of the database; exit code 0 = healthy)\n"
      "Environment overrides: EMBERFALL_BIND, EMBERFALL_PORT, EMBERFALL_MAX_PLAYERS, EMBERFALL_LOGIN_BIND,\n"
      "  EMBERFALL_LOGIN_PORT, EMBERFALL_TLS_CERT, EMBERFALL_TLS_KEY, EMBERFALL_HEALTH_BIND,\n"
      "  EMBERFALL_HEALTH_PORT, EMBERFALL_DB_PATH, EMBERFALL_CONTENT_PATH, EMBERFALL_AUTOSAVE_SECONDS,\n"
      "  EMBERFALL_TICK_RATE, EMBERFALL_LOG_DIR, EMBERFALL_LOG_LEVEL, EMBERFALL_ADMIN_PASSWORD, EMBERFALL_CONFIG");
}

double seconds_since(std::chrono::steady_clock::time_point origin) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - origin).count();
}

// User plus system CPU time this process has used, in seconds.
double process_cpu_seconds() {
  rusage ru{};
  if (getrusage(RUSAGE_SELF, &ru) != 0) return 0.0;
  auto secs = [](const timeval& tv) { return static_cast<double>(tv.tv_sec) + static_cast<double>(tv.tv_usec) / 1e6; };
  return secs(ru.ru_utime) + secs(ru.ru_stime);
}

}  // namespace

int main(int argc, char** argv) {
  std::string config_path;
  std::string admin_user;  // --grant-admin / --revoke-admin
  bool admin_grant = false;
  bool admin_op = false;
  std::string backup_dest;  // --backup
  bool check_db = false;    // --check-db
  if (const char* env = std::getenv("EMBERFALL_CONFIG")) config_path = env;  // NOLINT(concurrency-mt-unsafe)
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--config" && i + 1 < argc) {
      config_path = argv[++i];
    } else if ((arg == "--grant-admin" || arg == "--revoke-admin") && i + 1 < argc) {
      admin_grant = arg == "--grant-admin";
      admin_op = true;
      admin_user = argv[++i];
      if (admin_user.empty()) {
        std::fprintf(stderr, "%s needs a username\n", arg.c_str());
        return 2;
      }
    } else if (arg == "--backup") {
      if (i + 1 >= argc || argv[i + 1][0] == '\0' || argv[i + 1][0] == '-') {
        std::fprintf(stderr, "--backup needs a destination file (write ./NAME for a name starting with '-')\n");
        return 2;
      }
      backup_dest = argv[++i];
    } else if (arg == "--check-db") {
      check_db = true;
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
  if (check_db || !backup_dest.empty()) {
    // Operator tools: they never start the game (and never migrate a damaged file).
    try {
      if (check_db) {
        ef::db::SqliteRepository repo(cfg.db_path, ef::db::DbCheck::Full, ef::db::OpenMode::Operator);
        std::printf("%s: integrity check passed\n", cfg.db_path.c_str());
      }
      if (!backup_dest.empty()) {
        ef::db::SqliteRepository repo(cfg.db_path, ef::db::DbCheck::Quick, ef::db::OpenMode::Operator);
        repo.backup_to(backup_dest);
        std::printf("%s: backed up to %s (checked)\n", cfg.db_path.c_str(), backup_dest.c_str());
      }
      return 0;
    } catch (const std::exception& e) {
      std::fprintf(stderr, "%s\n", e.what());
      return 1;
    }
  }
  if (admin_op) {
    // Operator tool: flip an account's game master flag and exit (the server need not be running).
    try {
      ef::db::SqliteRepository repo(cfg.db_path);
      const std::string user = ef::auth::normalize_username(admin_user);
      const auto st = repo.set_account_admin(user, admin_grant);
      if (st == ef::db::DbStatus::NotFound) {
        std::fprintf(stderr, "no account named '%s' in %s\n", user.c_str(), cfg.db_path.c_str());
        return 1;
      }
      if (st != ef::db::DbStatus::Ok) {
        std::fprintf(stderr, "could not update the account\n");
        return 1;
      }
      const ef::db::AuditRecord e{static_cast<std::int64_t>(std::time(nullptr)), "(console)", "", admin_grant ? "grant-admin" : "revoke-admin", user,
                                  ""};
      repo.append_audit(e);
      spdlog::info("[audit] (console) {} {}", admin_grant ? "granted game master rights to" : "revoked game master rights from", user);
      std::printf("%s: game master rights %s (takes effect at their next login)\n", user.c_str(), admin_grant ? "granted" : "revoked");
      return 0;
    } catch (const std::exception& e) {
      std::fprintf(stderr, "fatal: %s\n", e.what());
      return 1;
    }
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

    const ef::db::DbCheck check = cfg.db_check == "full" ? ef::db::DbCheck::Full : cfg.db_check == "off" ? ef::db::DbCheck::Off : ef::db::DbCheck::Quick;
    std::unique_ptr<ef::db::SqliteRepository> repo;
    try {
      repo = std::make_unique<ef::db::SqliteRepository>(cfg.db_path, check);
    } catch (const ef::db::DbCorruptError& e) {
      spdlog::critical("{}", e.what());
      spdlog::critical("the server will not start on a damaged database. Restore the latest backup (docs/DEPLOY.md, "
                       "\"Restoring from a backup\"), or move the file aside to start empty.");
      spdlog::shutdown();
      return 3;
    }
    spdlog::info("database: {}", cfg.db_path);
    ef::db::DbWorker db(std::move(repo), ef::db::DbWorker::Mode::Threaded);

    ef::net::EnetConfig ncfg;
    ncfg.bind_address = cfg.bind_address;
    ncfg.port = cfg.port;
    ncfg.max_peers = cfg.max_players + 16;
    ncfg.max_packet_size = cfg.max_packet_size + static_cast<std::uint32_t>(ef::net::kSealOverhead);  // sealed packets are larger
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
    lcfg.attempts_per_second_per_ip = static_cast<double>(cfg.login_attempts_per_minute) / 60.0;
    lcfg.attempt_burst_per_ip = static_cast<double>(cfg.login_attempt_burst);
    ef::net::TlsLoginServer login_server(identity, auth_service, lcfg);
    login_server.start();

    // Packets are sealed after login (Phase 10): the game server sends through `secure`, and ENet
    // delivers to it; it hands the game server plaintext.
    ef::net::SecureTransport secure(transport);
    ef::game::GameServer server(cfg, content, secure, db, tickets, login_server.port(), identity.cert_pem);
    secure.set_handler(server);

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
    // p99 over the last tick_rate x 60 ticks: one minute while the loop keeps up (if it falls behind, the
    // window covers a little longer, which only makes the figure more conservative).
    ef::TickStats tick_stats(static_cast<std::size_t>(cfg.tick_rate) * 60);
    double last_cpu = process_cpu_seconds();
    auto last_window = clock::now();
    std::uint64_t last_sent = 0, last_received = 0;
    ef::game::TickPhases last_phases = server.tick_phases();
    static_assert(ef::game::kTickPhaseCount == ef::http::HealthStats::kPhases);

    while (g_stop_signal == 0) {
      const auto now = clock::now();
      if (now < next_tick) {
        const auto wait_ms = std::chrono::duration_cast<std::chrono::milliseconds>(next_tick - now).count();
        server.set_time(seconds_since(origin));
        transport.poll(static_cast<std::uint32_t>(std::max<long long>(0, wait_ms)), secure);
        continue;
      }

      const auto t0 = clock::now();
      server.set_time(seconds_since(origin));
      transport.poll(0, secure);
      server.tick();
      transport.flush();
      const auto us = static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::microseconds>(clock::now() - t0).count());

      window_us += us;
      window_max_us = std::max(window_max_us, us);
      tick_stats.add(us);
      if (++window_ticks == cfg.tick_rate) {
        // Once a second: tick times, CPU and bandwidth.
        stats.tick_us_avg = static_cast<std::uint32_t>(window_us / window_ticks);
        stats.tick_us_max = window_max_us;
        stats.tick_us_p99 = tick_stats.percentile(99.0);
        const auto now_w = clock::now();
        const double wall = std::chrono::duration<double>(now_w - last_window).count();
        const double cpu = process_cpu_seconds();
        if (wall > 0.0) {
          stats.cpu_permille = static_cast<std::uint32_t>(std::max(0.0, (cpu - last_cpu) / wall * 1000.0));
          stats.send_bps = static_cast<std::uint32_t>(static_cast<double>(transport.bytes_sent() - last_sent) / wall);
          stats.receive_bps = static_cast<std::uint32_t>(static_cast<double>(transport.bytes_received() - last_received) / wall);
        }
        last_cpu = cpu;
        last_window = now_w;
        last_sent = transport.bytes_sent();
        last_received = transport.bytes_received();
        stats.bytes_sent = last_sent;
        stats.bytes_received = last_received;
        stats.zones_loaded = static_cast<std::uint32_t>(server.zones_loaded());
        stats.lingering = static_cast<std::uint32_t>(server.lingering_count());
        const auto phases = server.tick_phases();
        for (std::size_t i = 0; i < ef::game::kTickPhaseCount; ++i)
          stats.phase_us[i] = static_cast<std::uint32_t>((phases[i] - last_phases[i]) / 1000 / std::max<std::uint32_t>(1, window_ticks));
        last_phases = phases;
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
    while (clock::now() < deadline && transport.peer_count() > 0) transport.poll(50, secure);
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
