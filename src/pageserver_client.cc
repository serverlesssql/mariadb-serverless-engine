/*
  Pageserver Client Implementation
  Copyright (c) 2024 Serverless MariaDB Project

  HTTP client for communicating with Rust pageserver service
*/

#include "pageserver_client.h"
#include "ha_serverless.h"
#include <cstdlib>
#include <cstring>
#include <cstdio>

// HTTP response callback for libcurl
size_t PageserverClient::write_callback(void* contents, size_t size, size_t nmemb, HttpResponse* response)
{
    size_t total_size = size * nmemb;
    
    response->data = (char*)realloc(response->data, response->size + total_size + 1);
    if (response->data) {
        memcpy(&(response->data[response->size]), contents, total_size);
        response->size += total_size;
        response->data[response->size] = 0;  // Null terminate
    }
    
    return total_size;
}

PageserverClient::PageserverClient(const char* pageserver_url, long timeout)
    : curl_handle(nullptr), base_url(nullptr), timeout_seconds(timeout)
{
    // Initialize libcurl
    curl_handle = curl_easy_init();
    
    // Set base URL
    set_base_url(pageserver_url);
    
    // Configure curl options
    if (curl_handle) {
        curl_easy_setopt(curl_handle, CURLOPT_TIMEOUT, timeout_seconds);
        curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, write_callback);
        curl_easy_setopt(curl_handle, CURLOPT_FOLLOWLOCATION, 1L);
    }
}

PageserverClient::~PageserverClient()
{
    if (curl_handle) {
        curl_easy_cleanup(curl_handle);
    }
    if (base_url) {
        free(base_url);
    }
}

void PageserverClient::set_base_url(const char* url)
{
    if (base_url) {
        free(base_url);
    }
    base_url = strdup(url);
}

int PageserverClient::make_http_request(const char* url, HttpResponse* response)
{
    if (!curl_handle) {
        return -1;
    }
    
    curl_easy_setopt(curl_handle, CURLOPT_URL, url);
    curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, response);
    
    CURLcode res = curl_easy_perform(curl_handle);
    if (res != CURLE_OK) {
        return -1;
    }
    
    long response_code;
    curl_easy_getinfo(curl_handle, CURLINFO_RESPONSE_CODE, &response_code);
    
    return (response_code == 200) ? 0 : -1;
}

char* PageserverClient::build_page_url(const PageId& page_id)
{
    char* url = (char*)malloc(256);
    snprintf(url, 256, "%s/page/%llu/%u", base_url, (unsigned long long)page_id.timeline_id, page_id.page_number);
    return url;
}

char* PageserverClient::build_timeline_url(const TimelineId& timeline_id)
{
    char* url = (char*)malloc(256);
    snprintf(url, 256, "%s/timeline/%llu", base_url, (unsigned long long)timeline_id.id);
    return url;
}

int PageserverClient::read_page(const PageId& page_id, char* buffer, size_t buffer_size)
{
    char* url = build_page_url(page_id);
    HttpResponse response;
    
    int result = make_http_request(url, &response);
    
    if (result == 0 && response.data && response.size <= buffer_size) {
        memcpy(buffer, response.data, response.size);
        // Pad with zeros if needed
        if (response.size < buffer_size) {
            memset(buffer + response.size, 0, buffer_size - response.size);
        }
    } else {
        result = -1;
    }
    
    free(url);
    return result;
}

int PageserverClient::get_timeline_info(const TimelineId& timeline_id, uint64_t* latest_lsn)
{
    char* url = build_timeline_url(timeline_id);
    HttpResponse response;
    
    int result = make_http_request(url, &response);
    
    if (result == 0 && response.data) {
        // Parse JSON response for LSN (simplified)
        *latest_lsn = 1;  // Placeholder
    } else {
        result = -1;
    }
    
    free(url);
    return result;
}

int PageserverClient::create_timeline(const TimelineId& timeline_id)
{
    char* url = build_timeline_url(timeline_id);
    HttpResponse response;
    
    // For now, just check if timeline exists
    int result = make_http_request(url, &response);
    
    free(url);
    return (result == 0) ? 0 : -1;  // Return actual result
}

int PageserverClient::delete_timeline(const TimelineId& timeline_id)
{
    // Placeholder implementation
    return 0;
}

int PageserverClient::check_availability()
{
    char* url = (char*)malloc(256);
    snprintf(url, 256, "%s/health", base_url);
    
    HttpResponse response;
    int result = make_http_request(url, &response);
    
    free(url);
    return result;
}

int PageserverClient::get_server_status()
{
    return check_availability();
}
