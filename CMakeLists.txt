# Copyright (c) 2024 Serverless MariaDB Project
# 
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; version 2 of the License.
# 
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
# 
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1335 USA

# Serverless MariaDB Storage Engine - Neon-inspired architecture
# Redirects all I/O to remote pageserver and safekeeper services

SET(SERVERLESS_SOURCES 
    ha_serverless.cc
    pageserver_client.cc
    safekeeper_client.cc
    connection_pool.cc
)

# Add libcurl for HTTP client communication with pageserver
FIND_PACKAGE(CURL REQUIRED)
IF(CURL_FOUND)
    INCLUDE_DIRECTORIES(${CURL_INCLUDE_DIRS})
    SET(SERVERLESS_LIBS ${CURL_LIBRARIES})
ENDIF()

# JSON parsing is handled by Rust services, not needed in storage engine

MYSQL_ADD_PLUGIN(serverless ${SERVERLESS_SOURCES} 
    STORAGE_ENGINE 
    MODULE_ONLY
    LINK_LIBRARIES ${SERVERLESS_LIBS}
    COMPONENT Server)
