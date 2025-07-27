/*
  Safekeeper Client for Serverless MariaDB Storage Engine
  Copyright (c) 2024 Serverless MariaDB Project

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.

  TCP client for communicating with the Rust safekeeper service
  to stream WAL records for durability and consensus.
*/

#ifndef SAFEKEEPER_CLIENT_H
#define SAFEKEEPER_CLIENT_H

// MariaDB types first
#include "my_global.h"
#include "mysql.h"

#include <stdint.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>

// Common type definitions
#include "serverless_types.h"

/**
 * WAL append request for async processing
 */
struct WalAppendRequest {
    TimelineId timeline_id;
    WalRecord record;
    bool completed;
    int result;
    
    WalAppendRequest(const TimelineId& tl_id, const WalRecord& wal_record)
        : timeline_id(tl_id), record(wal_record), completed(false), result(0) {}
};

/**
 * Safekeeper Client
 * 
 * Handles TCP communication with the Rust safekeeper service
 * to stream WAL records for durability. Implements async
 * WAL append queue and connection management for high performance.
 */
class SafekeeperClient {
private:
    // Connection details
    char* server_host;
    int server_port;
    int socket_fd;
    bool connected;
    
    // Async WAL append queue
    std::queue<WalAppendRequest*> append_queue;
    std::thread worker_thread;
    std::mutex queue_mutex;
    std::condition_variable queue_condition;
    bool shutdown_requested;
    
    // Connection management
    int establish_connection();
    void close_connection();
    int reconnect_if_needed();
    
    // Message serialization
    int serialize_append_request(const TimelineId& timeline_id, 
                                 const WalRecord& record, 
                                 char* buffer, size_t buffer_size);
    int deserialize_append_response(const char* buffer, size_t buffer_size,
                                    uint64_t* committed_lsn);
    
    // Async worker thread
    void worker_thread_main();
    int process_append_request(WalAppendRequest* request);
    
    // Low-level TCP operations
    int send_message(const char* data, size_t length);
    int receive_message(char* buffer, size_t buffer_size);
    
public:
    SafekeeperClient(const char* host, int port);
    ~SafekeeperClient();
    
    // Core WAL operations
    int append_wal_record(const TimelineId& timeline_id, const WalRecord& record);
    int append_wal_record_async(const TimelineId& timeline_id, const WalRecord& record);
    int read_wal_record(const TimelineId& timeline_id, uint64_t lsn, 
                        char* buffer, size_t buffer_size);
    
    // Timeline management
    int create_timeline(const TimelineId& timeline_id);
    int get_timeline_status(const TimelineId& timeline_id, uint64_t* latest_lsn);
    
    // Connection management
    int connect();
    void disconnect();
    bool is_connected() const { return connected; }
    
    // Status and health
    int get_server_status();
    int check_availability();
    
    // Configuration
    void set_server_address(const char* host, int port);
};

#endif /* SAFEKEEPER_CLIENT_H */
