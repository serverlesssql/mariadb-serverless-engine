#ifndef CONNECTION_POOL_IMPL_H
#define CONNECTION_POOL_IMPL_H

#include "connection_pool.h"

// Template specializations for PooledConnection
template<>
inline void PooledConnection<PageserverClient>::return_connection_impl(PageserverClient* c, ConnectionPool* p) {
    p->return_pageserver_connection(c);
}

template<>
inline void PooledConnection<SafekeeperClient>::return_connection_impl(SafekeeperClient* c, ConnectionPool* p) {
    p->return_safekeeper_connection(c);
}

#endif // CONNECTION_POOL_IMPL_H
