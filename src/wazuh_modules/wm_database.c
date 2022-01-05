/*
 * Wazuh Module for SQLite database syncing
 * Copyright (C) 2015-2021, Wazuh Inc.
 * November 29, 2016
 *
 * This program is free software; you can redistribute it
 * and/or modify it under the terms of the GNU General Public
 * License (version 2) as published by the FSF - Free Software
 * Foundation.
 */

#include "wmodules.h"
#include "sec.h"
#include "remoted_op.h"
#include "wazuh_db/helpers/wdb_global_helpers.h"
#include "addagent/manage_agents.h" // FILE_SIZE
#include "external/cJSON/cJSON.h"

#ifndef CLIENT

#ifdef INOTIFY_ENABLED
#include <sys/inotify.h>

#define IN_BUFFER_SIZE sizeof(struct inotify_event) + NAME_MAX + 1

static volatile unsigned int queue_i;
static volatile unsigned int queue_j;
static w_queue_t * queue;                 // Queue for pending files
static OSHash * ptable;                 // Table for pending paths
static pthread_mutex_t mutex_queue = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond_pending = PTHREAD_COND_INITIALIZER;

int inotify_fd;

#ifndef LOCAL
int wd_agents = -2;
int wd_groups = -2;
int wd_shared_groups = -2;
#endif // !LOCAL

/* Get current inotify queued events limit */
static int get_max_queued_events();

/* Set current inotify queued events limit */
static int set_max_queued_events(int size);

// Setup inotify reader
static void wm_inotify_setup(wm_database * data);

// Real time inotify reader thread
static void * wm_inotify_start(void * args);

// Insert request into internal structure
void wm_inotify_push(const char * dirname, const char * fname);

// Extract enqueued path from internal structure
char * wm_inotify_pop();

#endif // INOTIFY_ENABLED

wm_database *module;
int is_worker;
int wdb_wmdb_sock = -1;

// Module main function. It won't return
static void* wm_database_main(wm_database *data);
// Destroy data
static void* wm_database_destroy(wm_database *data);
// Read config
cJSON *wm_database_dump(const wm_database *data);
// Update manager information
static void wm_sync_manager();

#ifndef LOCAL

static void wm_check_agents();

/**
 * @brief Method to synchronize 'client.keys' and 'global.db'. All new agents found in 'client.keys will be added to the DB
 *        and any agent in the DB that doesn't have a key will be removed. This method runs in workers constantly, but in the master
 *        it will run only at beggining.
 *
 * @param master_first_time Parameter to run the syncronization even if it's a master node.
 */
static void wm_sync_agents(bool master_first_time);

// Clean dangling database files
static void wm_clean_dangling_legacy_dbs();
static void wm_clean_dangling_wdb_dbs();

// Clean dangling group files
void wm_clean_dangling_groups();

static void wm_sync_multi_groups(const char *dirname);

/**
 * @brief This method will read the legacy GROUPS_DIR folder to insert in the global.db the groups information it founds.
 *        After every successful insertion, the legacy file is deleted. If we are in a worker, the files are deleted without inserting.
 *        If the folder is empty, it will be removed.
 */
void wm_sync_legacy_groups_files();

/**
 * @brief Method to insert a single group file in the global.db. Like this insertion is performed for legacy group files only in the master,
 *        the group insertion overrides any existent group assignment.
 *
 * @param group_file The name of the group file.
 * @param group_file_path The full path of the group file.
 * @return int OS_SUCCESS if successful, OS_INVALID otherwise.
 */
int wm_sync_group_file (const char* group_file, const char* group_file_path);

#endif // LOCAL

static int wm_sync_shared_group(const char *fname);
static int wm_sync_file(const char *dirname, const char *path);

// Database module context definition
const wm_context WM_DATABASE_CONTEXT = {
    "database",
    (wm_routine)wm_database_main,
    (wm_routine)wm_database_destroy,
    (cJSON * (*)(const void *))wm_database_dump,
    NULL,
    NULL
};

// Module main function. It won't return
void* wm_database_main(wm_database *data) {
    module = data;

    mtinfo(WM_DATABASE_LOGTAG, "Module started.");

    // Reset template. Basically, remove queue/db/.template.db
    char path_template[PATH_MAX + 1];
    snprintf(path_template, sizeof(path_template), "%s/%s", WDB_DIR, WDB_PROF_NAME);
    unlink(path_template);
    mdebug1("Template db file removed: %s", path_template);

    // Check if it is a worker node
    is_worker = w_is_worker();

    // Manager name synchronization
    if (data->sync_agents) {
        wm_sync_manager();
    }

    // Synchronize client.keys at startup to insert agent groups files
    wm_sync_agents(true);
    // If we have groups assignment in legacy files, insert them (master) or remove them (worker)
    wm_sync_legacy_groups_files();

#ifdef INOTIFY_ENABLED
    if (data->real_time) {
        char * path;
        char * file;

        wm_inotify_setup(data);

#ifndef LOCAL
        wm_clean_dangling_groups();
#endif
        while (1) {
            path = wm_inotify_pop();

#ifndef LOCAL
            if (!strcmp(path, KEYS_FILE)) {
                wm_sync_agents(false);
            } else
#endif // !LOCAL
            {
                if (file = strrchr(path, '/'), file) {
                    *(file++) = '\0';
                    wm_sync_file(path, file);
                } else {
                    mterror(WM_DATABASE_LOGTAG, "Couldn't extract file name from '%s'", path);
                }
            }

            free(path);
        }
    } else {
#endif // INOTIFY_ENABLED

        // Systems that don't support inotify, or real-time disabled

        long long tsleep;
        long long tstart;
        clock_t cstart;
        struct timespec spec0;
        struct timespec spec1;

        while (1) {
            tstart = (long long) time(NULL);
            cstart = clock();
            gettime(&spec0);

#ifndef LOCAL
            if (data->sync_agents) {
                wm_check_agents();
                wm_sync_multi_groups(SHAREDCFG_DIR);
                wm_clean_dangling_groups();
                wm_clean_dangling_legacy_dbs();
                wm_clean_dangling_wdb_dbs();
            }
#endif
            gettime(&spec1);
            time_sub(&spec1, &spec0);
            mtdebug1(WM_DATABASE_LOGTAG, "Cycle completed: %.3lf ms (%.3f clock ms).", spec1.tv_sec * 1000 + spec1.tv_nsec / 1000000.0, (double)(clock() - cstart) / CLOCKS_PER_SEC * 1000);

            if (tsleep = tstart + (long long) data->interval - (long long) time(NULL), tsleep >= 0) {
                sleep(tsleep);
            } else {
                mtwarn(WM_DATABASE_LOGTAG, "Time interval exceeded by %lld seconds.", -tsleep);
            }
        }
#ifdef INOTIFY_ENABLED
    }
#endif

    return NULL;
}

// Update manager information
void wm_sync_manager() {
    agent_info_data *manager_data = NULL;
    char *os_uname = NULL;

    os_calloc(1, sizeof(agent_info_data), manager_data);
    os_calloc(1, sizeof(os_data), manager_data->osd);
    os_calloc(HOST_NAME_MAX, sizeof(char), manager_data->manager_host);

    if (gethostname(manager_data->manager_host, HOST_NAME_MAX) == 0)
        wdb_update_agent_name(0, manager_data->manager_host, &wdb_wmdb_sock);
    else
        mterror(WM_DATABASE_LOGTAG, "Couldn't get manager's hostname: %s.", strerror(errno));

    /* Get node name of the manager in cluster */
    manager_data->node_name = get_node_name();

    if ((os_uname = strdup(getuname()))) {
        char *ptr;

        if ((ptr = strstr(os_uname, " - ")))
            *ptr = '\0';

        parse_uname_string(os_uname, manager_data->osd);

        manager_data->id = 0;
        os_strdup(os_uname, manager_data->osd->os_uname);
        os_strdup(__ossec_name " " __ossec_version, manager_data->version);
        os_strdup(AGENT_CS_ACTIVE, manager_data->connection_status);
        os_strdup("synced", manager_data->sync_status);

        wdb_update_agent_data(manager_data, &wdb_wmdb_sock);

        os_free(os_uname);
    }

    wdb_free_agent_info_data(manager_data);
}

#ifndef LOCAL

void wm_check_agents() {
    static time_t timestamp = 0;
    static ino_t inode = 0;
    struct stat buffer;

    if (stat(KEYS_FILE, &buffer) < 0) {
        mterror(WM_DATABASE_LOGTAG, "Couldn't get client.keys stat: %s.", strerror(errno));
    } else {
        if (buffer.st_mtime != timestamp || buffer.st_ino != inode) {
            /* Synchronize */
            wm_sync_agents(false);
            timestamp = buffer.st_mtime;
            inode = buffer.st_ino;
        }
    }
}

// Synchronize agents
void wm_sync_agents(bool master_first_time) {
    keystore keys = KEYSTORE_INITIALIZER;
    clock_t clock0 = clock();
    struct timespec spec0;
    struct timespec spec1;

    // The client.keys file should only be synchronized with the database in the
    // worker nodes. In the case of the master, this will happen in the writter
    // thread of authd and only one time at the begining of modulesd.
    if (is_worker || (!is_worker && master_first_time)) {
        gettime(&spec0);

        mtdebug1(WM_DATABASE_LOGTAG, "Synchronizing agents.");
        OS_PassEmptyKeyfile();
        OS_ReadKeys(&keys, W_RAW_KEY, 0);

        sync_keys_with_wdb(&keys, &wdb_wmdb_sock);

        OS_FreeKeys(&keys);
        mtdebug1(WM_DATABASE_LOGTAG, "Agents synchronization completed.");
        gettime(&spec1);
        time_sub(&spec1, &spec0);
        mtdebug1(WM_DATABASE_LOGTAG, "wm_sync_agents(): %.3f ms (%.3f clock ms).", spec1.tv_sec * 1000 + spec1.tv_nsec / 1000000.0, (double)(clock() - clock0) / CLOCKS_PER_SEC * 1000);
    }
}

/**
 * @brief Synchronizes a keystore with the agent table of global.db.
 *
 * @param keys The keystore structure to be synchronized
 * @param wdb_sock The socket to be used in the calls to Wazuh DB
 */
void sync_keys_with_wdb(keystore *keys, int *wdb_sock) {
    keyentry *entry;
    char * group;
    char cidr[20];
    int *agents;
    unsigned int i;

    os_calloc(OS_SIZE_65536 + 1, sizeof(char), group);

    // Add new agents to the database

    for (i = 0; i < keys->keysize; i++) {
        entry = keys->keyentries[i];
        int id;

        mdebug2("Synchronizing agent %s '%s'.", entry->id, entry->name);

        if (!(id = atoi(entry->id))) {
            merror("At sync_keys_with_wdb(): invalid ID number.");
            continue;
        }

        if (get_agent_group(atoi(entry->id), group, OS_SIZE_65536 + 1, NULL) < 0) {
            *group = 0;
        }

        if (wdb_insert_agent(id, entry->name, NULL, OS_CIDRtoStr(entry->ip, cidr, 20) ?
                             entry->ip->ip : cidr, entry->raw_key, *group ? group : NULL,1, wdb_sock)) {
            // The agent already exists, update group only.
            mdebug2("The agent %s '%s' already exist in the database.", entry->id, entry->name);
        }
    }

    // Delete from the database all the agents without a key

    if ((agents = wdb_get_all_agents(FALSE, wdb_sock))) {
        char id[9];
        char wdbquery[OS_SIZE_128 + 1];
        char *wdboutput = NULL;
        int error;

        for (i = 0; agents[i] != -1; i++) {
            snprintf(id, 9, "%03d", agents[i]);

            if (OS_IsAllowedID(keys, id) == -1) {
                char *name = wdb_get_agent_name(agents[i], wdb_sock);

                if (wdb_remove_agent(agents[i], wdb_sock) < 0) {
                    mdebug1("Couldn't remove agent %s", id);
                    os_free(name);
                    continue;
                }

                if (wdboutput == NULL) {
                    os_malloc(OS_SIZE_1024, wdboutput);
                }

                snprintf(wdbquery, OS_SIZE_128, "wazuhdb remove %s", id);
                error = wdbc_query_ex(wdb_sock, wdbquery, wdboutput, OS_SIZE_1024);

                if (error == 0) {
                    mdebug1("DB from agent %s was deleted '%s'", id, wdboutput);
                } else {
                    merror("Could not remove the DB of the agent %s. Error: %d.", id, error);
                }

                // Remove agent-related files
                OS_RemoveCounter(id);
                OS_RemoveAgentTimestamp(id);

                if (name == NULL || *name == '\0') {
                    os_free(name);
                    continue;
                }

                delete_diff(name);

                free(name);
            }
        }

        os_free(wdboutput);
        os_free(agents);
    }

    os_free(group);
}

// Clean dangling database files
void wm_clean_dangling_legacy_dbs() {
    if (cldir_ex(WDB_DIR "/agents") == -1 && errno != ENOENT) {
        merror("Unable to clear directory '%s': %s (%d)", WDB_DIR "/agents", strerror(errno), errno);
    }
}

void wm_clean_dangling_wdb_dbs() {
    char path[PATH_MAX];
    char * end = NULL;
    char * name = NULL;
    struct dirent * dirent = NULL;
    DIR * dir;

    if (!(dir = opendir(WDB2_DIR))) {
        mterror(WM_DATABASE_LOGTAG, "Couldn't open directory '%s': %s.", WDB2_DIR, strerror(errno));
        return;
    }

    while ((dirent = readdir(dir)) != NULL) {
        // Taking only databases with numbers as a first character in the names to
        // exclude global.db, global.db-journal, wdb socket, and current directory.
        if (dirent->d_name[0] >= '0' && dirent->d_name[0] <= '9') {
            if (end = strchr(dirent->d_name, '.'), end) {
                int id = (int)strtol(dirent->d_name, &end, 10);

                if (id > 0 && strncmp(end, ".db", 3) == 0 && (name = wdb_get_agent_name(id, &wdb_wmdb_sock)) != NULL) {
                    if (*name == '\0') {
                        // Agent not found.

                        if (snprintf(path, sizeof(path), "%s/%s", WDB2_DIR, dirent->d_name) < (int)sizeof(path)) {
                            mtwarn(WM_DATABASE_LOGTAG, "Removing dangling WDB DB file: '%s'", path);
                            if (remove(path) < 0) {
                                mtdebug1(WM_DATABASE_LOGTAG, DELETE_ERROR, path, errno, strerror(errno));
                            }
                        }
                    }

                    free(name);
                }
            } else {
                mtwarn(WM_DATABASE_LOGTAG, "Strange file found: '%s/%s'", WDB2_DIR, dirent->d_name);
            }
        }
    }

    closedir(dir);
}

// Clean dangling group files
void wm_clean_dangling_groups() {
    char path[PATH_MAX];
    char * name;
    int agent_id;
    struct dirent * dirent = NULL;
    DIR * dir;

    mtdebug1(WM_DATABASE_LOGTAG, "Cleaning directory '%s'.", GROUPS_DIR);
    dir = opendir(GROUPS_DIR);

    if (dir == NULL) {
        mterror(WM_DATABASE_LOGTAG, "Couldn't open directory '%s': %s.", GROUPS_DIR, strerror(errno));
        return;
    }

    while ((dirent = readdir(dir)) != NULL) {
        if (dirent->d_name[0] != '.') {
            os_snprintf(path, sizeof(path), GROUPS_DIR "/%s", dirent->d_name);
            agent_id = atoi(dirent->d_name);

            if (agent_id <= 0) {
                mtwarn(WM_DATABASE_LOGTAG, "Strange file found: '%s/%s'", GROUPS_DIR, dirent->d_name);
                continue;
            }

            name = wdb_get_agent_name(agent_id, &wdb_wmdb_sock);

            if (name == NULL) {
                mterror(WM_DATABASE_LOGTAG, "Couldn't query the name of the agent %d to database", agent_id);
                continue;
            }

            if (*name == '\0') {
                mtdebug2(WM_DATABASE_LOGTAG, "Deleting dangling group file '%s'.", dirent->d_name);
                unlink(path);
            }

            free(name);
        }
    }

    closedir(dir);
}

void wm_sync_multi_groups(const char *dirname) {

    wdb_update_groups(dirname, &wdb_wmdb_sock);
}

void wm_sync_legacy_groups_files() {
    DIR *dir = opendir(GROUPS_DIR);

    if (!dir) {
        mdebug1("Couldn't open directory '%s': %s.", GROUPS_DIR, strerror(errno));
        return;
    }

    mtdebug1(WM_DATABASE_LOGTAG, "Scanning directory '%s'.", GROUPS_DIR);

    struct dirent *dir_entry = NULL;
    int sync_result = OS_INVALID;
    char group_file_path[OS_SIZE_512] = {0};
    bool is_dir_empty = true;

    while ((dir_entry = readdir(dir)) != NULL) {
        if (dir_entry->d_name[0] != '.') {
            is_dir_empty = false;
            snprintf(group_file_path, OS_SIZE_512, "%s/%s", GROUPS_DIR, dir_entry->d_name);

            if (is_worker) {
                mdebug1("Group file '%s' won't be synced in a worker node, removing.", group_file_path);
                unlink(group_file_path);
            } else {
                sync_result = wm_sync_group_file(dir_entry->d_name, group_file_path);

                if (OS_SUCCESS == sync_result) {
                    mdebug1("Group file '%s' successfully synced, removing.", group_file_path);
                    unlink(group_file_path);
                } else {
                    merror("Failed during the groups file '%s' syncronization.", group_file_path);
                }
            }
        }
    }
    closedir(dir);

    if (is_dir_empty) {
        if (rmdir(GROUPS_DIR)) {
            mdebug1("Unable to remove directory '%s': '%s' (%d) ", GROUPS_DIR, strerror(errno), errno);
        }
    }
}

int wm_sync_group_file (const char* group_file, const char* group_file_path) {
    int id_agent = atoi(group_file);

    if (id_agent <= 0) {
        mdebug1("Couldn't extract agent ID from file '%s'.", group_file_path);
        return OS_INVALID;
    }

    FILE *fp = fopen(group_file_path, "r");

    if (!fp) {
        mdebug1("Groups file '%s' could not be opened for syncronization.", group_file_path);
        return OS_INVALID;
    }

    char *groups_csv = NULL;
    os_calloc(OS_SIZE_65536 + 1, sizeof(char), groups_csv);
    int result = OS_INVALID;

    if (fgets(groups_csv, OS_SIZE_65536, fp)) {
        char *endl = strchr(groups_csv, '\n');

        if (endl) {
            *endl = '\0';
        }

        result = wdb_set_agent_groups_csv(id_agent, groups_csv, "override", "synced", "local", &wdb_wmdb_sock);
    } else {
        mdebug1("Empty group file '%s'.", group_file_path);
        result = OS_SUCCESS;
    }

    fclose(fp);
    os_free(groups_csv);

    return result;
}

#endif // LOCAL

int wm_sync_shared_group(const char *fname) {
    int result = 0;
    char path[PATH_MAX];
    DIR *dp;
    clock_t clock0 = clock();

    snprintf(path,PATH_MAX, "%s/%s",SHAREDCFG_DIR, fname);

    dp = opendir(path);

    /* The group was deleted */
    if (!dp) {
        wdb_remove_group_db(fname, &wdb_wmdb_sock);
    }
    else {
        if(wdb_find_group(fname, &wdb_wmdb_sock) <= 0){
            wdb_insert_group(fname, &wdb_wmdb_sock);
        }
        closedir(dp);
    }
    mtdebug2(WM_DATABASE_LOGTAG, "wm_sync_shared_group(): %.3f ms.", (double)(clock() - clock0) / CLOCKS_PER_SEC * 1000);
    return result;
}

int wm_sync_file(const char *dirname, const char *fname) {
    char path[PATH_MAX] = "";
    int result = OS_INVALID;

    mtdebug2(WM_DATABASE_LOGTAG, "Synchronizing file '%s/%s'", dirname, fname);

    if (snprintf(path, PATH_MAX, "%s/%s", dirname, fname) >= PATH_MAX) {
        mterror(WM_DATABASE_LOGTAG, "At wm_sync_file(): Path '%s/%s' exceeded length limit.", dirname, fname);
        return result;
    }

    if (!strcmp(dirname, SHAREDCFG_DIR)) {
        result = wm_sync_shared_group(fname);
    } else {
        mterror(WM_DATABASE_LOGTAG, "Directory name '%s' not recognized.", dirname);
        return result;
    }

    return result;
}


// Get read data

cJSON *wm_database_dump(const wm_database *data) {

    cJSON *root = cJSON_CreateObject();
    cJSON *wm_db = cJSON_CreateObject();

    if (data->sync_agents) cJSON_AddStringToObject(wm_db,"sync_agents","yes"); else cJSON_AddStringToObject(wm_db,"sync_agents","no");
    if (data->real_time) cJSON_AddStringToObject(wm_db,"real_time","yes"); else cJSON_AddStringToObject(wm_db,"real_time","no");
    cJSON_AddNumberToObject(wm_db,"interval",data->interval);
    cJSON_AddNumberToObject(wm_db,"max_queued_events",data->max_queued_events);

    cJSON_AddItemToObject(root,"database",wm_db);

    return root;
}


// Destroy data
void* wm_database_destroy(wm_database *data) {
    free(data);
    return NULL;
}

// Read configuration and return a module (if enabled) or NULL (if disabled)
wmodule* wm_database_read() {
#ifdef CLIENT
    // This module won't be available on agents
    return NULL;
#else
    wm_database data;
    wmodule *module = NULL;

    data.sync_agents = getDefine_Int("wazuh_database", "sync_agents", 0, 1);
    data.real_time = getDefine_Int("wazuh_database", "real_time", 0, 1);
    data.interval = getDefine_Int("wazuh_database", "interval", 0, 86400);
    data.max_queued_events = getDefine_Int("wazuh_database", "max_queued_events", 0, INT_MAX);

    if (data.sync_agents) {
        os_calloc(1, sizeof(wmodule), module);
        os_calloc(1, sizeof(wm_database), module->data);
        module->context = &WM_DATABASE_CONTEXT;
        memcpy(module->data, &data, sizeof(wm_database));
        module->tag = strdup(module->context->name);
    }

    return module;
#endif
}

#ifdef INOTIFY_ENABLED

/* Get current inotify queued events limit */
int get_max_queued_events() {
    int size;
    int n;
    FILE *fp;

    if (!(fp = fopen(MAX_QUEUED_EVENTS_PATH, "r"))) {
        mterror(WM_DATABASE_LOGTAG, FOPEN_ERROR, MAX_QUEUED_EVENTS_PATH, errno, strerror(errno));
        return -1;
    }

    n = fscanf(fp, "%d", &size);
    fclose(fp);

    if (n == 1) {
        return size;
    } else {
        return -1;
    }
}

/* Set current inotify queued events limit */
int set_max_queued_events(int size) {
    FILE *fp;

    if (!(fp = fopen(MAX_QUEUED_EVENTS_PATH, "w"))) {
        mterror(WM_DATABASE_LOGTAG, FOPEN_ERROR, MAX_QUEUED_EVENTS_PATH, errno, strerror(errno));
        return -1;
    }

    fprintf(fp, "%d\n", size);
    fclose(fp);
    return 0;
}

// Setup inotify reader
void wm_inotify_setup(wm_database * data) {
    int old_max_queued_events = -1;

    // Create hash table

    if (ptable = OSHash_Create(), !ptable) {
        merror_exit("At wm_inotify_setup(): OSHash_Create()");
    }

    // Create queue
    if (queue = queue_init(data->max_queued_events > 0 ? data->max_queued_events : 16384), !queue) {
        merror_exit("At wm_inotify_setup(): queue_init()");
    }

    // Set inotify queued events limit

    if (data->max_queued_events) {
        old_max_queued_events = get_max_queued_events();

        if (old_max_queued_events >= 0 && old_max_queued_events != data->max_queued_events) {
            mtdebug1(WM_DATABASE_LOGTAG, "Setting inotify queued events limit to '%d'", data->max_queued_events);

            if (set_max_queued_events(data->max_queued_events) < 0) {
                // Error: do not reset then
                old_max_queued_events = -1;
            }
        }
    }

    // Start inotify

    if (inotify_fd = inotify_init1(IN_CLOEXEC), inotify_fd < 0) {
        mterror_exit(WM_DATABASE_LOGTAG, "Couldn't init inotify: %s.", strerror(errno));
    }

    // Reset inotify queued events limit

    if (old_max_queued_events >= 0 && old_max_queued_events != data->max_queued_events) {
        mtdebug2(WM_DATABASE_LOGTAG, "Restoring inotify queued events limit to '%d'", old_max_queued_events);
        set_max_queued_events(old_max_queued_events);
    }

    // Run thread
    w_create_thread(wm_inotify_start, NULL);

    // First synchronization and add watch for client.keys, Syscheck and Rootcheck directories

#ifndef LOCAL

    char keysfile_path[PATH_MAX] = KEYS_FILE;
    char * keysfile_dir = dirname(keysfile_path);

    if (data->sync_agents) {
        if ((wd_agents = inotify_add_watch(inotify_fd, keysfile_dir, IN_CLOSE_WRITE | IN_MOVED_TO)) < 0)
            mterror(WM_DATABASE_LOGTAG, "Couldn't watch client.keys file: %s.", strerror(errno));

        mtdebug2(WM_DATABASE_LOGTAG, "wd_agents='%d'", wd_agents);

        if ((wd_groups = inotify_add_watch(inotify_fd, GROUPS_DIR, IN_CLOSE_WRITE | IN_MOVED_TO | IN_DELETE)) < 0)
            mterror(WM_DATABASE_LOGTAG, "Couldn't watch the agent groups directory: %s.", strerror(errno));

        mtdebug2(WM_DATABASE_LOGTAG, "wd_groups='%d'", wd_groups);

        if ((wd_shared_groups = inotify_add_watch(inotify_fd, SHAREDCFG_DIR, IN_CLOSE_WRITE | IN_MOVED_TO | IN_MOVED_FROM | IN_CREATE | IN_DELETE)) < 0)
            mterror(WM_DATABASE_LOGTAG, "Couldn't watch the shared groups directory: %s.", strerror(errno));

        mtdebug2(WM_DATABASE_LOGTAG, "wd_shared_groups='%d'", wd_shared_groups);

        wm_sync_agents(false);
        wm_sync_multi_groups(SHAREDCFG_DIR);
        wdb_agent_belongs_first_time(&wdb_wmdb_sock);
        wm_clean_dangling_groups();
        wm_clean_dangling_legacy_dbs();
        wm_clean_dangling_wdb_dbs();
    }

#endif
}

// Real time inotify reader thread
static void * wm_inotify_start(__attribute__((unused)) void * args) {
    char buffer[IN_BUFFER_SIZE];
    char keysfile_dir[PATH_MAX] = KEYS_FILE;
    char * keysfile;
    struct inotify_event *event;
    char * dirname = NULL;
    ssize_t count;
    size_t i;

    if (!(keysfile = strrchr(keysfile_dir, '/'))) {
        mterror_exit(WM_DATABASE_LOGTAG, "Couldn't decode keys file path '%s'.", keysfile_dir);
    }

    *(keysfile++) = '\0';

    while (1) {
        // Wait for changes

        mtdebug1(WM_DATABASE_LOGTAG, "Waiting for event notification...");

        do {
            if (count = read(inotify_fd, buffer, IN_BUFFER_SIZE), count < 0) {
                if (errno != EAGAIN)
                    mterror(WM_DATABASE_LOGTAG, "read(): %s.", strerror(errno));

                break;
            }

            buffer[count - 1] = '\0';

            for (i = 0; i < (size_t)count; i += (ssize_t)(sizeof(struct inotify_event) + event->len)) {
                event = (struct inotify_event*)&buffer[i];
                mtdebug2(WM_DATABASE_LOGTAG, "inotify: i='%zu', name='%s', mask='%u', wd='%d'", i, event->name, event->mask, event->wd);

                if (event->len > IN_BUFFER_SIZE) {
                    mterror(WM_DATABASE_LOGTAG, "Inotify event too large (%u)", event->len);
                    break;
                }

                if (event->name[0] == '.') {
                    mtdebug2(WM_DATABASE_LOGTAG, "Discarding hidden file.");
                    continue;
                }
#ifndef LOCAL
                if (event->wd == wd_agents) {
                    if (!strcmp(event->name, keysfile)) {
                        dirname = keysfile_dir;
                    } else {
                        continue;
                    }
                } else if (event->wd == wd_groups) {
                    dirname = GROUPS_DIR;
                } else if (event->wd == wd_shared_groups) {
                    dirname = SHAREDCFG_DIR;
                } else
#endif
                if (event->wd == -1 && event->mask == IN_Q_OVERFLOW) {
                    mterror(WM_DATABASE_LOGTAG, "Inotify event queue overflowed.");
                    continue;
                } else {
                    mterror(WM_DATABASE_LOGTAG, "Unknown watch descriptor '%d', mask='%u'.", event->wd, event->mask);
                    continue;
                }

                wm_inotify_push(dirname, event->name);
            }
        } while (count > 0);
    }

    return NULL;
}

// Insert request into internal structure
void wm_inotify_push(const char * dirname, const char * fname) {
    char path[PATH_MAX + 1];
    char * dup;

    if (snprintf(path, sizeof(path), "%s/%s", dirname, fname) >= (int)sizeof(path)) {
        mterror(WM_DATABASE_LOGTAG, "At wm_inotify_push(): Path too long: '%s'/'%s'", dirname, fname);
        return;
    }

    w_mutex_lock(&mutex_queue);

    if (queue_full(queue)) {
        mterror(WM_DATABASE_LOGTAG, "Internal queue is full (%zu).", queue->size);
        goto end;
    }

    switch (OSHash_Add(ptable, path, (void *)1)) {
    case 0:
        mterror(WM_DATABASE_LOGTAG, "Couldn't insert key into table.");
        break;

    case 1:
        mtdebug2(WM_DATABASE_LOGTAG, "Adding '%s': file already exists at path table.", path);
        break;

    case 2:
        os_strdup(path, dup);
        mtdebug2(WM_DATABASE_LOGTAG, "Adding '%s' to path table.", path);

        if (queue_push(queue, dup) < 0) {
            mterror(WM_DATABASE_LOGTAG, "Couldn't insert key into queue.");
            free(dup);
        }

        w_cond_signal(&cond_pending);
    }

end:
    w_mutex_unlock(&mutex_queue);
}

// Extract enqueued path from internal structure
char * wm_inotify_pop() {
    char * path;

    w_mutex_lock(&mutex_queue);

    while (queue_empty(queue)) {
        w_cond_wait(&cond_pending, &mutex_queue);
    }

    path = queue_pop(queue);

    if (!OSHash_Delete(ptable, path)) {
        mterror(WM_DATABASE_LOGTAG, "Couldn't delete key '%s' from path table.", path);
    }

    w_mutex_unlock(&mutex_queue);
    mtdebug2(WM_DATABASE_LOGTAG, "Taking '%s' from path table.", path);
    return path;
}

#endif // INOTIFY_ENABLED

#endif // !WIN32
