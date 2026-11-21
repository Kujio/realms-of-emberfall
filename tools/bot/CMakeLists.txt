add_executable(emberfall-bot main.cpp)
# Links the server core for the shared TLS client and protocol code.
target_link_libraries(emberfall-bot PRIVATE ef_server_core)
