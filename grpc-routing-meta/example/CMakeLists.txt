cmake_minimum_required(VERSION 3.16)
project(grpc_routing_meta LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)   # for clangd / IntelliSense

# ---------------------------------------------------------------------------
# Protobuf required; libprotoc needed to build the codegen plugin. gRPC is
# OPTIONAL (generated code writes into routingmeta::MetadataSink). build.sh is a
# no-cmake fallback that mirrors these same steps.
# ---------------------------------------------------------------------------
find_package(Protobuf REQUIRED)
message(STATUS "Protobuf: ${Protobuf_VERSION}")

find_package(Threads REQUIRED)

find_library(PROTOC_LIB NAMES protoc libprotoc
  HINTS ${Protobuf_LIBRARY_DIRS} /usr/lib /usr/lib/x86_64-linux-gnu /usr/local/lib)
if(NOT PROTOC_LIB)
  message(FATAL_ERROR "libprotoc not found. Install libprotoc-dev.")
endif()

set(GEN_DIR ${CMAKE_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${GEN_DIR})
set(PROTO_ROOT ${CMAKE_SOURCE_DIR}/proto)   # all protos (contract + systems) live here
set(IPROTO -I ${PROTO_ROOT})
set(PROTOC ${Protobuf_PROTOC_EXECUTABLE})

# ===========================================================================
# 1. Contract protos -> C++ (no ProjectMeta). Add a shared message by extending
#    this list; add a new system below.
# ===========================================================================
foreach(name metadata_options process_context)
  add_custom_command(
    OUTPUT ${GEN_DIR}/${name}.pb.cc ${GEN_DIR}/${name}.pb.h
    COMMAND ${PROTOC} ${IPROTO} --cpp_out=${GEN_DIR} ${PROTO_ROOT}/${name}.proto
    DEPENDS ${PROTO_ROOT}/${name}.proto
    COMMENT "protoc: ${name}.proto -> C++")
endforeach()

# ===========================================================================
# 2. The codegen plugin (links libprotoc + the compiled options).
# ===========================================================================
add_executable(protoc-gen-meta
  ${CMAKE_SOURCE_DIR}/src/plugin/protoc-gen-meta.cc
  ${GEN_DIR}/metadata_options.pb.cc)
target_include_directories(protoc-gen-meta PRIVATE ${GEN_DIR} ${Protobuf_INCLUDE_DIRS})
target_link_libraries(protoc-gen-meta PRIVATE ${PROTOC_LIB} ${Protobuf_LIBRARIES})

# ===========================================================================
# 3. Per-system protos: messages (--cpp_out) + ProjectMeta (--meta_out).
#    Add a new system by appending its stem to the foreach list.
# ===========================================================================
set(GEN_SRCS ${GEN_DIR}/metadata_options.pb.cc ${GEN_DIR}/process_context.pb.cc)
foreach(name sys1 sys2 sys3)
  add_custom_command(
    OUTPUT ${GEN_DIR}/${name}.pb.cc ${GEN_DIR}/${name}.pb.h
           ${GEN_DIR}/${name}.proj.cc ${GEN_DIR}/${name}.proj.h
    COMMAND ${PROTOC} ${IPROTO} --cpp_out=${GEN_DIR} ${PROTO_ROOT}/${name}.proto
    COMMAND ${PROTOC} ${IPROTO}
            --plugin=protoc-gen-meta=$<TARGET_FILE:protoc-gen-meta>
            --meta_out=${GEN_DIR} ${PROTO_ROOT}/${name}.proto
    DEPENDS ${PROTO_ROOT}/${name}.proto protoc-gen-meta
            ${GEN_DIR}/process_context.pb.h ${GEN_DIR}/metadata_options.pb.h
    COMMENT "protoc + protoc-gen-meta: ${name}.proto -> messages + ProjectMeta")
  list(APPEND GEN_SRCS ${GEN_DIR}/${name}.pb.cc ${GEN_DIR}/${name}.proj.cc)
endforeach()

# metadata_options.pb.cc / process_context.pb.cc are included exactly once: their
# descriptors are referenced by every system's messages.
add_library(routing_meta_gen STATIC ${GEN_SRCS})
target_include_directories(routing_meta_gen PUBLIC
  ${GEN_DIR} ${CMAKE_SOURCE_DIR}/src ${Protobuf_INCLUDE_DIRS})
target_link_libraries(routing_meta_gen PUBLIC ${Protobuf_LIBRARIES})

if(ROUTINGMETA_WITH_GRPC)
  find_package(gRPC CONFIG QUIET)
  if(gRPC_FOUND)
    target_compile_definitions(routing_meta_gen PUBLIC ROUTINGMETA_WITH_GRPC)
    target_link_libraries(routing_meta_gen PUBLIC gRPC::grpc++)
  else()
    message(WARNING "ROUTINGMETA_WITH_GRPC set but gRPC not found; building without it.")
  endif()
endif()

# ===========================================================================
# 4. Apps + tests.
# ===========================================================================
add_executable(unified_sender sender/unified_sender.cc)
target_link_libraries(unified_sender PRIVATE routing_meta_gen)

add_executable(receiver_verify receiver/receiver_verify.cc)
target_link_libraries(receiver_verify PRIVATE routing_meta_gen)

enable_testing()
add_executable(test_projection tests/test_projection.cc)
target_link_libraries(test_projection PRIVATE routing_meta_gen Threads::Threads)
add_test(NAME projection COMMAND test_projection)
add_executable(bench_projection tests/bench_projection.cc)
target_link_libraries(bench_projection PRIVATE routing_meta_gen)
add_test(NAME bench COMMAND bench_projection)
