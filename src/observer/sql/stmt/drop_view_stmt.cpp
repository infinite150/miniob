/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

//
// Created by Codex on 2026/4/13.
//

#include "sql/stmt/drop_view_stmt.h"

#include "common/lang/string.h"
#include "common/log/log.h"
#include "storage/db/db.h"

RC DropViewStmt::create(Db *db, const DropViewSqlNode &drop_view, Stmt *&stmt)
{
  stmt = nullptr;
  if (db == nullptr || common::is_blank(drop_view.view_name.c_str())) {
    LOG_WARN("invalid drop view args. db=%p, view_name=%s", db, drop_view.view_name.c_str());
    return RC::INVALID_ARGUMENT;
  }

  if (db->find_view(drop_view.view_name.c_str()) == nullptr) {
    LOG_WARN("view not exist: %s", drop_view.view_name.c_str());
    return RC::SCHEMA_TABLE_NOT_EXIST;
  }

  stmt = new DropViewStmt(drop_view.view_name);
  return RC::SUCCESS;
}

