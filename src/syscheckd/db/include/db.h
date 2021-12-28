/**
 * @file db.h
 * @brief Definition of FIM database library.
 * @date 2019-08-28
 *
 * @copyright Copyright (C) 2015-2021 Wazuh, Inc.
 */

#ifndef FIMDB_H
#define FIMDB_H
#include "fimCommonDefs.h"

#ifdef __cplusplus
extern "C" {
#endif

#include "syscheck.h"
#include <openssl/evp.h>

#define FIM_DB_MEMORY_PATH  ":memory:"
#define FIM_DB_DISK_PATH    "queue/fim/db/fim.db"

#define EVP_MAX_MD_SIZE 64

/**
 * @brief Initialize the FIM database.
 *
 * It will be dbsync the responsible of managing the DB.
 * @param storage storage 1 Store database in memory, disk otherwise.
 * @param sync_interval Interval when the synchronization will be performed.
 * @param sync_callback Callback to send the synchronization messages.
 * @param log_callback Callback to perform logging operations.
 * @param file_limit Maximum number of files to be monitored
 * @param value_limit Maximum number of registry values to be monitored.
 * @param is_windows True when the OS is Windows.
 */
void fim_db_init(int storage,
                 int sync_interval,
                 fim_sync_callback_t sync_callback,
                 logging_callback_t log_callback,
                 int file_limit,
                 int value_limit,
                 bool is_windows);

/**
 * @brief Get entry data using path.
 *
 * @param file_path File path can be a pattern or a primary key
 * @param data Pointer to the data structure where the callback context will be stored.
 *
 * @return FIMDB_OK on success, FIMDB_ERROR on failure.
 */
int fim_db_get_path(const char* file_path, callback_context_t data);

/**
 * @brief Find entries based on pattern search.
 *
 * @param pattern Pattern to be searched.
 * @param data Pointer to the data structure where the callback context will be stored.
 *
 * @return FIMDB_OK on success, FIMDB_ERROR on failure.
 */
int fim_db_file_pattern_search(const char* pattern, callback_context_t data);

/**
 * @brief Delete entry from the DB using file path.
 *
 * @param path Path of the entry to be removed.
 *
 * @return FIMDB_OK on success, FIMDB_ERR otherwise.
 */
int fim_db_remove_path(const char* path);

/**
 * @brief Get count of all inodes in file_entry table.
 *
 * @return Number of inodes in file_entry table.
 */
int fim_db_get_count_file_inode();

/**
 * @brief Get count of all entries in file_entry table.
 *
 * @return Number of entries in file_entry table.
 */
int fim_db_get_count_file_entry();

/**
 * @brief Makes any necessary queries to get the entry updated in the DB.
 *
 * @param path The path to the file being processed.
 * @param data The information linked to the path to be created or updated
 * @param updated The updated is a flag to keep if the operation was updated or not.
 * @return The result of the update operation.
 * @retval Returns any of the values returned by fim_db_set_scanned and fim_db_insert_entry.
 */
int fim_db_file_update(const fim_entry* data, bool* updated);

/**
 * @brief Find entries using the inode.
 *
 * @param inode Inode.
 * @param dev Device.
 * @param data Pointer to the data structure where the callback context will be stored.
 */
void fim_db_file_inode_search(unsigned long int inode, unsigned long int dev, callback_context_t data);

/**
 * @brief Push a message to the syscheck queue
 *
 * @param msg The specific message to be pushed
 */
void fim_sync_push_msg(const char* msg);

/**
 * @brief Thread that performs the syscheck data synchronization
 *
 */
void fim_run_integrity();

#ifdef __cplusplus
}
#endif // _cplusplus
#endif // FIMDB_H
