#include <iostream>
#include <vector>
#include <thread>
#include <chrono>
#include <atomic>
#include <csignal>

#include <rocksdb/db.h>
#include <rocksdb/options.h>
#include <rocksdb/utilities/optimistic_transaction_db.h>

#include <grpcpp/grpcpp.h>
#include "kv.grpc.pb.h"

// proxy_backend_server.cpp
// ------------------------
// Designed to run BEHIND an NGINX proxy.
// No SSL, No JWT. Pure maximum speed RocksDB Unary operations.

using namespace rocksdb;

class CallDataBase {
public:
    virtual void Proceed() = 0;
    virtual ~CallDataBase() = default;
};

class ProxyBackendServer final {
public:
    ~ProxyBackendServer() { Shutdown(); }

    void Run() {
        Options options;
        options.create_if_missing = true;
        
        // NITRO+ PROFILE 
        int num_cores = std::thread::hardware_concurrency();
        options.IncreaseParallelism(num_cores);
        options.OptimizeLevelStyleCompaction();
        options.max_background_jobs = 2; 
        options.info_log_level = rocksdb::ERROR_LEVEL; 
        
        // 128MB total footprint
        options.write_buffer_size = 64 * 1024 * 1024; 
        options.max_write_buffer_number = 2;
        options.min_write_buffer_number_to_merge = 1;
        
        options.unordered_write = true; 
        options.allow_concurrent_memtable_write = true;

        Status s = OptimisticTransactionDB::Open(options, "./data/proxy_backend", &txn_db_);
        if (!s.ok()) { std::cerr << "RocksDB Error: " << s.ToString() << std::endl; exit(1); }

        w_opts_.disableWAL = true;
        w_opts_.sync = false;

        grpc::ResourceQuota quota;
        quota.SetMaxThreads(num_cores * 4);

        grpc::ServerBuilder builder;
        builder.SetResourceQuota(quota);
        builder.SetMaxReceiveMessageSize(128 * 1024 * 1024); 
        
        // Remove throttling
        builder.AddChannelArgument(GRPC_ARG_MAX_CONCURRENT_STREAMS, 100000);
        builder.AddChannelArgument(GRPC_ARG_HTTP2_MAX_PINGS_WITHOUT_DATA, 0);
        builder.AddChannelArgument(GRPC_ARG_KEEPALIVE_PERMIT_WITHOUT_CALLS, 1);

        // Listen INSECURELY on all available CPUs
        for (int p = 50051; p <= 50058; p++) {
            builder.AddListeningPort("0.0.0.0:" + std::to_string(p), grpc::InsecureServerCredentials());
        }
        
        builder.RegisterService(&service_);

        for (int i = 0; i < num_cores; i++) {
            cqs_.emplace_back(builder.AddCompletionQueue());
        }

        server_ = builder.BuildAndStart();
        if (!server_) {
            std::cerr << "\n[!] FATAL ERROR: Failed to bind to ports 50051-50058. Address already in use!" << std::endl;
            std::cerr << "[!] Please run 'pkill -9 proxy_backend_server kv_server' to clear zombie processes." << std::endl;
            exit(1);
        }
        std::cout << ">>> NGINX PROXY BACKEND ACTIVE: INSECURE + NO JWT <<<" << std::endl;
        std::cout << ">>> Listening on ports 50051-50058 <<<" << std::endl;

        int threads_per_core = 1; 
        for (auto& cq : cqs_) {
            for (int i = 0; i < threads_per_core; i++) {
                workers_.emplace_back([this, cq_ptr = cq.get()]() {
                    for(int j=0; j<200; ++j) { 
                        new SingleCallData(&service_, cq_ptr, txn_db_, &w_opts_);
                    }
                    void* tag;
                    bool ok;
                    while (cq_ptr->Next(&tag, &ok)) {
                        if (ok) {
                            static_cast<CallDataBase*>(tag)->Proceed();
                        } else {
                            delete static_cast<CallDataBase*>(tag);
                        }
                    }
                });
            }
        }
        
        server_->Wait();
    }

    void Shutdown() {
        if (!is_shutdown_.exchange(true)) {
            std::cout << "\nInitiating graceful shutdown..." << std::endl;
            if (server_) server_->Shutdown();
            for (auto& cq : cqs_) cq->Shutdown();
            for (auto& t : workers_) if (t.joinable()) t.join();
            if (txn_db_) delete txn_db_;
            std::cout << "Server safely terminated." << std::endl;
        }
    }

private:
    struct SingleCallData : public CallDataBase {
        kv::KVService::AsyncService* service;
        grpc::ServerCompletionQueue* cq;
        OptimisticTransactionDB* txn_db;
        WriteOptions* w_opts;
        grpc::ServerContext ctx;
        kv::SingleRequest req;
        kv::SingleResponse resp;
        grpc::ServerAsyncResponseWriter<kv::SingleResponse> responder;
        enum { CREATE, PROCESS, FINISH } status;

        SingleCallData(kv::KVService::AsyncService* s, grpc::ServerCompletionQueue* c, OptimisticTransactionDB* db, WriteOptions* wo)
            : service(s), cq(c), txn_db(db), w_opts(wo), responder(&ctx), status(CREATE) { Proceed(); }

        void Proceed() override {
            if (status == CREATE) {
                status = PROCESS;
                service->RequestExecuteSingle(&ctx, &req, &responder, cq, cq, this);
            } else if (status == PROCESS) {
                new SingleCallData(service, cq, txn_db, w_opts);
                
                // NO JWT CHECK! Just execute raw speed.
                
                if (req.type() == kv::GET) {
                    std::string val;
                    Status s = txn_db->Get(ReadOptions(), req.key(), &val);
                    resp.set_success(s.ok() || s.IsNotFound());
                    if (s.ok()) resp.set_value(val);
                } else {
                    Status s;
                    if (req.type() == kv::PUT) s = txn_db->GetBaseDB()->Put(*w_opts, req.key(), req.value());
                    else if (req.type() == kv::DELETE) s = txn_db->GetBaseDB()->Delete(*w_opts, req.key());
                    resp.set_success(s.ok());
                }
                status = FINISH;
                responder.Finish(resp, grpc::Status::OK, this);
            } else { delete this; }
        }
    };

    std::vector<std::unique_ptr<grpc::ServerCompletionQueue>> cqs_;
    kv::KVService::AsyncService service_;
    std::unique_ptr<grpc::Server> server_;
    OptimisticTransactionDB* txn_db_ = nullptr;
    WriteOptions w_opts_;
    std::vector<std::thread> workers_;
    std::atomic<bool> is_shutdown_{false};
};

ProxyBackendServer* g_server = nullptr;
void HandleSignal(int signal) { if (g_server) g_server->Shutdown(); }

int main() {
    std::signal(SIGINT, HandleSignal);
    std::signal(SIGTERM, HandleSignal);
    ProxyBackendServer s;
    g_server = &s;
    s.Run();
    return 0;
}
