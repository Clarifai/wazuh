/*
 * Wazuh Syscheck
 * Copyright (C) 2015-2021, Wazuh Inc.
 * October 5, 2021.
 *
 * This program is free software; you can redistribute it
 * and/or modify it under the terms of the GNU General Public
 * License (version 2) as published by the FSF - Free Software
 * Foundation.
 */

#ifndef _DB_TEST_H
#define _DB_TEST_H
#include "gtest/gtest.h"
#include "gmock/gmock.h"
#include "commonDefs.h"

class DBTest : public testing::Test {
    protected:
        DBTest() = default;
        virtual ~DBTest() = default;

        void SetUp() override;
        void TearDown() override;
};

#endif //_DB_TEST_H
