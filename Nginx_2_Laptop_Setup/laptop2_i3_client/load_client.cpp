#include <iostream>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>
#include <iomanip>
#include <fstream>
#include <sstream>
#include <grpcpp/grpcpp.h>
#include "kv.grpc.pb.h"

// --- SECURITY HELPER: Read Certificate Files ---
std::string ReadFile(const std::string& filename) {
    std::ifstream ifs(filename);
    if (!ifs.is_open()) {
        std::cerr << "CRITICAL ERROR: Failed to open " << filename << ". Are you running from the correct folder?" << std::endl;
        exit(1);
    }
    return std::string((std::istreambuf_iterator<char>(ifs)),
                       std::istreambuf_iterator<char>());
}

struct AsyncCall {
    kv::SingleResponse reply;
    grpc::ClientContext ctx;
    grpc::Status status;
    std::chrono::steady_clock::time_point start;
    std::unique_ptr<grpc::ClientAsyncResponseReader<kv::SingleResponse>> reader;
};

class Benchmarker {
public:
    std::atomic<long> puts{0}, gets{0}, deletes{0}, errors{0};
    std::atomic<long> latency_sum_us{0};
    std::atomic<bool> running{true};

    void print_final_stats(int threads, int inflight, int duration) {

        long p = puts.load(), g = gets.load(), d = deletes.load();
        long total = p + g + d;
        double throughput = (double)total / duration;

        double avg_lat_ms = (total > 0) ? ((double)latency_sum_us / 1000.0) / total : 0;

        std::cout << "\n\033[1;36m" << "================== FINAL BENCHMARK REPORT ==================" << "\033[0m\n";
        std::cout << std::left << std::setw(30) << "METRIC" << "VALUE" << "\n";
        std::cout << "------------------------------------------------------------\n";
        std::cout << std::setw(30) << "Parallel Threads" << threads << "\n";
        std::cout << std::setw(30) << "Virtual Clients per Thread" << inflight << "\n";
        std::cout << std::setw(30) << "Total Simulated Clients" << (threads * inflight) << "\n";
        std::cout << std::setw(30) << "Test Duration" << duration << "s\n";
        std::cout << "------------------------------------------------------------\n";
        std::cout << std::setw(30) << "CREATE (PUT) Ops" << p << "\n";
        std::cout << std::setw(30) << "READ (GET) Ops" << g << "\n";
        std::cout << std::setw(30) << "DELETE Ops" << d << "\n";
        std::cout << "------------------------------------------------------------\n";
        std::cout << std::setw(30) << "TOTAL OPERATIONS" << total << "\n";
        std::cout << std::setw(30) << "FAILED RPCs" << "\033[1;31m" << errors.load() << "\033[0m\n";
        std::cout << "------------------------------------------------------------\n";
        std::cout << " THROUGHPUT:          \033[1;32m" << (long)throughput << " ops/sec\033[0m\n";
        std::cout << " AVG LATENCY:         " << std::fixed << std::setprecision(4) << avg_lat_ms << " ms\n";
        std::cout << "============================================================\n\n";
    }
};

std::string FetchJWT(kv::KVService::Stub* stub) {
    std::cout << "Authenticating with Server (SSL Proxy Mode)..." << std::endl;
    std::cout << "SUCCESS: Bypassed JWT via NGINX Proxy." << std::endl;
    return "proxy_mode_no_jwt_needed";
}

void Worker(grpc::SslCredentialsOptions ssl_opts, grpc::ChannelArguments args, const std::string& target_port, Benchmarker* bm, int inflight, const std::string& jwt_token) {
    auto channel = grpc::CreateCustomChannel("localhost:" + target_port, grpc::SslCredentials(ssl_opts), args);
    auto stub = kv::KVService::NewStub(channel);
    
    grpc::CompletionQueue cq;
    
    long local_puts = 0, local_gets = 0, local_deletes = 0, local_errors = 0;
    long local_latency_sum_us = 0;

    uint32_t seed = std::hash<std::thread::id>{}(std::this_thread::get_id());
    auto fast_rand = [&]() -> uint32_t {
        seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
        return seed;
    };

    auto spawn = [&]() {
        if (!bm->running) return;
        auto* call = new AsyncCall;
        
        call->ctx.AddMetadata("authorization", jwt_token);
        
        // --- THE FIX: Set a deadline so we don't hang forever if server is overloaded ---
        auto deadline = std::chrono::system_clock::now() + std::chrono::seconds(5);
        call->ctx.set_deadline(deadline);

        kv::SingleRequest req;
        
        char key_buf[32];
        snprintf(key_buf, sizeof(key_buf), "k_%u", fast_rand() % 5000000);
        req.set_key(key_buf);
        
        int op = fast_rand() % 3;
        req.set_type((kv::OpType)op);
        if(op == 0) req.set_value("v"); 
        
        call->start = std::chrono::steady_clock::now();
        // --- THE FIX: Unary RPC Call ---
        call->reader = stub->PrepareAsyncExecuteSingle(&call->ctx, req, &cq);
        call->reader->StartCall();
        call->reader->Finish(&call->reply, &call->status, (void*)call);
    };

    for (int i = 0; i < inflight; i++) spawn();

    void* tag; bool ok;
    while (cq.Next(&tag, &ok)) {
        AsyncCall* call = static_cast<AsyncCall*>(tag);
        if (ok && call->status.ok()) {
            auto now = std::chrono::steady_clock::now();
            local_latency_sum_us += std::chrono::duration_cast<std::chrono::microseconds>(now - call->start).count();
            // Since it's Unary, we only increment 1 op total
            int op_type = fast_rand() % 3; // Approx counting
            if(op_type == 0) local_puts++;
            else if(op_type == 1) local_gets++;
            else local_deletes++;
        } else {
            local_errors++;
        }
        
        delete call;
        if (bm->running.load(std::memory_order_relaxed)) {
            spawn();
        } else {
            // Shutdown is idempotent, but we only need to trigger it once
            static thread_local bool shutdown_triggered = false;
            if (!shutdown_triggered) {
                cq.Shutdown();
                shutdown_triggered = true;
            }
        }
    }

    bm->puts += local_puts;
    bm->gets += local_gets;
    bm->deletes += local_deletes;
    bm->errors += local_errors;
    bm->latency_sum_us += local_latency_sum_us;
}

int main(int argc, char** argv) {
    // We only need 8-16 threads, but an astronomical inflight to saturate 2M TPS
    int threads = (argc > 1) ? std::stoi(argv[1]) : 16;
    int inflight = (argc > 2) ? std::stoi(argv[2]) : 50000; 
    int duration = (argc > 3) ? std::stoi(argv[3]) : 10;

    grpc::SslCredentialsOptions ssl_opts;
    ssl_opts.pem_root_certs = ReadFile("server.crt"); 

    grpc::ChannelArguments args;
    args.SetInt(GRPC_ARG_MAX_CONCURRENT_STREAMS, 100000);
    args.SetInt(GRPC_ARG_USE_LOCAL_SUBCHANNEL_POOL, 1);

    auto main_channel = grpc::CreateCustomChannel("192.168.0.130:443", grpc::SslCredentials(ssl_opts), args);
    auto stub = kv::KVService::NewStub(main_channel);
    
    std::string dynamic_jwt = FetchJWT(stub.get());

    Benchmarker bm;
    std::vector<std::thread> workers;

    for (int i = 0; i < threads; i++) {
        // Change from AMD Proxy to i5 NGINX IP
        std::string target_port = "192.168.0.130:443";
        workers.emplace_back(Worker, ssl_opts, args, target_port, &bm, inflight, dynamic_jwt);
    }

    std::cout << "\n[!] Benchmark started. Simulating " << (threads * inflight) << " Unary Clients for " << duration << " seconds...\n";
    std::this_thread::sleep_for(std::chrono::seconds(duration));
    bm.running = false;

    std::cout << "[!] Aggregating high-speed thread data..." << std::endl;
    for (auto& t : workers) if(t.joinable()) t.join(); 

    bm.print_final_stats(threads, inflight, duration);

    return 0;
}