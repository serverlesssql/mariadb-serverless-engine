/*
  Serverless MariaDB Storage Engine
  Copyright (c) 2024 Serverless MariaDB Project

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1335 USA

  Neon-inspired serverless storage engine for MariaDB
  Implements true compute/storage separation by redirecting all I/O
  to remote pageserver and safekeeper services.
*/

#ifndef HA_SERVERLESS_H
#define HA_SERVERLESS_H

#ifdef USE_PRAGMA_INTERFACE
#pragma interface
#endif

// MariaDB core includes - order matters!
#include "my_global.h"
#include "mysql.h"
#include "handler.h"
#include "table.h"
#include "sql_class.h"

// C++ standard library includes
#include <unordered_map>
#include <list>
#include <ctime>
#include <cstdlib>

// Common type definitions
#include "serverless_types.h"

// Forward declarations for our clients
class PageserverClient;
class SafekeeperClient;

// Page cache entry
struct CachedPage {
    PageId page_id;
    char* data;             // 16KB page data
    uint64_t lsn;           // LSN when page was cached
    bool dirty;             // Whether page has been modified
    time_t last_access;     // For LRU eviction
    
    CachedPage(const PageId& id) 
        : page_id(id), data(nullptr), lsn(0), dirty(false), last_access(0) {}
    ~CachedPage() { if (data) free(data); }
};

/**
 * Serverless Storage Engine Handler
 * 
 * This class implements MariaDB's handler interface to provide
 * serverless storage capabilities. All page reads are served by
 * the pageserver, and all writes are streamed to the safekeeper
 * for durability and consensus.
 */
class ha_serverless: public handler
{
private:
    // Remote service clients
    PageserverClient* pageserver_client;
    SafekeeperClient* safekeeper_client;
    
    // Current table timeline
    TimelineId current_timeline;
    
    // Page cache (LRU with configurable size)
    static const size_t MAX_CACHED_PAGES = 1024;
    std::unordered_map<uint64_t, CachedPage*> page_cache;
    std::list<uint64_t> lru_list;
    mutable mysql_mutex_t cache_mutex;
    
    // WAL tracking
    uint64_t current_lsn;
    
    // Helper methods
    int read_page_from_cache_or_pageserver(const PageId& page_id, char* buffer);
    int write_page_to_safekeeper(const PageId& page_id, const char* data);
    void evict_page_from_cache(uint64_t page_key);
    uint64_t page_key(const PageId& page_id) const;
    
public:
    ha_serverless(handlerton *hton, TABLE_SHARE *table_arg);
    ~ha_serverless();
    
    // Storage engine identification
    const char *table_type() const { return "SERVERLESS"; }
    const char *index_type(uint index_number) { return "NONE"; }
    const char **bas_ext() const;
    
    // Table operations
    int open(const char *name, int mode, uint test_if_locked);
    int close(void);
    int create(const char *name, TABLE *table_arg,
               HA_CREATE_INFO *create_info);
    int delete_table(const char *name);
    int rename_table(const char* from, const char* to);
    
    // Row operations  
    int write_row(const uchar *buf);
    int update_row(const uchar *old_data, const uchar *new_data);
    int delete_row(const uchar *buf);
    
    // Scan operations
    int rnd_init(bool scan);
    int rnd_end();
    int rnd_next(uchar *buf);
    int rnd_pos(uchar *buf, uchar *pos);
    void position(const uchar *record);
    
    // Index operations (basic support for production)
    int index_init(uint idx, bool sorted);
    int index_end() { return 0; }
    int index_read_map(uchar *buf, const uchar *key,
                       key_part_map keypart_map,
                       enum ha_rkey_function find_flag);
    int index_next(uchar *buf);
    int index_prev(uchar *buf);
    int index_first(uchar *buf);
    int index_last(uchar *buf);
    
    // Information methods
    int info(uint flag);
    ha_rows records_in_range(uint inx, const key_range *min_key,
                             const key_range *max_key, page_range *pages);
    
    // Transaction support
    int external_lock(THD *thd, int lock_type);
    THR_LOCK_DATA **store_lock(THD *thd, THR_LOCK_DATA **to,
                               enum thr_lock_type lock_type);
    
    // Storage engine capabilities
    ulonglong table_flags() const
    {
        return (HA_REC_NOT_IN_SEQ | HA_CAN_GEOMETRY | HA_FAST_KEY_READ |
                HA_NULL_IN_KEY | HA_CAN_INDEX_BLOBS | HA_AUTO_PART_KEY |
                HA_FILE_BASED | HA_CAN_INSERT_DELAYED);
    }
    
    ulong index_flags(uint inx, uint part, bool all_parts) const
    {
        return HA_READ_NEXT | HA_READ_PREV | HA_READ_ORDER | HA_READ_RANGE | 
               HA_KEYREAD_ONLY | HA_DO_INDEX_COND_PUSHDOWN;
    }
    
    uint max_supported_record_length() const { return HA_MAX_REC_LENGTH; }
    uint max_supported_keys() const { return 64; }
    uint max_supported_key_parts() const { return 16; }
    uint max_supported_key_length() const { return 3072; }
    
    // Serverless-specific methods
    int initialize_timeline(const char* table_name);
    int ensure_pageserver_connection();
    int ensure_safekeeper_connection();
};

#endif /* HA_SERVERLESS_H */
