/*
  Pageserver Client for Serverless MariaDB Storage Engine
  Copyright (c) 2024 Serverless MariaDB Project

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.

  HTTP client for communicating with the Rust pageserver service
  to fetch data pages on demand for true serverless operation.
*/

#ifndef PAGESERVER_CLIENT_H
#define PAGESERVER_CLIENT_H

// MariaDB types first
#include "my_global.h"
#include "mysql.h"

#include <curl/curl.h>
#include <stdint.h>

// Common type definitions
#include "serverless_types.h"

/**
 * HTTP response structure for libcurl
 */
struct HttpResponse {
    char* data;
    size_t size;
    
    HttpResponse() : data(nullptr), size(0) {}
    ~HttpResponse() { if (data) free(data); }
};

/**
 * Pageserver Client
 * 
 * Handles HTTP communication with the Rust pageserver service
 * to fetch data pages on demand. Implements connection pooling
 * and error handling for production reliability.
 */
class PageserverClient {
private:
    CURL* curl_handle;
    char* base_url;
    long timeout_seconds;
    
    // HTTP response callback
    static size_t write_callback(void* contents, size_t size, size_t nmemb, HttpResponse* response);
    
    // Helper methods
    int make_http_request(const char* url, HttpResponse* response);
    char* build_page_url(const PageId& page_id);
    char* build_timeline_url(const TimelineId& timeline_id);
    
public:
    PageserverClient(const char* pageserver_url, long timeout = 30);
    ~PageserverClient();
    
    // Core page operations
    int read_page(const PageId& page_id, char* buffer, size_t buffer_size);
    int get_timeline_info(const TimelineId& timeline_id, uint64_t* latest_lsn);
    
    // Timeline management
    int create_timeline(const TimelineId& timeline_id);
    int delete_timeline(const TimelineId& timeline_id);
    
    // Health and status
    int check_availability();
    int get_server_status();
    
    // Configuration
    void set_timeout(long seconds) { timeout_seconds = seconds; }
    void set_base_url(const char* url);
};

#endif /* PAGESERVER_CLIENT_H */
