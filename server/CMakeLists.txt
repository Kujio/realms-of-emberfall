# Header-only protocol target shared by the server, the bot, and tests.
add_library(ef_protocol INTERFACE)
target_include_directories(ef_protocol INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/src)

add_library(ef_server_core STATIC
  src/core/config.cpp
  src/core/logging.cpp
  src/core/process_stats.cpp
  src/auth/password.cpp
  src/auth/names.cpp
  src/auth/tickets.cpp
  src/auth/auth_service.cpp
  src/auth/bans.cpp
  src/db/sqlite_repository.cpp
  src/db/db_worker.cpp
  src/game/content.cpp
  src/game/content_world.cpp
  src/game/content_quests.cpp
  src/game/content_social.cpp
  src/game/content_crafts.cpp
  src/game/crafting.cpp
  src/game/chat.cpp
  src/game/groups.cpp
  src/game/guilds.cpp
  src/game/commands.cpp
  src/game/quests.cpp
  src/game/world_geom.cpp
  src/game/dungeon_gen.cpp
  src/game/nav_grid.cpp
  src/game/zone_travel.cpp
  src/game/zone_specials.cpp
  src/game/factions.cpp
  src/game/zone_factions.cpp
  src/game/zone_quests.cpp
  src/game/rules.cpp
  src/game/movement.cpp
  src/game/spatial_hash.cpp
  src/game/zone.cpp
  src/game/zone_combat.cpp
  src/game/zone_mobs.cpp
  src/game/zone_loot.cpp
  src/game/zone_spells.cpp
  src/game/zone_pets.cpp
  src/game/zone_classes.cpp
  src/game/zone_items.cpp
  src/game/zone_trade.cpp
  src/game/zone_social.cpp
  src/game/zone_crafts.cpp
  src/game/items.cpp
  src/game/loot_rules.cpp
  src/game/trade.cpp
  src/game/auction.cpp
  src/game/class_rules.cpp
  src/game/spells.cpp
  src/game/game_server.cpp
  src/game/game_server_social.cpp
  src/game/game_server_admin.cpp
  src/net/enet_transport.cpp
  src/net/packet_crypto.cpp
  src/net/secure_transport.cpp
  src/net/tls.cpp
  src/net/tls_login_server.cpp
  src/http/health_server.cpp)
target_include_directories(ef_server_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(ef_server_core
  PUBLIC ef_build_flags ef_protocol ef_enet nlohmann_json::nlohmann_json spdlog::spdlog
         ef_sqlite3 PkgConfig::SODIUM OpenSSL::SSL OpenSSL::Crypto Threads::Threads)
target_compile_definitions(ef_server_core PUBLIC EF_VERSION="${PROJECT_VERSION}")

add_executable(emberfall-server src/main.cpp)
target_link_libraries(emberfall-server PRIVATE ef_server_core)

install(TARGETS emberfall-server RUNTIME DESTINATION bin)
