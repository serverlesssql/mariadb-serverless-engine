#ifndef CONNECTION_POOL_H
#define CONNECTION_POOL_H

#include <vector>
#include <queue>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <chrono>
#include <thread>

#include "pageserver_client.h"
#include "safekeeper_client.h"

/**
 * High-performance connection pool for serverless MariaDB storage engine
 * Eliminates cold start overhead by pre-warming connections
 */
class ConnectionPool {
private:
    // Pageserver connection pool
    std::queue<std::unique_ptr<PageserverClient>> available_pageserver_connections;
    std::vector<std::unique_ptr<PageserverClient>> all_pageserver_connections;
    
    // Safekeeper connection pool
    std::queue<std::unique_ptr<SafekeeperClient>> available_safekeeper_connections;
    std::vector<std::unique_ptr<SafekeeperClient>> all_safekeeper_connections;
    
    // Thread safety
    mutable std::mutex pageserver_mutex;
    mutable std::mutex safekeeper_mutex;
    std::condition_variable pageserver_cv;
    std::condition_variable safekeeper_cv;
    
    // Pool configuration
    size_t max_pageserver_connections;
    size_t max_safekeeper_connections;
    size_t min_pageserver_connections;
    size_t min_safekeeper_connections;
    
    // Connection health monitoring
    std::atomic<bool> health_check_running{false};
    std::thread health_check_thread;
    std::chrono::seconds health_check_interval{30};
    
    // Statistics
    std::atomic<uint64_t> pageserver_requests{0};
    std::atomic<uint64_t> safekeeper_requests{0};
    std::atomic<uint64_t> pageserver_cache_hits{0};
    std::atomic<uint64_t> safekeeper_cache_hits{0};
    
    // Connection creation
    std::unique_ptr<PageserverClient> create_pageserver_connection();
    std::unique_ptr<SafekeeperClient> create_safekeeper_connection();
    
    // Health monitoring
    void health_check_worker();
    bool is_connection_healthy(PageserverClient* client);
    bool is_connection_healthy(SafekeeperClient* client);
    
public:
    ConnectionPool(size_t min_pageserver = 5, size_t max_pageserver = 20,
                   size_t min_safekeeper = 3, size_t max_safekeeper = 10);
    ~ConnectionPool();
    
    // Initialization
    bool initialize();
    void shutdown();
    
    // Connection management
    PageserverClient* get_pageserver_connection(std::chrono::milliseconds timeout = std::chrono::milliseconds(1000));
    SafekeeperClient* get_safekeeper_connection(std::chrono::milliseconds timeout = std::chrono::milliseconds(1000));
    
    void return_pageserver_connection(PageserverClient* client);
    void return_safekeeper_connection(SafekeeperClient* client);
    
    // Pool management
    void warm_connections();
    void scale_pool_if_needed();
    
    // Statistics and monitoring
    struct PoolStats {
        size_t pageserver_total;
        size_t pageserver_available;
        size_t safekeeper_total;
        size_t safekeeper_available;
        uint64_t pageserver_requests;
        uint64_t safekeeper_requests;
        double pageserver_hit_rate;
        double safekeeper_hit_rate;
    };
    
    PoolStats get_stats() const;
    void reset_stats();
    
    // Configuration
    void set_health_check_interval(std::chrono::seconds interval);
    void set_pool_limits(size_t min_pageserver, size_t max_pageserver,
                        size_t min_safekeeper, size_t max_safekeeper);
};

// Simple RAII connection wrappers for automatic return to pool
class PooledPageserverConnection {
private:
    PageserverClient* client;
    ConnectionPool* pool;
    bool returned;
    
public:
    PooledPageserverConnection(PageserverClient* c, ConnectionPool* p) : client(c), pool(p), returned(false) {}
    
    ~PooledPageserverConnection() {
        if (!returned && client && pool) {
            pool->return_pageserver_connection(client);
        }
    }
    
    // Move constructor
    PooledPageserverConnection(PooledPageserverConnection&& other) noexcept 
        : client(other.client), pool(other.pool), returned(other.returned) {
        other.client = nullptr;
        other.returned = true;
    }
    
    // Delete copy constructor
    PooledPageserverConnection(const PooledPageserverConnection&) = delete;
    PooledPageserverConnection& operator=(const PooledPageserverConnection&) = delete;
    
    PageserverClient* get() { return client; }
    PageserverClient* operator->() { return client; }
    
    void return_early() {
        if (!returned && client && pool) {
            pool->return_pageserver_connection(client);
            returned = true;
        }
    }
};

class PooledSafekeeperConnection {
private:
    SafekeeperClient* client;
    ConnectionPool* pool;
    bool returned;
    
public:
    PooledSafekeeperConnection(SafekeeperClient* c, ConnectionPool* p) : client(c), pool(p), returned(false) {}
    
    ~PooledSafekeeperConnection() {
        if (!returned && client && pool) {
            pool->return_safekeeper_connection(client);
        }
    }
    
    // Move constructor
    PooledSafekeeperConnection(PooledSafekeeperConnection&& other) noexcept 
        : client(other.client), pool(other.pool), returned(other.returned) {
        other.client = nullptr;
        other.returned = true;
    }
    
    // Delete copy constructor
    PooledSafekeeperConnection(const PooledSafekeeperConnection&) = delete;
    PooledSafekeeperConnection& operator=(const PooledSafekeeperConnection&) = delete;
    
    SafekeeperClient* get() { return client; }
    SafekeeperClient* operator->() { return client; }
    
    void return_early() {
        if (!returned && client && pool) {
            pool->return_safekeeper_connection(client);
            returned = true;
        }
    }
};

// Global connection pool instance
extern std::unique_ptr<ConnectionPool> global_connection_pool;

#endif // CONNECTION_POOL_H
