/*
  Safekeeper Client Implementation
  Copyright (c) 2024 Serverless MariaDB Project

  TCP client for communicating with Rust safekeeper service
*/

#include "safekeeper_client.h"
#include "ha_serverless.h"
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>

SafekeeperClient::SafekeeperClient(const char* host, int port)
    : server_host(nullptr), server_port(port), socket_fd(-1), 
      connected(false), shutdown_requested(false)
{
    set_server_address(host, port);
    
    // Start worker thread for async WAL append
    worker_thread = std::thread(&SafekeeperClient::worker_thread_main, this);
}

SafekeeperClient::~SafekeeperClient()
{
    // Shutdown worker thread
    {
        std::lock_guard<std::mutex> lock(queue_mutex);
        shutdown_requested = true;
    }
    queue_condition.notify_all();
    
    if (worker_thread.joinable()) {
        worker_thread.join();
    }
    
    // Clean up pending requests
    while (!append_queue.empty()) {
        delete append_queue.front();
        append_queue.pop();
    }
    
    close_connection();
    
    if (server_host) {
        free(server_host);
    }
}

void SafekeeperClient::set_server_address(const char* host, int port)
{
    if (server_host) {
        free(server_host);
    }
    server_host = strdup(host);
    server_port = port;
}

int SafekeeperClient::establish_connection()
{
    if (connected) {
        return 0;
    }
    
    socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0) {
        return -1;
    }
    
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(server_port);
    
    if (inet_pton(AF_INET, server_host, &server_addr.sin_addr) <= 0) {
        close(socket_fd);
        socket_fd = -1;
        return -1;
    }
    
    if (::connect(socket_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        close(socket_fd);
        socket_fd = -1;
        return -1;
    }
    
    connected = true;
    return 0;
}

void SafekeeperClient::close_connection()
{
    if (socket_fd >= 0) {
        close(socket_fd);
        socket_fd = -1;
    }
    connected = false;
}

int SafekeeperClient::reconnect_if_needed()
{
    if (!connected) {
        return establish_connection();
    }
    return 0;
}

int SafekeeperClient::connect()
{
    return establish_connection();
}

void SafekeeperClient::disconnect()
{
    close_connection();
}

int SafekeeperClient::send_message(const char* data, size_t length)
{
    if (!connected || socket_fd < 0) {
        return -1;
    }
    
    ssize_t bytes_sent = send(socket_fd, data, length, 0);
    return (bytes_sent == (ssize_t)length) ? 0 : -1;
}

int SafekeeperClient::receive_message(char* buffer, size_t buffer_size)
{
    if (!connected || socket_fd < 0) {
        return -1;
    }
    
    ssize_t bytes_received = recv(socket_fd, buffer, buffer_size - 1, 0);
    if (bytes_received > 0) {
        buffer[bytes_received] = '\0';
        return bytes_received;
    }
    
    return -1;
}

int SafekeeperClient::serialize_append_request(const TimelineId& timeline_id, 
                                              const WalRecord& record, 
                                              char* buffer, size_t buffer_size)
{
    // Simple JSON serialization for now
    int written = snprintf(buffer, buffer_size,
        "{\"type\":\"append\",\"timeline_id\":%llu,\"lsn\":%llu,\"length\":%u,\"data\":\"%.*s\"}",
        (unsigned long long)timeline_id.id, (unsigned long long)record.lsn, record.length, (int)record.length, record.data);
    
    return (written < (int)buffer_size) ? written : -1;
}

int SafekeeperClient::deserialize_append_response(const char* buffer, size_t buffer_size,
                                                 uint64_t* committed_lsn)
{
    // Simple parsing for now
    *committed_lsn = 1;  // Placeholder
    return 0;
}

int SafekeeperClient::append_wal_record(const TimelineId& timeline_id, const WalRecord& record)
{
    if (reconnect_if_needed() != 0) {
        return -1;
    }
    
    char request_buffer[4096];
    int request_length = serialize_append_request(timeline_id, record, 
                                                 request_buffer, sizeof(request_buffer));
    if (request_length < 0) {
        return -1;
    }
    
    if (send_message(request_buffer, request_length) != 0) {
        return -1;
    }
    
    char response_buffer[1024];
    if (receive_message(response_buffer, sizeof(response_buffer)) < 0) {
        return -1;
    }
    
    uint64_t committed_lsn;
    return deserialize_append_response(response_buffer, strlen(response_buffer), &committed_lsn);
}

int SafekeeperClient::append_wal_record_async(const TimelineId& timeline_id, const WalRecord& record)
{
    WalAppendRequest* request = new WalAppendRequest(timeline_id, record);
    
    {
        std::lock_guard<std::mutex> lock(queue_mutex);
        append_queue.push(request);
    }
    queue_condition.notify_one();
    
    return 0;  // Async, so return success immediately
}

void SafekeeperClient::worker_thread_main()
{
    while (true) {
        WalAppendRequest* request = nullptr;
        
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            queue_condition.wait(lock, [this] { 
                return !append_queue.empty() || shutdown_requested; 
            });
            
            if (shutdown_requested) {
                break;
            }
            
            if (!append_queue.empty()) {
                request = append_queue.front();
                append_queue.pop();
            }
        }
        
        if (request) {
            request->result = process_append_request(request);
            request->completed = true;
            // In a full implementation, we'd notify waiters here
            delete request;
        }
    }
}

int SafekeeperClient::process_append_request(WalAppendRequest* request)
{
    return append_wal_record(request->timeline_id, request->record);
}

int SafekeeperClient::read_wal_record(const TimelineId& timeline_id, uint64_t lsn, 
                                     char* buffer, size_t buffer_size)
{
    // Placeholder implementation
    return -1;  // Not implemented yet
}

int SafekeeperClient::create_timeline(const TimelineId& timeline_id)
{
    if (reconnect_if_needed() != 0) {
        return -1;
    }
    
    char request_buffer[256];
    snprintf(request_buffer, sizeof(request_buffer),
             "{\"type\":\"create_timeline\",\"timeline_id\":%llu}", (unsigned long long)timeline_id.id);
    
    if (send_message(request_buffer, strlen(request_buffer)) != 0) {
        return -1;
    }
    
    char response_buffer[1024];
    if (receive_message(response_buffer, sizeof(response_buffer)) < 0) {
        return -1;
    }
    
    return 0;  // Success for now
}

int SafekeeperClient::get_timeline_status(const TimelineId& timeline_id, uint64_t* latest_lsn)
{
    *latest_lsn = 1;  // Placeholder
    return 0;
}

int SafekeeperClient::get_server_status()
{
    return connected ? 0 : -1;
}

int SafekeeperClient::check_availability()
{
    return reconnect_if_needed();
}
