/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "sql/executor/update_executor.h"

#include "common/log/log.h"
#include "event/session_event.h"
#include "event/sql_event.h"
#include "session/session.h"
#include "sql/optimizer/logical_plan_generator.h"
#include "sql/optimizer/physical_plan_generator.h"
#include "sql/stmt/update_stmt.h"
#include "storage/trx/trx.h"

RC UpdateExecutor::execute(SQLStageEvent *sql_event)
{
  Stmt    *stmt    = sql_event->stmt();
  Session *session = sql_event->session_event()->session();

  if (stmt == nullptr || stmt->type() != StmtType::UPDATE) {
    LOG_WARN("UpdateExecutor can not run this command");
    return RC::INVALID_ARGUMENT;
  }

  unique_ptr<LogicalOperator> logical_operator;
  RC rc = LogicalPlanGenerator().create(stmt, logical_operator);
  if (OB_FAIL(rc)) {
    LOG_WARN("failed to create logical plan for UPDATE. rc=%s", strrc(rc));
    return rc;
  }
  if (logical_operator == nullptr) {
    return RC::INTERNAL;
  }

  unique_ptr<PhysicalOperator> physical_operator;
  rc = PhysicalPlanGenerator().create(*logical_operator, physical_operator, session);
  if (OB_FAIL(rc)) {
    LOG_WARN("failed to create physical plan for UPDATE. rc=%s", strrc(rc));
    return rc;
  }
  if (physical_operator == nullptr) {
    return RC::INTERNAL;
  }

  Trx *trx = session->current_trx();
  trx->start_if_need();

  Session *prev = Session::current_session();
  Session::set_current_session(session);
  rc = physical_operator->open(trx);
  Session::set_current_session(prev);
  if (OB_FAIL(rc)) {
    LOG_WARN("failed to open UPDATE operator. rc=%s", strrc(rc));
    physical_operator->close();
    if (!session->is_trx_multi_operation_mode()) {
      session->current_trx()->rollback();
      session->destroy_trx();
    }
    return rc;
  }

  RC rc_next = RC::SUCCESS;
  while (RC::SUCCESS == (rc_next = physical_operator->next())) {
    (void)physical_operator->current_tuple();
  }
  if (rc_next != RC::RECORD_EOF) {
    LOG_WARN("UPDATE operator next failed. rc=%s", strrc(rc_next));
    rc = rc_next;
  }

  RC rc_close = physical_operator->close();
  if (OB_SUCC(rc)) {
    rc = rc_close;
  }

  if (!session->is_trx_multi_operation_mode()) {
    if (rc == RC::SUCCESS) {
      rc = session->current_trx()->commit();
    } else {
      (void)session->current_trx()->rollback();
    }
    session->destroy_trx();
  }

  return rc;
}
