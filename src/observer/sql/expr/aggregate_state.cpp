/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "sql/expr/aggregate_state.h"
#include <stdint.h>

#ifdef USE_SIMD
#include "common/math/simd_util.h"
#endif
template <typename T>
void SumState<T>::update(const T *values, int size)
{
  if (size <= 0) {
    return;
  }
  has_value = true;
#ifdef USE_SIMD
  if constexpr (is_same<T, float>::value) {
    value += mm256_sum_ps(values, size);
  } else if constexpr (is_same<T, int>::value) {
    value += mm256_sum_epi32(values, size);
  }
#else
  for (int i = 0; i < size; ++i) {
 	  value += values[i];
  }
#endif
}

template <typename T>
void AvgState<T>::update(const T *values, int size)
{
  for (int i = 0; i < size; ++i) {
    value += values[i];
  }
  count += size;
}

template <typename T>
void CountState<T>::update(const T *values, int size)
{
  value += size;
}

template <typename T>
void MaxState<T>::update(const T *values, int size)
{
  for (int i = 0; i < size; ++i) {
    if (!has_value || values[i] > value) {
      value     = values[i];
      has_value = true;
    }
  }
}

template <typename T>
void MinState<T>::update(const T *values, int size)
{
  for (int i = 0; i < size; ++i) {
    if (!has_value || values[i] < value) {
      value     = values[i];
      has_value = true;
    }
  }
}

void* create_aggregate_state(AggregateExpr::Type aggr_type, AttrType attr_type)
{
  void* state_ptr = nullptr;
  if (aggr_type == AggregateExpr::Type::SUM) {
    AttrType use_type = (attr_type == AttrType::UNDEFINED) ? AttrType::INTS : attr_type;
    if (use_type == AttrType::INTS) {
      state_ptr = malloc(sizeof(SumState<int>));
      new (state_ptr) SumState<int>();
    } else if (use_type == AttrType::FLOATS) {
      state_ptr = malloc(sizeof(SumState<float>));
      new (state_ptr) SumState<float>();
    }
  } else if (aggr_type == AggregateExpr::Type::COUNT) {
    state_ptr = malloc(sizeof(CountState<int>));
    new (state_ptr) CountState<int>();
  } else if (aggr_type == AggregateExpr::Type::AVG) {
    AttrType use_type = (attr_type == AttrType::UNDEFINED) ? AttrType::INTS : attr_type;
    if (use_type == AttrType::INTS) {
      state_ptr = malloc(sizeof(AvgState<int>));
      new (state_ptr) AvgState<int>();
    } else if (use_type == AttrType::FLOATS) {
      state_ptr = malloc(sizeof(AvgState<float>));
      new (state_ptr) AvgState<float>();
    }
  } else if (aggr_type == AggregateExpr::Type::MAX) {
    AttrType use_type = (attr_type == AttrType::UNDEFINED) ? AttrType::INTS : attr_type;
    if (use_type == AttrType::INTS) {
      state_ptr = malloc(sizeof(MaxState<int>));
      new (state_ptr) MaxState<int>();
    } else if (use_type == AttrType::FLOATS) {
      state_ptr = malloc(sizeof(MaxState<float>));
      new (state_ptr) MaxState<float>();
    }
  } else if (aggr_type == AggregateExpr::Type::MIN) {
    AttrType use_type = (attr_type == AttrType::UNDEFINED) ? AttrType::INTS : attr_type;
    if (use_type == AttrType::INTS) {
      state_ptr = malloc(sizeof(MinState<int>));
      new (state_ptr) MinState<int>();
    } else if (use_type == AttrType::FLOATS) {
      state_ptr = malloc(sizeof(MinState<float>));
      new (state_ptr) MinState<float>();
    }
  }
  return state_ptr;
}

RC aggregate_state_update_by_value(void *state, AggregateExpr::Type aggr_type, AttrType attr_type, const Value& val)
{
  RC rc = RC::SUCCESS;
  if (val.attr_type() == AttrType::UNDEFINED) {
    if (aggr_type == AggregateExpr::Type::SUM || aggr_type == AggregateExpr::Type::AVG ||
        aggr_type == AggregateExpr::Type::MAX || aggr_type == AggregateExpr::Type::MIN) {
      return RC::SUCCESS;
    }
    if (aggr_type == AggregateExpr::Type::COUNT) {
      return RC::SUCCESS;
    }
  }
  AttrType use_type = (attr_type == AttrType::UNDEFINED) ? AttrType::INTS : attr_type;
  if (aggr_type == AggregateExpr::Type::SUM) {
    if (use_type == AttrType::INTS) {
      static_cast<SumState<int>*>(state)->update(val.get_int());
    } else if (use_type == AttrType::FLOATS) {
      static_cast<SumState<float>*>(state)->update(val.get_float());
    } else {
      return RC::UNIMPLEMENTED;
    }
  } else if (aggr_type == AggregateExpr::Type::COUNT) {
    static_cast<CountState<int>*>(state)->update(1);
  } else if (aggr_type == AggregateExpr::Type::AVG) {
    if (use_type == AttrType::INTS) {
      static_cast<AvgState<int>*>(state)->update(val.get_int());
    } else if (use_type == AttrType::FLOATS) {
      static_cast<AvgState<float>*>(state)->update(val.get_float());
    } else {
      return RC::UNIMPLEMENTED;
    }
  } else if (aggr_type == AggregateExpr::Type::MAX) {
    if (use_type == AttrType::INTS) {
      static_cast<MaxState<int>*>(state)->update(val.get_int());
    } else if (use_type == AttrType::FLOATS) {
      static_cast<MaxState<float>*>(state)->update(val.get_float());
    } else {
      return RC::UNIMPLEMENTED;
    }
  } else if (aggr_type == AggregateExpr::Type::MIN) {
    if (use_type == AttrType::INTS) {
      static_cast<MinState<int>*>(state)->update(val.get_int());
    } else if (use_type == AttrType::FLOATS) {
      static_cast<MinState<float>*>(state)->update(val.get_float());
    } else {
      return RC::UNIMPLEMENTED;
    }
  } else {
    return RC::UNIMPLEMENTED;
  }
  return rc;
}

template <class STATE, typename T>
void append_to_column(void *state, Column &column)
{
  STATE *state_ptr = reinterpret_cast<STATE *>(state);
  T res = state_ptr->template finalize<T>();
  column.append_one((char *)&res);
}

RC finialize_aggregate_state(void *state, AggregateExpr::Type aggr_type, AttrType attr_type, Column& col)
{
  RC rc = RC::SUCCESS;
  AttrType use_type = (attr_type == AttrType::UNDEFINED) ? AttrType::INTS : attr_type;
  if ( aggr_type == AggregateExpr::Type::SUM) {
    if (use_type == AttrType::INTS) {
      if (!static_cast<SumState<int>*>(state)->has_value) {
        rc = col.append_null();
      } else {
        append_to_column<SumState<int>, int>(state, col);
      }
    } else if (use_type == AttrType::FLOATS) {
      if (!static_cast<SumState<float>*>(state)->has_value) {
        rc = col.append_null();
      } else {
        append_to_column<SumState<float>, float>(state, col);
      }
    } else {
      rc = RC::UNIMPLEMENTED;
    }
  } else if (aggr_type == AggregateExpr::Type::COUNT) {
    append_to_column<CountState<int>, int>(state, col);
  } else if (aggr_type == AggregateExpr::Type::AVG) {
    if (use_type == AttrType::INTS) {
      append_to_column<AvgState<int>, float>(state, col);
    } else if (use_type == AttrType::FLOATS) {
      append_to_column<AvgState<float>, float>(state, col);
    } else {
      rc = RC::UNIMPLEMENTED;
    }
  } else if (aggr_type == AggregateExpr::Type::MAX) {
    if (use_type == AttrType::INTS) {
      append_to_column<MaxState<int>, int>(state, col);
    } else if (use_type == AttrType::FLOATS) {
      append_to_column<MaxState<float>, float>(state, col);
    } else {
      rc = RC::UNIMPLEMENTED;
    }
  } else if (aggr_type == AggregateExpr::Type::MIN) {
    if (use_type == AttrType::INTS) {
      append_to_column<MinState<int>, int>(state, col);
    } else if (use_type == AttrType::FLOATS) {
      append_to_column<MinState<float>, float>(state, col);
    } else {
      rc = RC::UNIMPLEMENTED;
    }
  } else {
    rc = RC::UNIMPLEMENTED;
  }
  return rc;
}

template <class STATE, typename T>
void update_aggregate_state(void *state, const Column &column)
{
  STATE *state_ptr = reinterpret_cast<STATE *>(state);
  T *    data      = (T *)column.data();
  state_ptr->update(data, column.count());
}

RC aggregate_state_update_by_column(void *state, AggregateExpr::Type aggr_type, AttrType attr_type, Column &col)
{
  RC rc = RC::SUCCESS;
  // 当表达式类型为 UNDEFINED 时，尝试使用列的实际类型（如 FLOATS），避免 AVG/SUM 等聚合错误地使用 INTS
  AttrType use_type = attr_type;
  if (use_type == AttrType::UNDEFINED) {
    if (col.attr_type() == AttrType::FLOATS || col.attr_type() == AttrType::INTS) {
      use_type = col.attr_type();
    } else {
      use_type = AttrType::INTS;
    }
  }
  if (aggr_type == AggregateExpr::Type::COUNT) {
    static_cast<CountState<int> *>(state)->value += col.count();
    return RC::SUCCESS;
  }
  if (aggr_type == AggregateExpr::Type::SUM) {
    for (int i = 0; i < col.count(); i++) {
      Value v = col.get_value(i);
      if (v.attr_type() != AttrType::UNDEFINED) {
        rc = aggregate_state_update_by_value(state, AggregateExpr::Type::SUM, use_type, v);
        if (rc != RC::SUCCESS) {
          return rc;
        }
      }
    }
  } else if (aggr_type == AggregateExpr::Type::AVG) {
    if (use_type == AttrType::INTS) {
      update_aggregate_state<AvgState<int>, int>(state, col);
    } else if (use_type == AttrType::FLOATS) {
      update_aggregate_state<AvgState<float>, float>(state, col);
    } else {
      rc = RC::UNIMPLEMENTED;
    }
  } else if (aggr_type == AggregateExpr::Type::MAX) {
    if (use_type == AttrType::INTS) {
      update_aggregate_state<MaxState<int>, int>(state, col);
    } else if (use_type == AttrType::FLOATS) {
      update_aggregate_state<MaxState<float>, float>(state, col);
    } else {
      rc = RC::UNIMPLEMENTED;
    }
  } else if (aggr_type == AggregateExpr::Type::MIN) {
    if (use_type == AttrType::INTS) {
      update_aggregate_state<MinState<int>, int>(state, col);
    } else if (use_type == AttrType::FLOATS) {
      update_aggregate_state<MinState<float>, float>(state, col);
    } else {
      rc = RC::UNIMPLEMENTED;
    }
  } else {
    rc = RC::UNIMPLEMENTED;
  }
  return rc;
}

template class SumState<int>;
template class SumState<float>;

template class CountState<int>;

template class AvgState<int>;
template class AvgState<float>;

template class MaxState<int>;
template class MaxState<float>;

template class MinState<int>;
template class MinState<float>;
