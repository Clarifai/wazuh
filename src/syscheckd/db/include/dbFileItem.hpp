/*
 * Wazuh Syscheckd
 * Copyright (C) 2015-2021, Wazuh Inc.
 * September 23, 2021.
 *
 * This program is free software; you can redistribute it
 * and/or modify it under the terms of the GNU General Public
 * License (version 2) as published by the FSF - Free Software
 * Foundation.
 */

#ifndef _FILEITEM_HPP
#define _FILEITEM_HPP
#include "json.hpp"
#include "dbItem.hpp"


class FileItem final : public DBItem
{
    public:
        FileItem(fim_entry* const fim)
            : DBItem(std::string(fim->file_entry.path)
                     , fim->file_entry.data->scanned
                     , fim->file_entry.data->last_event
                     , fim->file_entry.data->checksum
                     , fim->file_entry.data->mode)
        {
            m_options = fim->file_entry.data->options;
            m_time = fim->file_entry.data->mtime;
            m_size = fim->file_entry.data->size;
            m_dev = fim->file_entry.data->dev;
            m_inode = fim->file_entry.data->inode;
            m_attributes = std::string(fim->file_entry.data->attributes);
            m_gid = std::atoi(fim->file_entry.data->gid);
            m_groupname = std::string(fim->file_entry.data->group_name);
            m_md5 = std::string(fim->file_entry.data->hash_md5);
            m_perm = std::string(fim->file_entry.data->perm);
            m_sha1 = std::string(fim->file_entry.data->hash_sha1);
            m_sha256 = std::string(fim->file_entry.data->hash_sha256);
            m_uid = std::atoi(fim->file_entry.data->uid);
            m_username = std::string(fim->file_entry.data->user_name);
            createJSON();
        }

        FileItem(const nlohmann::json& fim)
            : DBItem(fim.at("path"), fim.at("scanned"), fim.at("last_event"), fim.at("checksum"), fim.at("mode"))
        {
            m_options = fim.at("options");
            m_time = fim.at("mtime");
            m_size = fim.at("size");
            m_dev = fim.at("dev");
            m_inode = fim.at("inode");
            m_attributes = fim.at("attributes");
            m_gid = fim.at("gid");
            m_groupname = fim.at("group_name");
            m_md5 = fim.at("hash_md5");
            m_perm = fim.at("perm");
            m_sha1 = fim.at("hash_sha1");
            m_sha256 = fim.at("hash_sha256");
            m_uid = fim.at("uid");
            m_username = fim.at("user_name");

            m_statementConf = std::make_unique<nlohmann::json>(fim);
        };

        ~FileItem() = default;
        void toFimEntry(fim_entry& fim)
        {
            createFimEntry(fim);
        };

        nlohmann::json* toJSON()
        {
            return m_statementConf.get();
        };

    private:
        int                                             m_options;
        int                                             m_gid;
        int                                             m_uid;
        unsigned int                                    m_size;
        unsigned long int                               m_dev;
        unsigned long int                               m_inode;
        time_t                                          m_time;
        std::string                                     m_attributes;
        std::string                                     m_groupname;
        std::string                                     m_md5;
        std::string                                     m_perm;
        std::string                                     m_sha1;
        std::string                                     m_sha256;
        std::string                                     m_username;
        std::unique_ptr<nlohmann::json>                 m_statementConf;

        void createFimEntry(fim_entry& fim);
        void createJSON();
};
#endif //_FILEITEM_HPP
