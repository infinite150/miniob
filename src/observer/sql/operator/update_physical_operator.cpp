/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "sql/operator/update_physical_operator.h"

#include <unordered_map>

#include "common/log/log.h"
#include "common/value.h"
#include "session/session.h"
#include "sql/expr/expression.h"
#include "sql/expr/expression_iterator.h"
#include "storage/table/table.h"
#include "storage/trx/trx.h"
#include "sql/expr/tuple.h"

UpdatePhysicalOperator::UpdatePhysicalOperator(
    Table *table, const vector<const FieldMeta *> &field_metas, vector<unique_ptr<Expression>> *rhs_exprs, Session *subquery_session)
    : table_(table), field_metas_(field_metas), rhs_exprs_(rhs_exprs), trx_(nullptr), subquery_session_(subquery_session)
{}

RC UpdatePhysicalOperator::open(Trx *trx)
{
  trx_ = trx;
  if (table_ != nullptr) {
    table_->add_ref();
  }
  Session *session = subquery_session_ != nullptr ? subquery_session_ : Session::current_session();
  if (rhs_exprs_ != nullptr) {
    for (auto &e : *rhs_exprs_) {
      if (e) {
        RC prep_rc = RC::SUCCESS;
        ExpressionIterator::for_each_subquery(*e, [session, trx, &prep_rc](SubQueryExpr &sq) {
          if (prep_rc != RC::SUCCESS) {
            return;
          }
          if (session != nullptr) {
            RC rc = sq.generate_logical_oper();
            if (OB_FAIL(rc)) {
              prep_rc = rc;
              return;
            }
            rc = sq.generate_physical_oper(session);
            if (OB_FAIL(rc)) {
              prep_rc = rc;
              return;
            }
          }
          sq.set_trx(trx);
        });
        if (OB_FAIL(prep_rc)) {
          LOG_WARN("UpdatePhysicalOperator: subquery plan generation failed. rc=%s", strrc(prep_rc));
          // 仅由 add_ref 对称：失败时 SqlResult 仍会 close()，此处不得 release，避免双重 release
          return prep_rc;
        }
      }
    }
  }
  if (children_.empty()) {
    LOG_WARN("UpdatePhysicalOperator::open has no child, table=%s", table_ ? table_->name() : "null");
    return RC::SUCCESS;
  }

  RC rc = children_[0]->open(trx);
  if (OB_FAIL(rc)) {
    LOG_WARN("UpdatePhysicalOperator::open failed at children_[0]->open. rc=%s", strrc(rc));
    return rc;
  }

  vector<Record> old_records;
  while (OB_SUCC(rc = children_[0]->next())) {
    Tuple *tuple = children_[0]->current_tuple();
    if (tuple == nullptr) {
      LOG_WARN("UpdatePhysicalOperator::open failed at children_[0]->current_tuple, got null tuple");
      return RC::INTERNAL;
    }
    RowTuple *row_tuple = static_cast<RowTuple *>(tuple);
    Record   &record   = row_tuple->record();

    Record rec_copy;
    rc = rec_copy.copy_data(record.data(), record.len());
    if (OB_FAIL(rc)) {
      LOG_WARN("failed to copy record data. rc=%s", strrc(rc));
      return rc;
    }
    rec_copy.set_rid(record.rid());
    old_records.emplace_back(std::move(rec_copy));
  }

  children_[0]->close();

  if (rc != RC::RECORD_EOF) {
    LOG_WARN("child operator error. rc=%s", strrc(rc));
    return rc;
  }

  vector<Record> updated_new;
  // 已成功 delete 旧记录的数量（用于失败回滚时避免对未删除行重复插入）
  size_t         deleted_old_count = 0;
  for (Record &old_record : old_records) {
    Record new_record;
    rc = build_new_record(old_record, new_record);
    if (OB_FAIL(rc)) {
      LOG_WARN("UpdatePhysicalOperator::open failed at build_new_record. table=%s rid=%s rc=%s",
          table_ ? table_->name() : "null", old_record.rid().to_string().c_str(), strrc(rc));
      goto rollback;
    }
    rc = trx_->delete_record(table_, old_record);
    if (OB_FAIL(rc)) {
      LOG_WARN("UpdatePhysicalOperator::open failed at trx_->delete_record. table=%s rid=%s rc=%s",
          table_ ? table_->name() : "null", old_record.rid().to_string().c_str(), strrc(rc));
      goto rollback;
    }
    deleted_old_count++;
    rc = trx_->insert_record(table_, new_record);
    if (OB_FAIL(rc)) {
      LOG_WARN("UpdatePhysicalOperator::open failed at trx_->insert_record. table=%s old_rid=%s rc=%s",
          table_ ? table_->name() : "null", old_record.rid().to_string().c_str(), strrc(rc));
      goto rollback;
    }
    updated_new.emplace_back();
    updated_new.back().copy_data(new_record.data(), new_record.len());
    updated_new.back().set_rid(new_record.rid());
  }

  return RC::SUCCESS;

rollback:
  // MVCC 模式下，当前语句的 delete/insert 已完整记录在事务操作集中，
  // 由外层执行器在语句失败时统一调用 trx->rollback() 回溯。
  // 这里若再做手工补偿（delete+insert）会与事务回滚叠加，放大副作用。
  if (trx_ != nullptr && trx_->type() == TrxKit::Type::MVCC) {
    LOG_WARN("UpdatePhysicalOperator::open rollback path in MVCC mode, defer recovery to outer trx->rollback. rc=%s",
        strrc(rc));
    return rc;
  }

  for (size_t i = 0; i < updated_new.size(); i++) {
    RC rc2 = trx_->delete_record(table_, updated_new[i]);
    if (OB_FAIL(rc2)) {
      LOG_ERROR("rollback: failed to delete new record. rc=%s", strrc(rc2));
    }
    rc2 = trx_->insert_record(table_, old_records[i]);
    if (OB_FAIL(rc2)) {
      LOG_ERROR("rollback: failed to re-insert old record. rc=%s", strrc(rc2));
    }
  }
  // 只恢复“已删除但尚未插入新行”的旧记录，未删除的旧记录不能重复插入。
  for (size_t i = updated_new.size(); i < deleted_old_count; i++) {
    RC rc2 = trx_->insert_record(table_, old_records[i]);
    if (OB_FAIL(rc2)) {
      LOG_ERROR("rollback: failed to re-insert old record (deleted but not inserted). rc=%s", strrc(rc2));
    }
  }
  return rc;
}

RC UpdatePhysicalOperator::next()
{
  return RC::RECORD_EOF;
}

RC UpdatePhysicalOperator::close()
{
  if (table_ != nullptr) {
    table_->release();
  }
  if (!children_.empty()) {
    RC rc = children_[0]->close();
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to close child operator. rc=%s", strrc(rc));
    }
  }
  return RC::SUCCESS;
}

RC UpdatePhysicalOperator::tuple_schema(TupleSchema &schema) const
{
  schema.clear();
  return RC::SUCCESS;
}

RC UpdatePhysicalOperator::build_new_record(const Record &old_record, Record &new_record) const
{
  if (table_ == nullptr || field_metas_.empty() || rhs_exprs_ == nullptr
      || rhs_exprs_->size() != field_metas_.size()) {
    return RC::INTERNAL;
  }

  const TableMeta &table_meta = table_->table_meta();
  const int        sys_fields = table_meta.sys_field_num();
  const int        user_fields = table_meta.field_num() - sys_fields;

  unordered_map<string, const Expression *> update_map;
  for (size_t i = 0; i < field_metas_.size(); i++) {
    update_map[field_metas_[i]->name()] = (*rhs_exprs_)[i].get();
  }

  RowTuple tuple;
  tuple.set_schema(table_, table_meta.field_metas());
  tuple.set_record(const_cast<Record *>(&old_record));

  vector<Value> values;
  values.reserve(user_fields);

  for (int i = 0; i < user_fields; i++) {
    const FieldMeta *field = table_meta.field(i + sys_fields);
    if (field == nullptr) {
      return RC::INTERNAL;
    }
    auto it = update_map.find(field->name());
    if (it != update_map.end()) {
      Value cell;
      RC rc = it->second->get_value(tuple, cell);
      if (OB_FAIL(rc)) {
        return rc;
      }
      if (cell.is_null()) {
        if (!field->nullable()) {
          LOG_WARN("cannot set NULL on NOT NULL field. table=%s, field=%s", table_->name(), field->name());
          return RC::INVALID_ARGUMENT;
        }
      } else if (field->type() != cell.attr_type()) {
        Value casted;
        rc = Value::cast_to(cell, field->type(), casted);
        if (OB_FAIL(rc)) {
          LOG_WARN("field type mismatch on update. table=%s, field=%s", table_->name(), field->name());
          return RC::SCHEMA_FIELD_TYPE_MISMATCH;
        }
        cell = std::move(casted);
      }
      values.emplace_back(std::move(cell));
    } else {
      Value cell;
      RC rc = tuple.cell_at(i + sys_fields, cell);
      if (OB_FAIL(rc)) {
        return rc;
      }
      values.emplace_back(std::move(cell));
    }
  }

  return table_->make_record(static_cast<int>(values.size()), values.data(), new_record);
}
