/*
  Common Types for Serverless MariaDB Storage Engine
  Copyright (c) 2024 Serverless MariaDB Project
*/

#ifndef SERVERLESS_TYPES_H
#define SERVERLESS_TYPES_H

#include <stdint.h>

// Page and timeline identifiers
struct PageId {
    uint64_t timeline_id;
    uint32_t page_number;
    
    PageId(uint64_t tl_id, uint32_t page_num) 
        : timeline_id(tl_id), page_number(page_num) {}
};

struct TimelineId {
    uint64_t id;
    
    TimelineId(uint64_t timeline_id) : id(timeline_id) {}
};

// WAL record for safekeeper
struct WalRecord {
    uint64_t lsn;           // Log Sequence Number
    uint32_t length;        // Record length
    const char* data;       // Record data
    
    WalRecord(uint64_t log_lsn, uint32_t len, const char* record_data)
        : lsn(log_lsn), length(len), data(record_data) {}
};

#endif /* SERVERLESS_TYPES_H */
