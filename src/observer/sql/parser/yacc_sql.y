
%{

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/log/log.h"
#include "common/lang/string.h"
#include "common/sys/rc.h"
#include "sql/parser/parse_defs.h"
#include "sql/parser/yacc_sql.hpp"
#include "sql/parser/lex_sql.h"
#include "sql/expr/expression.h"

using namespace std;

string token_name(const char *sql_string, YYLTYPE *llocp)
{
  return string(sql_string + llocp->first_column, llocp->last_column - llocp->first_column + 1);
}

int yyerror(YYLTYPE *llocp, const char *sql_string, ParsedSqlResult *sql_result, yyscan_t scanner, const char *msg)
{
  unique_ptr<ParsedSqlNode> error_sql_node = make_unique<ParsedSqlNode>(SCF_ERROR);
  error_sql_node->error.error_msg = msg;
  error_sql_node->error.line = llocp->first_line;
  error_sql_node->error.column = llocp->first_column;
  sql_result->add_sql_node(std::move(error_sql_node));
  return 0;
}

ArithmeticExpr *create_arithmetic_expression(ArithmeticExpr::Type type,
                                             Expression *left,
                                             Expression *right,
                                             const char *sql_string,
                                             YYLTYPE *llocp)
{
  ArithmeticExpr *expr = new ArithmeticExpr(type, left, right);
  expr->set_name(token_name(sql_string, llocp));
  return expr;
}

UnboundAggregateExpr *create_aggregate_expression(const char *aggregate_name,
                                           Expression *child,
                                           const char *sql_string,
                                           YYLTYPE *llocp)
{
  UnboundAggregateExpr *expr = new UnboundAggregateExpr(aggregate_name, child);
  expr->set_name(token_name(sql_string, llocp));
  return expr;
}

Expression *create_func_expr(FunctionExpr::Type func_type,
                            vector<unique_ptr<Expression>> *args,
                            const char *sql_string,
                            YYLTYPE *llocp)
{
  vector<unique_ptr<Expression>> children;
  if (args != nullptr) {
    for (auto &a : *args) {
      children.push_back(std::move(a));
    }
    delete args;
  }
  FunctionExpr *expr = new FunctionExpr(func_type, std::move(children));
  expr->set_name(token_name(sql_string, llocp));
  return expr;
}

%}

%define api.pure full
%define parse.error verbose
/** 启用位置标识 **/
%locations
%lex-param { yyscan_t scanner }
/** 这些定义了在yyparse函数中的参数 **/
%parse-param { const char * sql_string }
%parse-param { ParsedSqlResult * sql_result }
%parse-param { void * scanner }

//标识tokens
%token  SEMICOLON
        BY
        CREATE
        DROP
        GROUP
        ORDER
        ASC
        TABLE
        TABLES
        VIEW
        INDEX
        CALC
        SELECT
        DESC
        SHOW
        SYNC
        INSERT
        DELETE
        UPDATE
        LBRACE
        RBRACE
        COMMA
        TRX_BEGIN
        TRX_COMMIT
        TRX_ROLLBACK
        INT_T
        STRING_T
        FLOAT_T
        DATE_T
        VECTOR_T
        TEXT_T
        HELP
        EXIT
        DOT //QUOTE
        INTO
        VALUES
        FROM
        WHERE
        HAVING
        AND
        SET
        ON
        LOAD
        DATA
        INFILE
        EXPLAIN
        STORAGE
        FORMAT
        PRIMARY
        KEY
        ANALYZE
        AS
        FIELDS
        TERMINATED
        ENCLOSED
        IS
        EQ
        LT
        GT
        LE
        GE
        NE
        NOT
        LIKE
        IN
        EXISTS
        INNER
        JOIN
        OR
        LENGTH
        ROUND
        DATE_FORMAT
        UNIQUE

/** union 中定义各种数据类型，真实生成的代码也是union类型，所以不能有非POD类型的数据 **/
%union {
  ParsedSqlNode *                            sql_node;
  ConditionSqlNode *                         condition;
  Value *                                    value;
  enum CompOp                                comp;
  RelAttrSqlNode *                           rel_attr;
  vector<AttrInfoSqlNode> *                  attr_infos;
  AttrInfoSqlNode *                          attr_info;
  Expression *                               expression;
  vector<unique_ptr<Expression>> *           expression_list;
  vector<Value> *                            value_list;
  vector<ConditionSqlNode> *                 condition_list;
  vector<RelAttrSqlNode> *                   rel_attr_list;
  vector<string> *                           relation_list;
  vector<string> *                           key_list;
  InnerJoinSqlNode *                         inner_join_node;
  vector<InnerJoinSqlNode> *                 inner_join_list;
  Expression *                               on_condition;
  char *                                     cstring;
  int                                        number;
  float                                      floats;
  vector<std::pair<string, Expression *>> *    update_assign_list;
  vector<vector<Value> *> *                   value_list_groups;
  OrderByParseResult *                       order_by_parse_result;
}

%destructor { if ($$) { for (auto *p : *$$) delete p; delete $$; } } <value_list_groups>
/* 须与 %union 成员名一致：update_assign_list（非终结符名叫 update_list） */
%destructor {
  if ($$) {
    for (auto &p : *$$) {
      delete p.second;
    }
    delete $$;
  }
} <update_assign_list>
%destructor { delete $$; } <condition>
%destructor { delete $$; } <value>
%destructor { delete $$; } <rel_attr>
%destructor { delete $$; } <attr_infos>
%destructor { delete $$; } <expression>
%destructor { delete $$; } <expression_list>
%destructor { delete $$; } <value_list>
%destructor { delete $$; } <condition_list>
// %destructor { delete $$; } <rel_attr_list>
%destructor { delete $$; } <key_list>
%destructor { delete $$; } <inner_join_node>
%destructor { delete $$; } <inner_join_list>
%destructor { delete $$; } <on_condition>
%destructor { delete $$; } <order_by_parse_result>

%token <number> NUMBER
%token <floats> FLOAT
%token <cstring> ID
%token <cstring> SSS
%token NULL_T
//非终结符

/** type 定义了各种解析后的结果输出的是什么类型。类型对应了 union 中的定义的成员变量名称 **/
%type <number>              type
%type <condition>           condition
%type <value>               value
%type <number>              number
%type <comp>                comp_op
%type <rel_attr>            rel_attr
%type <attr_infos>          attr_def_list
%type <attr_info>           attr_def
%type <number>              opt_nullability
%type <value_list>          value_list
%type <condition_list>      where
%type <condition_list>      condition_list
%type <expression>          where_expr
%type <expression>          condition_expr
%type <comp>                exists_op
%type <cstring>             storage_format
%type <key_list>            primary_key
%type <key_list>            attr_list
%type <update_assign_list>  update_list
%type <value_list_groups>  value_list_groups
%type <inner_join_node>     from_node
%type <inner_join_list>     from_list
%type <inner_join_node>     join_list
%type <on_condition>        on_condition
%type <expression>          expression
%type <expression>          aggregate_expression
%type <expression>          func_expr
%type <expression>          sub_query_expr
%type <expression_list>     expression_list
%type <expression_list>     select_expr_list
%type <expression>          select_expr
%type <cstring>             alias_ident
%type <expression_list>     group_by_list
%type <expression_list>     group_by
%type <expression>          having_expr
%type <order_by_parse_result> order_by_clause
%type <order_by_parse_result> order_by_list
%type <order_by_parse_result> order_by_item
%type <cstring>             fields_terminated_by
%type <cstring>             enclosed_by
%type <sql_node>            calc_stmt
%type <sql_node>            select_stmt
%type <sql_node>            insert_stmt
%type <sql_node>            update_stmt
%type <sql_node>            delete_stmt
%type <sql_node>            create_table_stmt
%type <sql_node>            create_view_stmt
%type <sql_node>            drop_table_stmt
%type <sql_node>            drop_view_stmt
%type <sql_node>            analyze_table_stmt
%type <sql_node>            show_tables_stmt
%type <sql_node>            desc_table_stmt
%type <sql_node>            create_index_stmt
%type <sql_node>            drop_index_stmt
%type <sql_node>            sync_stmt
%type <sql_node>            begin_stmt
%type <sql_node>            commit_stmt
%type <sql_node>            rollback_stmt
%type <sql_node>            load_data_stmt
%type <sql_node>            explain_stmt
%type <sql_node>            set_variable_stmt
%type <sql_node>            help_stmt
%type <sql_node>            exit_stmt
%type <sql_node>            command_wrapper
// commands should be a list but I use a single command instead
%type <sql_node>            commands

%nonassoc ID
%left OR
%left AND
%left '+' '-'
%left '*' '/'
%right UMINUS
%precedence LBRACE
%%

commands: command_wrapper opt_semicolon  //commands or sqls. parser starts here.
  {
    unique_ptr<ParsedSqlNode> sql_node = unique_ptr<ParsedSqlNode>($1);
    sql_result->add_sql_node(std::move(sql_node));
  }
  ;

command_wrapper:
    calc_stmt
  | select_stmt
  | insert_stmt
  | update_stmt
  | delete_stmt
  | create_table_stmt
  | create_view_stmt
  | drop_table_stmt
  | drop_view_stmt
  | analyze_table_stmt
  | show_tables_stmt
  | desc_table_stmt
  | create_index_stmt
  | drop_index_stmt
  | sync_stmt
  | begin_stmt
  | commit_stmt
  | rollback_stmt
  | load_data_stmt
  | explain_stmt
  | set_variable_stmt
  | help_stmt
  | exit_stmt
    ;

exit_stmt:      
    EXIT {
      (void)yynerrs;  // 这么写为了消除yynerrs未使用的告警。如果你有更好的方法欢迎提PR
      $$ = new ParsedSqlNode(SCF_EXIT);
    };

help_stmt:
    HELP {
      $$ = new ParsedSqlNode(SCF_HELP);
    };

sync_stmt:
    SYNC {
      $$ = new ParsedSqlNode(SCF_SYNC);
    }
    ;

begin_stmt:
    TRX_BEGIN  {
      $$ = new ParsedSqlNode(SCF_BEGIN);
    }
    ;

commit_stmt:
    TRX_COMMIT {
      $$ = new ParsedSqlNode(SCF_COMMIT);
    }
    ;

rollback_stmt:
    TRX_ROLLBACK  {
      $$ = new ParsedSqlNode(SCF_ROLLBACK);
    }
    ;

drop_table_stmt:    /*drop table 语句的语法解析树*/
    DROP TABLE ID {
      $$ = new ParsedSqlNode(SCF_DROP_TABLE);
      $$->drop_table.relation_name = $3;
    };

drop_view_stmt:
    DROP VIEW ID {
      $$ = new ParsedSqlNode(SCF_DROP_VIEW);
      $$->drop_view.view_name = $3;
    }
    ;

analyze_table_stmt:  /* analyze table 语法的语法解析树*/
    ANALYZE TABLE ID {
      $$ = new ParsedSqlNode(SCF_ANALYZE_TABLE);
      $$->analyze_table.relation_name = $3;
    }
    ;

show_tables_stmt:
    SHOW TABLES {
      $$ = new ParsedSqlNode(SCF_SHOW_TABLES);
    }
    ;

desc_table_stmt:
    DESC ID  {
      $$ = new ParsedSqlNode(SCF_DESC_TABLE);
      $$->desc_table.relation_name = $2;
    }
    ;

create_index_stmt:    /*create index 语句的语法解析树，支持单字段和多字段，支持 unique */
    CREATE INDEX ID ON ID LBRACE attr_list RBRACE
    {
      $$ = new ParsedSqlNode(SCF_CREATE_INDEX);
      CreateIndexSqlNode &create_index = $$->create_index;
      create_index.index_name = $3;
      create_index.relation_name = $5;
      create_index.unique = false;
      if ($7 != nullptr) {
        create_index.attribute_names.swap(*$7);
        delete $7;
      }
    }
    | CREATE UNIQUE INDEX ID ON ID LBRACE attr_list RBRACE
    {
      $$ = new ParsedSqlNode(SCF_CREATE_INDEX);
      CreateIndexSqlNode &create_index = $$->create_index;
      create_index.index_name = $4;
      create_index.relation_name = $6;
      create_index.unique = true;
      if ($8 != nullptr) {
        create_index.attribute_names.swap(*$8);
        delete $8;
      }
    }
    ;

drop_index_stmt:      /*drop index 语句的语法解析树*/
    DROP INDEX ID ON ID
    {
      $$ = new ParsedSqlNode(SCF_DROP_INDEX);
      $$->drop_index.index_name = $3;
      $$->drop_index.relation_name = $5;
    }
    ;
create_table_stmt:    /*create table 语句的语法解析树*/
    CREATE TABLE ID LBRACE attr_def_list primary_key RBRACE storage_format
    {
      $$ = new ParsedSqlNode(SCF_CREATE_TABLE);
      CreateTableSqlNode &create_table = $$->create_table;
      create_table.relation_name = $3;
      //free($3);

      create_table.attr_infos.swap(*$5);
      delete $5;

      if ($6 != nullptr) {
        create_table.primary_keys.swap(*$6);
        delete $6;
      }
      if ($8 != nullptr) {
        create_table.storage_format = $8;
      }
    }
    ;

create_view_stmt:
    CREATE VIEW ID AS select_stmt
    {
      $$ = new ParsedSqlNode(SCF_CREATE_VIEW);
      $$->create_view.view_name = $3;
      $$->create_view.select_sql = token_name(sql_string, &@5);
      $$->create_view.column_names.clear();
      delete $5;
    }
    | CREATE VIEW ID LBRACE attr_list RBRACE AS select_stmt
    {
      $$ = new ParsedSqlNode(SCF_CREATE_VIEW);
      $$->create_view.view_name = $3;
      $$->create_view.select_sql = token_name(sql_string, &@8);
      if ($5 != nullptr) {
        $$->create_view.column_names.swap(*$5);
        delete $5;
      }
      delete $8;
    }
    ;
    
attr_def_list:
    attr_def
    {
      $$ = new vector<AttrInfoSqlNode>;
      $$->emplace_back(*$1);
      delete $1;
    }
    | attr_def_list COMMA attr_def
    {
      $$ = $1;
      $$->emplace_back(*$3);
      delete $3;
    }
    ;
    
attr_def:
    ID type LBRACE number RBRACE opt_nullability
    {
      $$ = new AttrInfoSqlNode;
      $$->type = (AttrType)$2;
      $$->name = $1;
      $$->length = $4;
      $$->nullable = ($6 != 0);
    }
    | ID type opt_nullability
    {
      $$ = new AttrInfoSqlNode;
      $$->type = (AttrType)$2;
      $$->name = $1;
      $$->length = 4;
      $$->nullable = ($3 != 0);
    }
    ;

opt_nullability:
    /* empty */
    {
      $$ = 1; // default: NULL
    }
    | NULL_T
    {
      $$ = 1;
    }
    | NOT NULL_T
    {
      $$ = 0;
    }
    ;
number:
    NUMBER {$$ = $1;}
    ;
type:
    INT_T      { $$ = static_cast<int>(AttrType::INTS); }
    | STRING_T { $$ = static_cast<int>(AttrType::CHARS); }
    | FLOAT_T  { $$ = static_cast<int>(AttrType::FLOATS); }
    | DATE_T   { $$ = static_cast<int>(AttrType::DATES); }
    | VECTOR_T { $$ = static_cast<int>(AttrType::VECTORS); }
    | TEXT_T   { $$ = static_cast<int>(AttrType::TEXTS); }
    ;
primary_key:
    /* empty */
    {
      $$ = nullptr;
    }
    | COMMA PRIMARY KEY LBRACE attr_list RBRACE
    {
      $$ = $5;
    }
    ;

attr_list:
    ID {
      $$ = new vector<string>();
      $$->push_back($1);
    }
    | ID COMMA attr_list {
      if ($3 != nullptr) {
        $$ = $3;
      } else {
        $$ = new vector<string>;
      }

      $$->insert($$->begin(), $1);
    }
    ;

insert_stmt:        /*insert   语句的语法解析树*/
    INSERT INTO ID VALUES LBRACE value_list RBRACE 
    {
      $$ = new ParsedSqlNode(SCF_INSERT);
      $$->insertion.relation_name = $3;
      $$->insertion.values.swap(*$6);
      delete $6;
    }
    | INSERT INTO ID VALUES value_list_groups
    {
      $$ = new ParsedSqlNode(SCF_INSERT);
      $$->insertion.relation_name = $3;
      if ($5 != nullptr) {
        for (auto *row : *$5) {
          $$->insertion.value_rows.push_back(std::move(*row));
          delete row;
        }
        delete $5;
      }
    }
    ;

value_list_groups:
    LBRACE value_list RBRACE COMMA LBRACE value_list RBRACE
    {
      $$ = new vector<vector<Value> *>();
      $$->push_back($2);
      $$->push_back($6);
    }
    | value_list_groups COMMA LBRACE value_list RBRACE
    {
      $$ = $1;
      $$->push_back($4);
    }
    ;

value_list:
    value
    {
      $$ = new vector<Value>;
      $$->emplace_back(*$1);
      delete $1;
    }
    | value_list COMMA value { 
      $$ = $1;
      $$->emplace_back(*$3);
      delete $3;
    }
    ;
value:
    NUMBER {
      $$ = new Value((int)$1);
      @$ = @1;
    }
    | '-' NUMBER %prec UMINUS {
      $$ = new Value(-(int)$2);
      @$ = @1;
    }
    |FLOAT {
      $$ = new Value((float)$1);
      @$ = @1;
    }
    | '-' FLOAT %prec UMINUS {
      $$ = new Value(-(float)$2);
      @$ = @1;
    }
    |SSS {
      char *tmp = common::substr($1,1,strlen($1)-2);
      $$ = new Value(tmp);
      free(tmp);
    }
    | NULL_T {
      $$ = new Value();
      $$->set_null();
      @$ = @1;
    }
    ;
storage_format:
    /* empty */
    {
      $$ = nullptr;
    }
    | STORAGE FORMAT EQ ID
    {
      $$ = $4;
    }
    ;
    
delete_stmt:    /*  delete 语句的语法解析树*/
    DELETE FROM ID where 
    {
      $$ = new ParsedSqlNode(SCF_DELETE);
      $$->deletion.relation_name = $3;
      if ($4 != nullptr) {
        $$->deletion.conditions.swap(*$4);
        delete $4;
      }
    }
    ;
update_stmt:      /*  update 语句的语法解析树*/
    UPDATE ID SET update_list where 
    {
      $$ = new ParsedSqlNode(SCF_UPDATE);
      $$->update.relation_name = $2;
      if ($4 != nullptr && !$4->empty()) {
        for (auto &p : *$4) {
          $$->update.updates.emplace_back(p.first, p.second);
          p.second = nullptr;
        }
        delete $4;
      }
      if ($5 != nullptr) {
        $$->update.conditions.swap(*$5);
        delete $5;
      }
    }
    ;

update_list:
    ID EQ expression
    {
      $$ = new vector<std::pair<string, Expression *>>();
      $$->emplace_back($1, $3);
    }
    | update_list COMMA ID EQ expression
    {
      $$ = $1;
      $$->emplace_back($3, $5);
    }
    ;
select_stmt:        /*  select 语句的语法解析树*/
    SELECT select_expr_list FROM from_node from_list where_expr group_by having_expr order_by_clause
    {
      $$ = new ParsedSqlNode(SCF_SELECT);
      if ($2 != nullptr) {
        $$->selection.expressions.swap(*$2);
        delete $2;
      }

      if ($4 != nullptr) {
        $$->selection.relations.push_back(std::move(*$4));
        delete $4;
      }
      if ($5 != nullptr) {
        for (size_t i = 0; i < $5->size(); i++) {
          $$->selection.relations.push_back(std::move((*$5)[i]));
        }
        delete $5;
      }

      if ($6 != nullptr) {
        $$->selection.condition_expr = $6;
      }

      if ($7 != nullptr) {
        $$->selection.group_by.swap(*$7);
        delete $7;
      }
      if ($8 != nullptr) {
        $$->selection.having_expr = $8;
      }
      if ($9 != nullptr) {
        $$->selection.order_by_exprs.swap($9->exprs);
        $$->selection.order_by_asc.swap($9->asc);
        delete $9;
      }
    }
    | SELECT select_expr_list
    {
      $$ = new ParsedSqlNode(SCF_SELECT);
      if ($2 != nullptr) {
        $$->selection.expressions.swap(*$2);
        delete $2;
      }
    }
    ;

select_expr_list:
    select_expr
    {
      $$ = new vector<unique_ptr<Expression>>;
      $$->emplace_back(unique_ptr<Expression>($1));
    }
    | select_expr_list COMMA select_expr
    {
      if ($1 != nullptr) {
        $$ = $1;
      } else {
        $$ = new vector<unique_ptr<Expression>>;
      }
      $$->emplace_back(unique_ptr<Expression>($3));
    }
    ;

/** Non-reserved keywords usable as identifiers (alias names, etc.) */
alias_ident:
    ID
    | DATA
    | FIELDS
    | TERMINATED
    | ENCLOSED
    | LENGTH
    | ROUND
    | DATE_FORMAT
    | FORMAT
    | STORAGE
    ;

select_expr:
    expression
    {
      $$ = $1;
    }
    | '*'
    {
      $$ = new StarExpr();
    }
    | ID DOT '*'
    {
      $$ = new StarExpr($1);
    }
    | expression AS alias_ident
    {
      $1->set_name($3);
      $$ = $1;
    }
    | '*' AS alias_ident
    {
      $$ = nullptr;
      yyerror(&@$, sql_string, sql_result, scanner, "star cannot have column alias");
      YYERROR;
    }
    | ID DOT '*' AS alias_ident
    {
      $$ = nullptr;
      yyerror(&@$, sql_string, sql_result, scanner, "qualified star cannot have column alias");
      YYERROR;
    }
    ;
calc_stmt:
    CALC expression_list
    {
      $$ = new ParsedSqlNode(SCF_CALC);
      $$->calc.expressions.swap(*$2);
      delete $2;
    }
    ;

expression_list:
    expression
    {
      $$ = new vector<unique_ptr<Expression>>;
      $$->emplace_back($1);
    }
    | expression_list COMMA expression
    {
      if ($1 != nullptr) {
        $$ = $1;
      } else {
        $$ = new vector<unique_ptr<Expression>>;
      }
      $$->emplace_back($3);
    }
    ;
expression:
    expression '+' expression {
      $$ = create_arithmetic_expression(ArithmeticExpr::Type::ADD, $1, $3, sql_string, &@$);
    }
    | expression '-' expression {
      $$ = create_arithmetic_expression(ArithmeticExpr::Type::SUB, $1, $3, sql_string, &@$);
    }
    | expression '*' expression {
      $$ = create_arithmetic_expression(ArithmeticExpr::Type::MUL, $1, $3, sql_string, &@$);
    }
    | expression '/' expression {
      $$ = create_arithmetic_expression(ArithmeticExpr::Type::DIV, $1, $3, sql_string, &@$);
    }
    | LBRACE expression RBRACE {
      $$ = $2;
      $$->set_name(token_name(sql_string, &@$));
    }
    | '-' expression %prec UMINUS {
      $$ = create_arithmetic_expression(ArithmeticExpr::Type::NEGATIVE, $2, nullptr, sql_string, &@$);
    }
    | value %prec UMINUS {
      $$ = new ValueExpr(*$1);
      $$->set_name(token_name(sql_string, &@$));
      delete $1;
    }
    | rel_attr %prec UMINUS {
      RelAttrSqlNode *node = $1;
      $$ = new UnboundFieldExpr(node->relation_name, node->attribute_name);
      $$->set_name(token_name(sql_string, &@$));
      delete $1;
    }
    | aggregate_expression %prec UMINUS {
      $$ = $1;
    }
    | func_expr %prec UMINUS {
      $$ = $1;
    }
    | sub_query_expr %prec UMINUS {
      $$ = $1;
    }
    | expression alias_ident %prec ID {
      $$ = $1;
      $$->set_name($2);
    }
    ;

sub_query_expr:
    LBRACE select_stmt RBRACE
    {
      $$ = new SubQueryExpr($2->selection);
      delete $2;
    }
    ;

aggregate_expression:
    ID LBRACE expression RBRACE {
      $$ = create_aggregate_expression($1, $3, sql_string, &@$);
    }
    /* COUNT(*) 等：* 不能放回 expression（会与乘法冲突），单独在此接收 */
    | ID LBRACE '*' RBRACE {
      $$ = create_aggregate_expression($1, new StarExpr(), sql_string, &@$);
    }
    ;

func_expr:
    LENGTH LBRACE expression_list RBRACE {
      $$ = create_func_expr(FunctionExpr::Type::LENGTH, $3, sql_string, &@$);
    }
    | ROUND LBRACE expression_list RBRACE {
      $$ = create_func_expr(FunctionExpr::Type::ROUND, $3, sql_string, &@$);
    }
    | DATE_FORMAT LBRACE expression_list RBRACE {
      $$ = create_func_expr(FunctionExpr::Type::DATE_FORMAT, $3, sql_string, &@$);
    }
    ;

rel_attr:
    ID {
      $$ = new RelAttrSqlNode;
      $$->attribute_name = $1;
    }
    | ID DOT ID {
      $$ = new RelAttrSqlNode;
      $$->relation_name  = $1;
      $$->attribute_name = $3;
    }
    ;

where:
    /* empty */
    {
      $$ = nullptr;
    }
    | WHERE condition_list {
      $$ = $2;  
    }
    ;

where_expr:
    /* empty */
    {
      $$ = nullptr;
    }
    | WHERE condition_expr {
      $$ = $2;
    }
    ;

condition_expr:
    condition_expr AND condition_expr {
      vector<unique_ptr<Expression>> children;
      children.push_back(unique_ptr<Expression>($1));
      children.push_back(unique_ptr<Expression>($3));
      $$ = new ConjunctionExpr(ConjunctionExpr::Type::AND, children);
    }
    | condition_expr OR condition_expr {
      vector<unique_ptr<Expression>> children;
      children.push_back(unique_ptr<Expression>($1));
      children.push_back(unique_ptr<Expression>($3));
      $$ = new ConjunctionExpr(ConjunctionExpr::Type::OR, children);
    }
    | expression IS NULL_T {
      $$ = new IsNullExpr(unique_ptr<Expression>($1), false);
    }
    | expression IS NOT NULL_T {
      $$ = new IsNullExpr(unique_ptr<Expression>($1), true);
    }
    | expression comp_op expression {
      $$ = new ComparisonExpr($2, unique_ptr<Expression>($1), unique_ptr<Expression>($3));
    }
    | expression IN LBRACE select_stmt RBRACE %prec LBRACE {
      $$ = new ComparisonExpr(IN_OP, unique_ptr<Expression>($1), unique_ptr<Expression>(new SubQueryExpr($4->selection)));
      delete $4;
    }
    | expression NOT IN LBRACE select_stmt RBRACE %prec LBRACE {
      $$ = new ComparisonExpr(NOT_IN_OP, unique_ptr<Expression>($1), unique_ptr<Expression>(new SubQueryExpr($5->selection)));
      delete $5;
    }
    | expression IN LBRACE expression_list RBRACE %prec LBRACE {
      $$ = new ComparisonExpr(IN_OP, unique_ptr<Expression>($1), unique_ptr<Expression>(new ValueListExpr(std::move(*$4))));
      delete $4;
    }
    | expression NOT IN LBRACE expression_list RBRACE %prec LBRACE {
      $$ = new ComparisonExpr(NOT_IN_OP, unique_ptr<Expression>($1), unique_ptr<Expression>(new ValueListExpr(std::move(*$5))));
      delete $5;
    }
    | exists_op LBRACE select_stmt RBRACE {
      Value v;
      v.set_type(AttrType::UNDEFINED);
      $$ = new ComparisonExpr($1, unique_ptr<Expression>(new ValueExpr(v)), unique_ptr<Expression>(new SubQueryExpr($3->selection)));
      delete $3;
    }
    | LBRACE condition_expr RBRACE {
      $$ = $2;
    }
    ;

condition_list:
    /* empty */
    {
      $$ = nullptr;
    }
    | condition {
      $$ = new vector<ConditionSqlNode>;
      $$->emplace_back(*$1);
      delete $1;
    }
    | condition AND condition_list {
      $$ = $3;
      $$->emplace_back(*$1);
      delete $1;
    }
    ;

from_list:
    /* empty */
    {
      $$ = nullptr;
    }
    | COMMA from_node from_list {
      if (nullptr != $3) {
        $$ = $3;
      } else {
        $$ = new vector<InnerJoinSqlNode>;
      }
      /* 右递归 from_list 原先末尾追加导致逗号表顺序与书写顺序相反；插到开头以保持与 FROM 从左到右一致 */
      $$->insert($$->begin(), std::move(*$2));
      delete $2;
    }
    ;

from_node:
    ID ID join_list {
      if (nullptr != $3) {
        $$ = $3;
      } else {
        $$ = new InnerJoinSqlNode;
      }
      $$->base_relation.first = $1;
      $$->base_relation.second = $2;
      std::reverse($$->join_relations.begin(), $$->join_relations.end());
      std::reverse($$->conditions.begin(), $$->conditions.end());
    }
    | ID AS ID join_list {
      if (nullptr != $4) {
        $$ = $4;
      } else {
        $$ = new InnerJoinSqlNode;
      }
      $$->base_relation.first = $1;
      $$->base_relation.second = $3;
      std::reverse($$->join_relations.begin(), $$->join_relations.end());
      std::reverse($$->conditions.begin(), $$->conditions.end());
    }
    | ID join_list {
      if (nullptr != $2) {
        $$ = $2;
      } else {
        $$ = new InnerJoinSqlNode;
      }
      $$->base_relation.first = $1;
      $$->base_relation.second = "";
      std::reverse($$->join_relations.begin(), $$->join_relations.end());
      std::reverse($$->conditions.begin(), $$->conditions.end());
    }
    ;

join_list:
    /* empty */
    {
      $$ = nullptr;
    }
    | INNER JOIN ID ID ON on_condition join_list {
      if (nullptr != $7) {
        $$ = $7;
      } else {
        $$ = new InnerJoinSqlNode;
      }
      $$->join_relations.emplace_back($3, $4);
      $$->conditions.push_back($6);
    }
    | INNER JOIN ID AS ID ON on_condition join_list {
      if (nullptr != $8) {
        $$ = $8;
      } else {
        $$ = new InnerJoinSqlNode;
      }
      $$->join_relations.emplace_back($3, $5);
      $$->conditions.push_back($7);
    }
    | INNER JOIN ID ON on_condition join_list {
      if (nullptr != $6) {
        $$ = $6;
      } else {
        $$ = new InnerJoinSqlNode;
      }
      $$->join_relations.emplace_back($3, "");
      $$->conditions.push_back($5);
    }
    ;

on_condition:
    expression comp_op expression {
      $$ = new ComparisonExpr($2, unique_ptr<Expression>($1), unique_ptr<Expression>($3));
    }
    | on_condition AND on_condition {
      vector<unique_ptr<Expression>> children;
      children.push_back(unique_ptr<Expression>($1));
      children.push_back(unique_ptr<Expression>($3));
      $$ = new ConjunctionExpr(ConjunctionExpr::Type::AND, children);
    }
    | on_condition OR on_condition {
      vector<unique_ptr<Expression>> children;
      children.push_back(unique_ptr<Expression>($1));
      children.push_back(unique_ptr<Expression>($3));
      $$ = new ConjunctionExpr(ConjunctionExpr::Type::OR, children);
    }
    | LBRACE on_condition RBRACE {
      $$ = $2;
    }
    ;

condition:
    rel_attr comp_op value
    {
      $$ = new ConditionSqlNode;
      $$->left_is_attr = 1;
      $$->left_attr = *$1;
      $$->right_is_attr = 0;
      $$->right_value = *$3;
      $$->comp = $2;

      delete $1;
      delete $3;
    }
    | value comp_op value 
    {
      $$ = new ConditionSqlNode;
      $$->left_is_attr = 0;
      $$->left_value = *$1;
      $$->right_is_attr = 0;
      $$->right_value = *$3;
      $$->comp = $2;

      delete $1;
      delete $3;
    }
    | rel_attr comp_op rel_attr
    {
      $$ = new ConditionSqlNode;
      $$->left_is_attr = 1;
      $$->left_attr = *$1;
      $$->right_is_attr = 1;
      $$->right_attr = *$3;
      $$->comp = $2;

      delete $1;
      delete $3;
    }
    | value comp_op rel_attr
    {
      $$ = new ConditionSqlNode;
      $$->left_is_attr = 0;
      $$->left_value = *$1;
      $$->right_is_attr = 1;
      $$->right_attr = *$3;
      $$->comp = $2;

      delete $1;
      delete $3;
    }
    | rel_attr IS NULL_T
    {
      $$ = new ConditionSqlNode;
      $$->left_is_attr = 1;
      $$->left_attr = *$1;
      $$->comp = IS_NULL_OP;
      $$->right_is_attr = 0;
      $$->right_value.set_null();
      delete $1;
    }
    | rel_attr IS NOT NULL_T
    {
      $$ = new ConditionSqlNode;
      $$->left_is_attr = 1;
      $$->left_attr = *$1;
      $$->comp = IS_NOT_NULL_OP;
      $$->right_is_attr = 0;
      $$->right_value.set_null();
      delete $1;
    }
    ;

comp_op:
      EQ { $$ = EQUAL_TO; }
    | LT { $$ = LESS_THAN; }
    | GT { $$ = GREAT_THAN; }
    | LE { $$ = LESS_EQUAL; }
    | GE { $$ = GREAT_EQUAL; }
    | NE { $$ = NOT_EQUAL; }
    | LIKE { $$ = LIKE_OP; }
    | NOT LIKE { $$ = NOT_LIKE_OP; }
    | IN { $$ = IN_OP; }
    | NOT IN { $$ = NOT_IN_OP; }
    ;

exists_op:
    EXISTS { $$ = EXISTS_OP; }
    | NOT EXISTS { $$ = NOT_EXISTS_OP; }
    ;

// your code here
group_by_list:
    expression
    {
      $$ = new vector<unique_ptr<Expression>>;
      $$->emplace_back($1);
    }
    | group_by_list COMMA expression
    {
      if ($1 != nullptr) {
        $$ = $1;
      } else {
        $$ = new vector<unique_ptr<Expression>>;
      }
      $$->emplace_back($3);
    }
    ;

group_by:
    /* empty */
    {
      $$ = nullptr;
    }
    | GROUP BY group_by_list
    {
      // group by 的表达式范围与select查询值的表达式范围是不同的，比如group by不支持 *
      // 但是这里没有处理。
      $$ = $3;
    }
    ;

having_expr:
    /* empty */
    {
      $$ = nullptr;
    }
    | HAVING condition_expr
    {
      $$ = $2;
    }
    ;

order_by_item:
    expression
    {
      $$ = new OrderByParseResult;
      $$->exprs.emplace_back(unique_ptr<Expression>($1));
      $$->asc.push_back(true);
    }
    | expression ASC
    {
      $$ = new OrderByParseResult;
      $$->exprs.emplace_back(unique_ptr<Expression>($1));
      $$->asc.push_back(true);
    }
    | expression DESC
    {
      $$ = new OrderByParseResult;
      $$->exprs.emplace_back(unique_ptr<Expression>($1));
      $$->asc.push_back(false);
    }
    ;

order_by_list:
    order_by_item
    {
      $$ = $1;
    }
    | order_by_list COMMA order_by_item
    {
      $$ = $1;
      $$->exprs.push_back(std::move($3->exprs[0]));
      $$->asc.push_back($3->asc[0]);
      delete $3;
    }
    ;

order_by_clause:
    /* empty */
    {
      $$ = nullptr;
    }
    | ORDER BY order_by_list
    {
      $$ = $3;
    }
    ;

load_data_stmt:
    LOAD DATA INFILE SSS INTO TABLE ID fields_terminated_by enclosed_by
    {
      char *tmp_file_name = common::substr($4, 1, strlen($4) - 2);
      
      $$ = new ParsedSqlNode(SCF_LOAD_DATA);
      $$->load_data.relation_name = $7;
      $$->load_data.file_name = tmp_file_name;
      if ($8 != nullptr) {
        char *tmp = common::substr($8,1,strlen($8)-2);
        $$->load_data.terminated = $8;
        free(tmp);
      }
      if ($9 != nullptr) {
        char *tmp = common::substr($9,1,strlen($9)-2);
        $$->load_data.enclosed = $9;
        free(tmp);
      }
      free(tmp_file_name);
    }
    ;

fields_terminated_by:
    /* empty */
    {
      $$ = nullptr;
    }
    | FIELDS TERMINATED BY SSS
    {
      $$ = $4;
    };

enclosed_by:
    /* empty */
    {
      $$ = nullptr;
    }
    | ENCLOSED BY SSS
    {
      $$ = $3;
    };

explain_stmt:
    EXPLAIN command_wrapper
    {
      $$ = new ParsedSqlNode(SCF_EXPLAIN);
      $$->explain.sql_node = unique_ptr<ParsedSqlNode>($2);
    }
    ;

set_variable_stmt:
    SET ID EQ value
    {
      $$ = new ParsedSqlNode(SCF_SET_VARIABLE);
      $$->set_variable.name  = $2;
      $$->set_variable.value = *$4;
      delete $4;
    }
    ;

opt_semicolon: /*empty*/
    | SEMICOLON
    ;
%%
//_____________________________________________________________________
extern void scan_string(const char *str, yyscan_t scanner);

int sql_parse(const char *s, ParsedSqlResult *sql_result) {
  yyscan_t scanner;
  std::vector<char *> allocated_strings;
  yylex_init_extra(static_cast<void*>(&allocated_strings),&scanner);
  scan_string(s, scanner);
  int result = yyparse(s, sql_result, scanner);

  for (char *ptr : allocated_strings) {
    free(ptr);
  }
  allocated_strings.clear();

  yylex_destroy(scanner);
  return result;
}
