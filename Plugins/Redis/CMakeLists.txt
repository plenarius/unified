add_subdirectory(cpp_redis)

# Opt-in client backend: redis-plus-plus on hiredis, both vendored in External/
# and linked statically, so the plugin keeps having no runtime dependency
# beyond OpenSSL (already in the NWNX images for HTTPClient/WebHook).
# Selected at runtime with NWNX_REDIS_CLIENT=redis-plus-plus; the default
# remains cpp_redis.
find_package(OpenSSL)

set(NWNX_HIREDIS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/External/hiredis)
set(NWNX_REDISPP_DIR ${CMAKE_CURRENT_SOURCE_DIR}/External/redis-plus-plus/src/sw/redis++)

add_library(nwnx_hiredis STATIC
    ${NWNX_HIREDIS_DIR}/alloc.c
    ${NWNX_HIREDIS_DIR}/async.c
    ${NWNX_HIREDIS_DIR}/hiredis.c
    ${NWNX_HIREDIS_DIR}/net.c
    ${NWNX_HIREDIS_DIR}/read.c
    ${NWNX_HIREDIS_DIR}/sds.c
    ${NWNX_HIREDIS_DIR}/sockcompat.c)
set_target_properties(nwnx_hiredis PROPERTIES C_STANDARD 99 POSITION_INDEPENDENT_CODE ON)
target_compile_options(nwnx_hiredis PRIVATE -w)

set(NWNX_REDISPP_SOURCES
    ${NWNX_REDISPP_DIR}/command.cpp
    ${NWNX_REDISPP_DIR}/command_options.cpp
    ${NWNX_REDISPP_DIR}/connection.cpp
    ${NWNX_REDISPP_DIR}/connection_pool.cpp
    ${NWNX_REDISPP_DIR}/crc16.cpp
    ${NWNX_REDISPP_DIR}/errors.cpp
    ${NWNX_REDISPP_DIR}/pipeline.cpp
    ${NWNX_REDISPP_DIR}/redis.cpp
    ${NWNX_REDISPP_DIR}/redis_cluster.cpp
    ${NWNX_REDISPP_DIR}/redis_uri.cpp
    ${NWNX_REDISPP_DIR}/reply.cpp
    ${NWNX_REDISPP_DIR}/sentinel.cpp
    ${NWNX_REDISPP_DIR}/shards.cpp
    ${NWNX_REDISPP_DIR}/shards_pool.cpp
    ${NWNX_REDISPP_DIR}/subscriber.cpp
    ${NWNX_REDISPP_DIR}/transaction.cpp
    ${NWNX_REDISPP_DIR}/patterns/redlock.cpp)

if (OPENSSL_FOUND)
    target_sources(nwnx_hiredis PRIVATE ${NWNX_HIREDIS_DIR}/ssl.c)
    target_include_directories(nwnx_hiredis PRIVATE ${OPENSSL_INCLUDE_DIR})
    target_link_libraries(nwnx_hiredis PUBLIC ${OPENSSL_LIBRARIES})
    list(APPEND NWNX_REDISPP_SOURCES ${NWNX_REDISPP_DIR}/tls/sw/redis++/tls.cpp)
    set(NWNX_REDISPP_TLS_DIR ${NWNX_REDISPP_DIR}/tls)
else()
    message(WARNING "Redis: OpenSSL not found, the redis-plus-plus client is built without TLS")
    set(NWNX_REDISPP_TLS_DIR ${NWNX_REDISPP_DIR}/no_tls)
endif()

set(REDIS_PLUS_PLUS_HAS_redisEnableKeepAliveWithInterval ON)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/External/redis-plus-plus/hiredis_features.h.in
               ${CMAKE_CURRENT_BINARY_DIR}/redispp-gen/sw/redis++/hiredis_features.h)

add_library(nwnx_redispp STATIC ${NWNX_REDISPP_SOURCES})
set_target_properties(nwnx_redispp PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_options(nwnx_redispp PRIVATE -w)
target_include_directories(nwnx_redispp PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/External
    ${CMAKE_CURRENT_SOURCE_DIR}/External/redis-plus-plus/src
    ${NWNX_REDISPP_DIR}/cxx17
    ${NWNX_REDISPP_TLS_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}/redispp-gen)
target_link_libraries(nwnx_redispp PUBLIC nwnx_hiredis)

add_plugin(Redis
  Redis.cpp
  NWScript.cpp
  Config.cpp
  Connection.cpp
  PubSub.cpp
  ModernClient.cpp
)

target_include_directories(Redis PRIVATE cpp_redis/includes)
target_link_libraries(Redis cpp_redis nwnx_redispp)
