# Third-party dependencies.
#
# Strategy (see docs/DECISIONS.md): FetchContent with pinned versions and SHA-256 hashes
# for everything except libsodium and OpenSSL, which come from the system packages
# (libsodium-dev + libssl-dev on Debian/Ubuntu; dev-libs/libsodium + dev-libs/openssl on Gentoo).

include(FetchContent)
set(FETCHCONTENT_QUIET ON)

# Marks a third-party target's include dirs as SYSTEM so its headers don't trip our -Werror.
function(ef_mark_system target)
  get_target_property(_inc ${target} INTERFACE_INCLUDE_DIRECTORIES)
  if(_inc)
    set_target_properties(${target} PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_inc}")
  endif()
endfunction()

# --- ENet 1.3.18 (built from source with our own target; its CMakeLists predates CMake 3.5)
FetchContent_Declare(enet
  URL https://github.com/lsalzman/enet/archive/refs/tags/v1.3.18.tar.gz
  URL_HASH SHA256=28603c895f9ed24a846478180ee72c7376b39b4bb1287b73877e5eae7d96b0dd
  SOURCE_SUBDIR do-not-add-subdirectory)
FetchContent_MakeAvailable(enet)
add_library(ef_enet STATIC
  ${enet_SOURCE_DIR}/callbacks.c ${enet_SOURCE_DIR}/compress.c ${enet_SOURCE_DIR}/host.c
  ${enet_SOURCE_DIR}/list.c ${enet_SOURCE_DIR}/packet.c ${enet_SOURCE_DIR}/peer.c
  ${enet_SOURCE_DIR}/protocol.c ${enet_SOURCE_DIR}/unix.c)
target_include_directories(ef_enet SYSTEM PUBLIC ${enet_SOURCE_DIR}/include)
target_compile_definitions(ef_enet PRIVATE
  HAS_GETADDRINFO=1 HAS_GETNAMEINFO=1 HAS_GETHOSTBYNAME_R=1 HAS_GETHOSTBYADDR_R=1
  HAS_POLL=1 HAS_FCNTL=1 HAS_INET_PTON=1 HAS_INET_NTOP=1 HAS_MSGHDR_FLAGS=1 HAS_SOCKLEN_T=1)
target_compile_options(ef_enet PRIVATE -w)
set_target_properties(ef_enet PROPERTIES POSITION_INDEPENDENT_CODE ON)

# --- nlohmann/json 3.12.0
FetchContent_Declare(nlohmann_json
  URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
  URL_HASH SHA256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa)
set(JSON_BuildTests OFF CACHE INTERNAL "")
set(JSON_Install OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(nlohmann_json)
ef_mark_system(nlohmann_json)

# --- spdlog 1.15.3 (bundled fmt)
FetchContent_Declare(spdlog
  URL https://github.com/gabime/spdlog/archive/refs/tags/v1.15.3.tar.gz
  URL_HASH SHA256=15a04e69c222eb6c01094b5c7ff8a249b36bb22788d72519646fb85feb267e67)
set(SPDLOG_BUILD_SHARED OFF CACHE INTERNAL "")
set(SPDLOG_INSTALL OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(spdlog)
ef_mark_system(spdlog)
target_compile_options(spdlog PRIVATE -w)

# --- SQLite 3.50.4 amalgamation
FetchContent_Declare(sqlite3
  URL https://www.sqlite.org/2025/sqlite-amalgamation-3500400.zip
  URL_HASH SHA256=1d3049dd0f830a025a53105fc79fd2ab9431aea99e137809d064d8ee8356b032
  SOURCE_SUBDIR do-not-add-subdirectory)
FetchContent_MakeAvailable(sqlite3)
add_library(ef_sqlite3 STATIC ${sqlite3_SOURCE_DIR}/sqlite3.c)
target_include_directories(ef_sqlite3 SYSTEM PUBLIC ${sqlite3_SOURCE_DIR})
target_compile_definitions(ef_sqlite3 PRIVATE
  SQLITE_THREADSAFE=1 SQLITE_DEFAULT_WAL_SYNCHRONOUS=1 SQLITE_OMIT_LOAD_EXTENSION=1 SQLITE_DQS=0)
target_compile_options(ef_sqlite3 PRIVATE -w)
find_package(Threads REQUIRED)
target_link_libraries(ef_sqlite3 PUBLIC Threads::Threads ${CMAKE_DL_LIBS} m)

# --- Catch2 3.8.1
if(EF_BUILD_TESTS)
  FetchContent_Declare(Catch2
    URL https://github.com/catchorg/Catch2/archive/refs/tags/v3.8.1.tar.gz
    URL_HASH SHA256=18b3f70ac80fccc340d8c6ff0f339b2ae64944782f8d2fca2bd705cf47cadb79)
  FetchContent_MakeAvailable(Catch2)
  ef_mark_system(Catch2)
  ef_mark_system(Catch2WithMain)
  list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
endif()

# --- libsodium (system)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SODIUM REQUIRED IMPORTED_TARGET libsodium>=1.0.18)

# --- OpenSSL 3 (system): TLS for the login service. Chosen over hand-rolled crypto; see DECISIONS.md.
find_package(OpenSSL 3.0 REQUIRED)
