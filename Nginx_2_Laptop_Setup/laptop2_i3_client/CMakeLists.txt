cmake_minimum_required(VERSION 3.10)
project(KVClient)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(Threads REQUIRED)

set(GEN_DIR "generated")
file(GLOB PROTO_SRCS "${GEN_DIR}/*.cc")

add_executable(kv_client
    load_client.cpp
    ${PROTO_SRCS}
)

target_link_libraries(kv_client grpc++ grpc protobuf OpenSSL::SSL OpenSSL::Crypto Threads::Threads)
