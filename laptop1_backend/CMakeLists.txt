cmake_minimum_required(VERSION 3.10)
project(KVServerBackend)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(Threads REQUIRED)

set(GEN_DIR "generated")
file(GLOB PROTO_SRCS "${GEN_DIR}/*.cc")

add_executable(proxy_backend_server
    proxy_backend_server.cpp
    ${PROTO_SRCS}
)

target_link_libraries(proxy_backend_server grpc++ grpc protobuf rocksdb OpenSSL::SSL OpenSSL::Crypto Threads::Threads)
