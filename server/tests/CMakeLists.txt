add_executable(emberfall-tests
  test_protocol.cpp
  test_fuzz.cpp
  test_movement.cpp
  test_spatial_hash.cpp
  test_rate_limiter.cpp
  test_auth.cpp
  test_db.cpp
  test_config.cpp
  test_content.cpp
  test_zone.cpp
  test_game_server.cpp
  test_tls.cpp
  test_rules.cpp
  test_ai.cpp
  test_combat.cpp
  test_spells.cpp
  test_spell_zone.cpp
  test_classes.cpp
  test_items.cpp
  test_balance_sim.cpp
  test_world.cpp
  test_dungeons.cpp
  test_factions.cpp
  test_quests.cpp)
target_link_libraries(emberfall-tests PRIVATE ef_server_core ef_balance_sim Catch2::Catch2WithMain)
target_compile_definitions(emberfall-tests PRIVATE EF_CONTENT_DIR="${PROJECT_SOURCE_DIR}/content")

include(Catch)
catch_discover_tests(emberfall-tests)
