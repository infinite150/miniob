#include "common/type/text_type.h"

#include "common/lang/comparator.h"
#include "common/lang/sstream.h"
#include "common/log/log.h"
#include "common/value.h"

int TextType::compare(const Value &left, const Value &right) const
{
  ASSERT(left.attr_type() == AttrType::TEXTS, "left type is not text");
  ASSERT(is_string_type(right.attr_type()), "right type is not string");
  return common::compare_string((void *)left.data(), left.length(), (void *)right.data(), right.length());
}

RC TextType::set_value_from_str(Value &val, const string &data) const
{
  val.set_text(data.c_str());
  return RC::SUCCESS;
}

RC TextType::cast_to(const Value &val, AttrType type, Value &result) const
{
  switch (type) {
    case AttrType::CHARS:
      result.set_string(val.get_string().c_str());
      return RC::SUCCESS;
    case AttrType::TEXTS:
      result.set_text(val.get_string().c_str());
      return RC::SUCCESS;
    default:
      return RC::UNIMPLEMENTED;
  }
}

int TextType::cast_cost(AttrType type)
{
  if (type == AttrType::TEXTS) {
    return 0;
  }
  if (type == AttrType::CHARS) {
    return 1;
  }
  return INT32_MAX;
}

RC TextType::to_string(const Value &val, string &result) const
{
  if (val.data() == nullptr) {
    result.clear();
    return RC::SUCCESS;
  }
  stringstream ss;
  ss << val.data();
  result = ss.str();
  return RC::SUCCESS;
}
