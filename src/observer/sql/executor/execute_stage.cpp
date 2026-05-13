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
// Created by Longda on 2021/4/13.
//

#include "sql/executor/execute_stage.h"

#include "common/log/log.h"
#include "event/session_event.h"
#include "event/sql_event.h"
#include "common/log/log.h"
#include "session/session.h"
#include "sql/executor/command_executor.h"
#include "sql/executor/sql_result.h"
#include "sql/operator/calc_physical_operator.h"
#include "sql/optimizer/logical_plan_generator.h"
#include "sql/optimizer/physical_plan_generator.h"
#include "sql/parser/parse.h"
#include "sql/parser/parse_defs.h"
#include "sql/stmt/insert_stmt.h"
#include "sql/stmt/select_stmt.h"
#include "sql/stmt/view_rewriter.h"
#include "storage/db/db.h"
#include "storage/table/table.h"
#include "storage/trx/trx.h"
#include "sql/optimizer/logical_plan_generator.h"
#include "sql/optimizer/physical_plan_generator.h"
#include "sql/stmt/select_stmt.h"
#include "sql/stmt/stmt.h"
#include "storage/default/default_handler.h"

using namespace common;

RC ExecuteStage::handle_request(SQLStageEvent *sql_event)
{
  RC rc = RC::SUCCESS;

  unique_ptr<PhysicalOperator> &physical_operator = sql_event->physical_operator();
  Stmt                        *stmt              = sql_event->stmt();

  // UPDATE 始终在 execute 阶段构建物理计划并设置 operator，避免依赖 optimize 阶段
  //（cascade 未设置 winner 或 RBO 路径差异导致的问题）
  if (stmt != nullptr && stmt->type() == StmtType::INSERT) {
    auto *insert_stmt = static_cast<InsertStmt *>(stmt);
    if (insert_stmt->is_insert_select()) {
      Session *session = sql_event->session_event()->session();
      Db *db = session->get_current_db();
      if (db == nullptr) { return RC::SCHEMA_DB_NOT_EXIST; }
      Table *table = db->find_table(insert_stmt->table()->name());
      if (table == nullptr) { return RC::SCHEMA_TABLE_NOT_EXIST; }
      ParsedSqlResult parsed;
      parse(insert_stmt->select_sql().c_str(), &parsed);
      if (parsed.sql_nodes().size() != 1) { return RC::SQL_SYNTAX; }
      ParsedSqlNode *sel_node = parsed.sql_nodes().front().get();
      if (sel_node == nullptr || sel_node->flag != SCF_SELECT) { return RC::INVALID_ARGUMENT; }
      rewrite_sql_for_view(db, *sel_node);
      Stmt *sel_stmt = nullptr;
      RC rc = SelectStmt::create(db, sel_node->selection, sel_stmt);
      if (OB_FAIL(rc)) { return rc; }
      unique_ptr<Stmt> sel_holder(sel_stmt);
      unique_ptr<LogicalOperator> log_op;
      LogicalPlanGenerator().create(sel_stmt, log_op);
      if (log_op == nullptr) { return RC::INTERNAL; }
      log_op->generate_general_child();
      unique_ptr<PhysicalOperator> phy_op;
      PhysicalPlanGenerator().create(*log_op, phy_op, session);
      if (phy_op == nullptr) { return RC::INTERNAL; }
      Trx *trx = session->current_trx();
      Session *prev = Session::current_session();
      Session::set_current_session(session);
      trx->start_if_need();
      rc = phy_op->open(trx);
      if (OB_FAIL(rc)) { Session::set_current_session(prev); return rc; }
      int inserted = 0;
      int field_num = table->table_meta().field_num() - table->table_meta().sys_field_num();
      while (OB_SUCC(rc = phy_op->next())) {
        Tuple *t = phy_op->current_tuple();
        if (t == nullptr) break;
        vector<Value> vals(field_num);
        bool row_ok = true;
        for (int i = 0; i < field_num; i++) {
          if (OB_FAIL(t->cell_at(i, vals[i]))) { row_ok = false; break; }
        }
        if (!row_ok) break;
        Record rec;
        if (OB_FAIL(table->make_record(field_num, vals.data(), rec))) break;
        if (OB_FAIL(table->insert_record(rec))) break;
        inserted++;
      }
      if (rc == RC::RECORD_EOF) rc = RC::SUCCESS;
      phy_op->close();
      Session::set_current_session(prev);
      sql_event->session_event()->sql_result()->set_return_code(rc);
      return rc;
    }
  }

  if (stmt != nullptr && stmt->type() == StmtType::UPDATE) {
    Session *session = sql_event->session_event()->session();
    unique_ptr<LogicalOperator> logical_operator;
    rc = LogicalPlanGenerator().create(stmt, logical_operator);
    if (OB_FAIL(rc)) {
      LOG_WARN("failed to create logical plan for UPDATE. rc=%s", strrc(rc));
      sql_event->session_event()->sql_result()->set_return_code(rc);
      return rc;
    }
    if (logical_operator == nullptr) {
      LOG_WARN("logical plan for UPDATE is null");
      sql_event->session_event()->sql_result()->set_return_code(RC::INTERNAL);
      return RC::INTERNAL;
    }
    unique_ptr<PhysicalOperator> oper;
    rc = PhysicalPlanGenerator().create(*logical_operator, oper, session);
    if (OB_FAIL(rc)) {
      LOG_WARN("failed to create physical plan for UPDATE. rc=%s", strrc(rc));
      sql_event->session_event()->sql_result()->set_return_code(rc);
      return rc;
    }
    if (oper == nullptr) {
      LOG_WARN("physical plan for UPDATE is null");
      sql_event->session_event()->sql_result()->set_return_code(RC::INTERNAL);
      return RC::INTERNAL;
    }
    sql_event->session_event()->sql_result()->set_operator(std::move(oper));
    return RC::SUCCESS;
  }

  if (physical_operator != nullptr) {
    return handle_request_with_physical_operator(sql_event);
  }

  SessionEvent *session_event = sql_event->session_event();

  if (stmt != nullptr) {
    CommandExecutor command_executor;
    rc = command_executor.execute(sql_event);
    session_event->sql_result()->set_return_code(rc);
  } else {
    return RC::INTERNAL;
  }
  return rc;
}

RC ExecuteStage::handle_request_with_physical_operator(SQLStageEvent *sql_event)
{
  RC rc = RC::SUCCESS;

  unique_ptr<PhysicalOperator> &physical_operator = sql_event->physical_operator();
  ASSERT(physical_operator != nullptr, "physical operator should not be null");

  SqlResult *sql_result = sql_event->session_event()->sql_result();
  sql_result->set_operator(std::move(physical_operator));
  return rc;
}
