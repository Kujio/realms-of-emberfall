# libFuzzer targets (Phase 8). Built only with EF_FUZZ=ON under Clang (the "fuzz" preset), together with
# AddressSanitizer and UndefinedBehaviorSanitizer.
add_executable(emberfall-fuzz-protocol fuzz_protocol.cpp)
target_link_libraries(emberfall-fuzz-protocol PRIVATE ef_server_core)
target_compile_options(emberfall-fuzz-protocol PRIVATE -fsanitize=fuzzer)
target_link_options(emberfall-fuzz-protocol PRIVATE -fsanitize=fuzzer)

add_executable(emberfall-fuzz-server fuzz_server.cpp)
target_link_libraries(emberfall-fuzz-server PRIVATE ef_server_core)
target_compile_definitions(emberfall-fuzz-server PRIVATE EF_DEFAULT_CONTENT_DIR="${PROJECT_SOURCE_DIR}/content")
target_compile_options(emberfall-fuzz-server PRIVATE -fsanitize=fuzzer)
target_link_options(emberfall-fuzz-server PRIVATE -fsanitize=fuzzer)
