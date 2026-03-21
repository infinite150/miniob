/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#pragma once

#include "sql/operator/physical_operator.h"
#include "sql/expr/tuple.h"
#include "common/lang/vector.h"

class Trx;
class FieldMeta;

/**
 * @brief 物理算子：更新（与 Insert/Delete 类似，通过子算子扫描待更新行，逐行 delete + insert 实现）
 * @ingroup PhysicalOperator
 */
class UpdatePhysicalOperator : public PhysicalOperator
{
public:
  UpdatePhysicalOperator(Table *table, const vector<const FieldMeta *> &field_metas, const vector<Value> &values);
  virtual ~UpdatePhysicalOperator() = default;

  PhysicalOperatorType type() const override { return PhysicalOperatorType::UPDATE; }
  OpType               get_op_type() const override { return OpType::UPDATE; }

  RC open(Trx *trx) override;
  RC next() override;
  RC close() override;

  /// UPDATE 不返回结果集，清空 schema 以便 write_result 走 cell_num==0 分支并返回 SUCCESS/FAILURE
  RC tuple_schema(TupleSchema &schema) const override;

  Tuple *current_tuple() override { return nullptr; }

private:
  /// 根据旧记录与 SET 值构造新记录（仅替换目标字段）
  RC build_new_record(const Record &old_record, Record &new_record) const;

private:
  Table                    *table_       = nullptr;
  vector<const FieldMeta *> field_metas_;
  vector<Value>             values_;
  Trx                      *trx_         = nullptr;
};
