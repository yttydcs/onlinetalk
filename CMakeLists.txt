cmake_minimum_required(VERSION 3.16)

project(onlinetalk LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ONLINETALK_WARNINGS_AS_ERRORS "Treat warnings as errors" OFF)

if(MSVC)
  add_compile_options(/W4)
  if(ONLINETALK_WARNINGS_AS_ERRORS)
    add_compile_options(/WX)
  endif()
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
  if(ONLINETALK_WARNINGS_AS_ERRORS)
    add_compile_options(-Werror)
  endif()
endif()

find_package(Threads REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(nlohmann_json REQUIRED)

find_package(SDL2 REQUIRED)
find_package(SDL2_ttf REQUIRED)

find_library(CRYPT_LIBRARY crypt)
if(NOT CRYPT_LIBRARY)
  find_library(CRYPT_LIBRARY xcrypt)
endif()
if(NOT CRYPT_LIBRARY)
  message(FATAL_ERROR "libcrypt (crypt) not found. Install libxcrypt-dev or libcrypt-dev.")
endif()

add_library(onlinetalk_common
  src/common/config.cpp
  src/common/crypto/sha256.cpp
  src/common/fs.cpp
  src/common/log.cpp
  src/common/net/byte_buffer.cpp
  src/common/protocol/codec.cpp
)

target_include_directories(onlinetalk_common PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(onlinetalk_common
  PUBLIC
    nlohmann_json::nlohmann_json
)

add_executable(onlinetalk_server
  src/server/main.cpp
  src/server/net/connection.cpp
  src/server/net/tcp_server.cpp
  src/server/services/auth_service.cpp
  src/server/services/file_service.cpp
  src/server/services/group_service.cpp
  src/server/services/id_generator.cpp
  src/server/services/message_service.cpp
  src/server/session/session_manager.cpp
  src/server/storage/database.cpp
)

target_link_libraries(onlinetalk_server
  PRIVATE
    onlinetalk_common
    SQLite::SQLite3
    OpenSSL::Crypto
    Threads::Threads
    ${CRYPT_LIBRARY}
)

target_compile_definitions(onlinetalk_server PRIVATE _GNU_SOURCE)

add_executable(onlinetalk_client
  src/client/main.cpp
)

target_link_libraries(onlinetalk_client
  PRIVATE
    onlinetalk_common
    SDL2::SDL2
    SDL2_ttf::SDL2_ttf
    OpenSSL::Crypto
    Threads::Threads
)
