#pragma once

#include "common/sys/rc.h"

class SQLStageEvent;

class CreateTableSelectExecutor
{
public:
  CreateTableSelectExecutor()          = default;
  virtual ~CreateTableSelectExecutor() = default;

  RC execute(SQLStageEvent *sql_event);
};
