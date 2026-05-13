"""Implement CREATE TABLE ... AS SELECT in miniob."""
import os
base = "/root/miniob"

def rpl(path, old, new):
    with open(path) as f: c = f.read()
    if old not in c:
        print(f"MISS: {os.path.basename(path)}: {repr(old[:60])}")
        return False
    c = c.replace(old, new)
    with open(path, "w") as f: f.write(c)
    print(f"  OK: {os.path.basename(path)}")
    return True

# ===== 1. parse_defs.h =====
f = base + "/src/observer/sql/parser/parse_defs.h"

rpl(f,
    "struct CreateTableSqlNode\n{\n  string                  relation_name;  ///< Relation name\n  vector<AttrInfoSqlNode> attr_infos;     ///< attributes\n  vector<string>          primary_keys;   ///< primary keys\n  // TODO: integrate to CreateTableOptions\n  string storage_format;  ///< storage format\n  string storage_engine;  ///< storage engine\n};",
    "struct CreateTableSqlNode\n{\n  string                  relation_name;  ///< Relation name\n  vector<AttrInfoSqlNode> attr_infos;     ///< attributes\n  vector<string>          primary_keys;   ///< primary keys\n  // TODO: integrate to CreateTableOptions\n  string storage_format;  ///< storage format\n  string storage_engine;  ///< storage engine\n};\n\n/**\n * @brief create table as select\n * @ingroup SQLParser\n */\nstruct CreateTableSelectSqlNode\n{\n  string   relation_name;\n  string   select_sql;\n};")

rpl(f, "  SCF_CREATE_TABLE,\n  SCF_CREATE_VIEW,", "  SCF_CREATE_TABLE,\n  SCF_CREATE_TABLE_SELECT,\n  SCF_CREATE_VIEW,")

rpl(f, "  CreateTableSqlNode  create_table;\n  CreateViewSqlNode   create_view;",
    "  CreateTableSqlNode       create_table;\n  CreateTableSelectSqlNode  create_table_select;\n  CreateViewSqlNode        create_view;")

print("=== parse_defs.h done ===")

# ===== 2. yacc_sql.y =====
f = base + "/src/observer/sql/parser/yacc_sql.y"

rpl(f,
    "%type <sql_node>            create_table_stmt\n%type <sql_node>            create_view_stmt",
    "%type <sql_node>            create_table_stmt\n%type <sql_node>            create_table_select_stmt\n%type <sql_node>            create_view_stmt")

rpl(f,
    "  | create_table_stmt\n  | create_view_stmt",
    "  | create_table_stmt\n  | create_table_select_stmt\n  | create_view_stmt")

rpl(f, "    ;\n\ncreate_view_stmt:",
    "    ;\n\ncreate_table_select_stmt:\n    CREATE TABLE ID AS select_stmt\n    {\n      $$ = new ParsedSqlNode(SCF_CREATE_TABLE_SELECT);\n      $$->create_table_select.relation_name = $3;\n      $$->create_table_select.select_sql = token_name(sql_string, &@5);\n      delete $5;\n    }\n    ;\n\ncreate_view_stmt:")

print("=== yacc_sql.y done ===")

# ===== 3. stmt.h =====
f = base + "/src/observer/sql/stmt/stmt.h"
rpl(f, "  DEFINE_ENUM_ITEM(CREATE_TABLE)  \\\n  DEFINE_ENUM_ITEM(CREATE_VIEW)",
    "  DEFINE_ENUM_ITEM(CREATE_TABLE)  \\\n  DEFINE_ENUM_ITEM(CREATE_TABLE_SELECT)  \\\n  DEFINE_ENUM_ITEM(CREATE_VIEW)")
print("=== stmt.h done ===")

# ===== 4. stmt.cpp =====
f = base + "/src/observer/sql/stmt/stmt.cpp"
rpl(f, '#include "sql/stmt/create_view_stmt.h"',
    '#include "sql/stmt/create_table_select_stmt.h"\n#include "sql/stmt/create_view_stmt.h"')
rpl(f,
    "    case SCF_CREATE_TABLE: {\n      return CreateTableStmt::create(db, sql_node.create_table, stmt);\n    }\n\n    case SCF_CREATE_VIEW:",
    "    case SCF_CREATE_TABLE: {\n      return CreateTableStmt::create(db, sql_node.create_table, stmt);\n    }\n\n    case SCF_CREATE_TABLE_SELECT: {\n      return CreateTableSelectStmt::create(db, sql_node.create_table_select, stmt);\n    }\n\n    case SCF_CREATE_VIEW:")
rpl(f, "  case StmtType::CREATE_TABLE:\n  case StmtType::DROP_TABLE:",
    "  case StmtType::CREATE_TABLE:\n  case StmtType::CREATE_TABLE_SELECT:\n  case StmtType::DROP_TABLE:")
print("=== stmt.cpp done ===")

# ===== 5. command_executor.cpp =====
f = base + "/src/observer/sql/executor/command_executor.cpp"
rpl(f, '#include "sql/executor/create_table_executor.h"',
    '#include "sql/executor/create_table_executor.h"\n#include "sql/executor/create_table_select_executor.h"')
rpl(f,
    "    case StmtType::CREATE_TABLE: {\n      CreateTableExecutor executor;\n      rc = executor.execute(sql_event);\n    } break;\n\n    case StmtType::CREATE_VIEW:",
    "    case StmtType::CREATE_TABLE: {\n      CreateTableExecutor executor;\n      rc = executor.execute(sql_event);\n    } break;\n\n    case StmtType::CREATE_TABLE_SELECT: {\n      CreateTableSelectExecutor executor;\n      rc = executor.execute(sql_event);\n    } break;\n\n    case StmtType::CREATE_VIEW:")
print("=== command_executor.cpp done ===")

print("\nAll parser/plumbing done!")
