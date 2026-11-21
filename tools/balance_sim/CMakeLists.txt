# Balance simulator v1: a library (shared with the tests) and the emberfall-balance-sim tool.
add_library(ef_balance_sim STATIC balance_sim.cpp)
target_include_directories(ef_balance_sim PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ef_balance_sim PUBLIC ef_server_core)

add_executable(emberfall-balance-sim main.cpp)
target_link_libraries(emberfall-balance-sim PRIVATE ef_balance_sim)
target_compile_definitions(emberfall-balance-sim PRIVATE EF_DEFAULT_CONTENT_DIR="${PROJECT_SOURCE_DIR}/content")
