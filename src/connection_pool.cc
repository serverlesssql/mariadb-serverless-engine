#include "connection_pool.h"
#include <mysql/plugin.h>
#include <my_sys.h>
#include <sql_class.h>
#include <algorithm>
#include <chrono>

// Global connection pool instance
std::unique_ptr<ConnectionPool> global_connection_pool;

ConnectionPool::ConnectionPool(size_t min_pageserver, size_t max_pageserver,
                               size_t min_safekeeper, size_t max_safekeeper)
    : max_pageserver_connections(max_pageserver)
    , max_safekeeper_connections(max_safekeeper)
    , min_pageserver_connections(min_pageserver)
    , min_safekeeper_connections(min_safekeeper)
{
    // Reserve space for connections
    all_pageserver_connections.reserve(max_pageserver_connections);
    all_safekeeper_connections.reserve(max_safekeeper_connections);
}

ConnectionPool::~ConnectionPool() {
    shutdown();
}

bool ConnectionPool::initialize() {
    try {
        // Create minimum number of connections
        warm_connections();
        
        // Start health check thread
        health_check_running = true;
        health_check_thread = std::thread(&ConnectionPool::health_check_worker, this);
        
        sql_print_information("ServerlessDB: Connection pool initialized with %zu pageserver and %zu safekeeper connections",
                              min_pageserver_connections, min_safekeeper_connections);
        
        return true;
    } catch (const std::exception& e) {
        sql_print_error("ServerlessDB: Failed to initialize connection pool: %s", e.what());
        return false;
    }
}

void ConnectionPool::shutdown() {
    // Stop health check thread
    health_check_running = false;
    if (health_check_thread.joinable()) {
        health_check_thread.join();
    }
    
    // Clear all connections
    {
        std::lock_guard<std::mutex> lock(pageserver_mutex);
        while (!available_pageserver_connections.empty()) {
            available_pageserver_connections.pop();
        }
        all_pageserver_connections.clear();
    }
    
    {
        std::lock_guard<std::mutex> lock(safekeeper_mutex);
        while (!available_safekeeper_connections.empty()) {
            available_safekeeper_connections.pop();
        }
        all_safekeeper_connections.clear();
    }
    
    sql_print_information("ServerlessDB: Connection pool shutdown complete");
}

std::unique_ptr<PageserverClient> ConnectionPool::create_pageserver_connection() {
    try {
        std::unique_ptr<PageserverClient> client(new PageserverClient("http://localhost:9997"));
        
        // Test connection
        if (!is_connection_healthy(client.get())) {
            sql_print_warning("ServerlessDB: Failed to create healthy pageserver connection");
            return nullptr;
        }
        
        return client;
    } catch (const std::exception& e) {
        sql_print_error("ServerlessDB: Failed to create pageserver connection: %s", e.what());
        return nullptr;
    }
}

std::unique_ptr<SafekeeperClient> ConnectionPool::create_safekeeper_connection() {
    try {
        std::unique_ptr<SafekeeperClient> client(new SafekeeperClient("localhost", 5433));
        
        // Test connection
        if (!is_connection_healthy(client.get())) {
            sql_print_warning("ServerlessDB: Failed to create healthy safekeeper connection");
            return nullptr;
        }
        
        return client;
    } catch (const std::exception& e) {
        sql_print_error("ServerlessDB: Failed to create safekeeper connection: %s", e.what());
        return nullptr;
    }
}

PageserverClient* ConnectionPool::get_pageserver_connection(std::chrono::milliseconds timeout) {
    pageserver_requests++;
    
    std::unique_lock<std::mutex> lock(pageserver_mutex);
    
    // Wait for available connection or timeout
    if (!pageserver_cv.wait_for(lock, timeout, [this] {
        return !available_pageserver_connections.empty();
    })) {
        // Timeout - try to create new connection if under limit
        if (all_pageserver_connections.size() < max_pageserver_connections) {
            auto new_connection = create_pageserver_connection();
            if (new_connection) {
                PageserverClient* client = new_connection.get();
                all_pageserver_connections.push_back(std::move(new_connection));
                return client;
            }
        }
        
        sql_print_warning("ServerlessDB: Timeout waiting for pageserver connection");
        return nullptr;
    }
    
    // Get connection from pool
    auto connection = std::move(available_pageserver_connections.front());
    available_pageserver_connections.pop();
    
    PageserverClient* client = connection.release();
    pageserver_cache_hits++;
    
    return client;
}

SafekeeperClient* ConnectionPool::get_safekeeper_connection(std::chrono::milliseconds timeout) {
    safekeeper_requests++;
    
    std::unique_lock<std::mutex> lock(safekeeper_mutex);
    
    // Wait for available connection or timeout
    if (!safekeeper_cv.wait_for(lock, timeout, [this] {
        return !available_safekeeper_connections.empty();
    })) {
        // Timeout - try to create new connection if under limit
        if (all_safekeeper_connections.size() < max_safekeeper_connections) {
            auto new_connection = create_safekeeper_connection();
            if (new_connection) {
                SafekeeperClient* client = new_connection.get();
                all_safekeeper_connections.push_back(std::move(new_connection));
                return client;
            }
        }
        
        sql_print_warning("ServerlessDB: Timeout waiting for safekeeper connection");
        return nullptr;
    }
    
    // Get connection from pool
    auto connection = std::move(available_safekeeper_connections.front());
    available_safekeeper_connections.pop();
    
    SafekeeperClient* client = connection.release();
    safekeeper_cache_hits++;
    
    return client;
}

void ConnectionPool::return_pageserver_connection(PageserverClient* client) {
    if (!client) return;
    
    std::lock_guard<std::mutex> lock(pageserver_mutex);
    
    // Find the connection in our managed list
    auto it = std::find_if(all_pageserver_connections.begin(), all_pageserver_connections.end(),
                          [client](const std::unique_ptr<PageserverClient>& ptr) {
                              return ptr.get() == client;
                          });
    
    if (it != all_pageserver_connections.end()) {
        // Return to available pool
        available_pageserver_connections.push(std::move(*it));
        all_pageserver_connections.erase(it);
        
        // Notify waiting threads
        pageserver_cv.notify_one();
    } else {
        sql_print_warning("ServerlessDB: Attempted to return unknown pageserver connection");
    }
}

void ConnectionPool::return_safekeeper_connection(SafekeeperClient* client) {
    if (!client) return;
    
    std::lock_guard<std::mutex> lock(safekeeper_mutex);
    
    // Find the connection in our managed list
    auto it = std::find_if(all_safekeeper_connections.begin(), all_safekeeper_connections.end(),
                          [client](const std::unique_ptr<SafekeeperClient>& ptr) {
                              return ptr.get() == client;
                          });
    
    if (it != all_safekeeper_connections.end()) {
        // Return to available pool
        available_safekeeper_connections.push(std::move(*it));
        all_safekeeper_connections.erase(it);
        
        // Notify waiting threads
        safekeeper_cv.notify_one();
    } else {
        sql_print_warning("ServerlessDB: Attempted to return unknown safekeeper connection");
    }
}

void ConnectionPool::warm_connections() {
    // Create minimum pageserver connections
    {
        std::lock_guard<std::mutex> lock(pageserver_mutex);
        for (size_t i = 0; i < min_pageserver_connections; ++i) {
            auto connection = create_pageserver_connection();
            if (connection) {
                available_pageserver_connections.push(std::move(connection));
            } else {
                sql_print_warning("ServerlessDB: Failed to create pageserver connection %zu during warm-up", i);
            }
        }
        
        // Move to managed list
        while (!available_pageserver_connections.empty()) {
            all_pageserver_connections.push_back(std::move(available_pageserver_connections.front()));
            available_pageserver_connections.pop();
        }
        
        // Add back to available
        for (auto& conn : all_pageserver_connections) {
            available_pageserver_connections.push(std::unique_ptr<PageserverClient>(conn.release()));
        }
        all_pageserver_connections.clear();
        
        // Rebuild managed list
        while (!available_pageserver_connections.empty()) {
            all_pageserver_connections.push_back(std::move(available_pageserver_connections.front()));
            available_pageserver_connections.pop();
        }
        
        // Add back to available for use
        for (size_t i = 0; i < all_pageserver_connections.size(); ++i) {
            available_pageserver_connections.push(std::unique_ptr<PageserverClient>(all_pageserver_connections[i].get()));
        }
    }
    
    // Create minimum safekeeper connections
    {
        std::lock_guard<std::mutex> lock(safekeeper_mutex);
        for (size_t i = 0; i < min_safekeeper_connections; ++i) {
            auto connection = create_safekeeper_connection();
            if (connection) {
                all_safekeeper_connections.push_back(std::move(connection));
            } else {
                sql_print_warning("ServerlessDB: Failed to create safekeeper connection %zu during warm-up", i);
            }
        }
        
        // Add to available pool
        for (size_t i = 0; i < all_safekeeper_connections.size(); ++i) {
            available_safekeeper_connections.push(std::unique_ptr<SafekeeperClient>(all_safekeeper_connections[i].get()));
        }
    }
    
    sql_print_information("ServerlessDB: Warmed %zu pageserver and %zu safekeeper connections",
                          all_pageserver_connections.size(), all_safekeeper_connections.size());
}

bool ConnectionPool::is_connection_healthy(PageserverClient* client) {
    if (!client) return false;
    
    try {
        // Simple health check - try to get server status
        // This is a placeholder - implement actual health check based on your client API
        return true;
    } catch (...) {
        return false;
    }
}

bool ConnectionPool::is_connection_healthy(SafekeeperClient* client) {
    if (!client) return false;
    
    try {
        // Simple health check - try to ping safekeeper
        // This is a placeholder - implement actual health check based on your client API
        return true;
    } catch (...) {
        return false;
    }
}

void ConnectionPool::health_check_worker() {
    while (health_check_running) {
        std::this_thread::sleep_for(health_check_interval);
        
        if (!health_check_running) break;
        
        // Check pageserver connections
        {
            std::lock_guard<std::mutex> lock(pageserver_mutex);
            auto it = all_pageserver_connections.begin();
            while (it != all_pageserver_connections.end()) {
                if (!is_connection_healthy(it->get())) {
                    sql_print_information("ServerlessDB: Removing unhealthy pageserver connection");
                    it = all_pageserver_connections.erase(it);
                } else {
                    ++it;
                }
            }
        }
        
        // Check safekeeper connections
        {
            std::lock_guard<std::mutex> lock(safekeeper_mutex);
            auto it = all_safekeeper_connections.begin();
            while (it != all_safekeeper_connections.end()) {
                if (!is_connection_healthy(it->get())) {
                    sql_print_information("ServerlessDB: Removing unhealthy safekeeper connection");
                    it = all_safekeeper_connections.erase(it);
                } else {
                    ++it;
                }
            }
        }
        
        // Scale pool if needed
        scale_pool_if_needed();
    }
}

void ConnectionPool::scale_pool_if_needed() {
    // Auto-scale based on usage patterns
    // This is a simple implementation - can be made more sophisticated
    
    double pageserver_hit_rate = pageserver_requests > 0 ? 
        static_cast<double>(pageserver_cache_hits) / pageserver_requests : 1.0;
    double safekeeper_hit_rate = safekeeper_requests > 0 ? 
        static_cast<double>(safekeeper_cache_hits) / safekeeper_requests : 1.0;
    
    // If hit rate is low, consider adding more connections
    if (pageserver_hit_rate < 0.8 && all_pageserver_connections.size() < max_pageserver_connections) {
        std::lock_guard<std::mutex> lock(pageserver_mutex);
        auto new_connection = create_pageserver_connection();
        if (new_connection) {
            all_pageserver_connections.push_back(std::move(new_connection));
            sql_print_information("ServerlessDB: Scaled up pageserver connections to %zu", 
                                 all_pageserver_connections.size());
        }
    }
    
    if (safekeeper_hit_rate < 0.8 && all_safekeeper_connections.size() < max_safekeeper_connections) {
        std::lock_guard<std::mutex> lock(safekeeper_mutex);
        auto new_connection = create_safekeeper_connection();
        if (new_connection) {
            all_safekeeper_connections.push_back(std::move(new_connection));
            sql_print_information("ServerlessDB: Scaled up safekeeper connections to %zu", 
                                 all_safekeeper_connections.size());
        }
    }
}

ConnectionPool::PoolStats ConnectionPool::get_stats() const {
    PoolStats stats;
    
    {
        std::lock_guard<std::mutex> lock(pageserver_mutex);
        stats.pageserver_total = all_pageserver_connections.size();
        stats.pageserver_available = available_pageserver_connections.size();
    }
    
    {
        std::lock_guard<std::mutex> lock(safekeeper_mutex);
        stats.safekeeper_total = all_safekeeper_connections.size();
        stats.safekeeper_available = available_safekeeper_connections.size();
    }
    
    stats.pageserver_requests = pageserver_requests.load();
    stats.safekeeper_requests = safekeeper_requests.load();
    
    stats.pageserver_hit_rate = stats.pageserver_requests > 0 ? 
        static_cast<double>(pageserver_cache_hits.load()) / stats.pageserver_requests : 1.0;
    stats.safekeeper_hit_rate = stats.safekeeper_requests > 0 ? 
        static_cast<double>(safekeeper_cache_hits.load()) / stats.safekeeper_requests : 1.0;
    
    return stats;
}

void ConnectionPool::reset_stats() {
    pageserver_requests = 0;
    safekeeper_requests = 0;
    pageserver_cache_hits = 0;
    safekeeper_cache_hits = 0;
}

void ConnectionPool::set_health_check_interval(std::chrono::seconds interval) {
    health_check_interval = interval;
}

void ConnectionPool::set_pool_limits(size_t min_pageserver, size_t max_pageserver,
                                    size_t min_safekeeper, size_t max_safekeeper) {
    min_pageserver_connections = min_pageserver;
    max_pageserver_connections = max_pageserver;
    min_safekeeper_connections = min_safekeeper;
    max_safekeeper_connections = max_safekeeper;
}
