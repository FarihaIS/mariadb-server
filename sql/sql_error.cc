/* Copyright (c) 1995, 2011, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1335  USA */

/**********************************************************************
This file contains the implementation of error and warnings related

  - Whenever an error or warning occurred, it pushes it to a warning list
    that the user can retrieve with SHOW WARNINGS or SHOW ERRORS.

  - For each statement, we return the number of warnings generated from this
    command.  Note that this can be different from @@warning_count as
    we reset the warning list only for questions that uses a table.
    This is done to allow on to do:
    INSERT ...;
    SELECT @@warning_count;
    SHOW WARNINGS;
    (If we would reset after each command, we could not retrieve the number
     of warnings)

  - When client requests the information using SHOW command, then 
    server processes from this list and returns back in the form of 
    resultset.

    Supported syntaxes:

    SHOW [COUNT(*)] ERRORS [LIMIT [offset,] rows]
    SHOW [COUNT(*)] WARNINGS [LIMIT [offset,] rows]
    SELECT @@warning_count, @@error_count;

***********************************************************************/

#include "mariadb.h"
#include "sql_priv.h"
#include "unireg.h"
#include "sql_error.h"
#include "sp_rcontext.h"

/*
  Design notes about Sql_condition::m_message_text.

  The member Sql_condition::m_message_text contains the text associated with
  an error, warning or note (which are all SQL 'conditions')

  Producer of Sql_condition::m_message_text:
  ----------------------------------------

  (#1) the server implementation itself, when invoking functions like
  my_error() or push_warning()

  (#2) user code in stored programs, when using the SIGNAL statement.

  (#3) user code in stored programs, when using the RESIGNAL statement.

  When invoking my_error(), the error number and message is typically
  provided like this:
  - my_error(ER_WRONG_DB_NAME, MYF(0), ...);
  - my_message(ER_SLAVE_IGNORED_TABLE, ER(ER_SLAVE_IGNORED_TABLE), MYF(0));

  In both cases, the message is retrieved from ER(ER_XXX), which in turn
  is read from the resource file errmsg.sys at server startup.
  The strings stored in the errmsg.sys file are expressed in the character set
  that corresponds to the server --language start option
  (see error_message_charset_info).

  When executing:
  - a SIGNAL statement,
  - a RESIGNAL statement,
  the message text is provided by the user logic, and is expressed in UTF8.

  Storage of Sql_condition::m_message_text:
  ---------------------------------------

  (#4) The class Sql_condition is used to hold the message text member.
  This class represents a single SQL condition.

  (#5) The class Warning_info represents a SQL condition area, and contains
  a collection of SQL conditions in the Warning_info::m_warn_list

  Consumer of Sql_condition::m_message_text:
  ----------------------------------------

  (#6) The statements SHOW WARNINGS and SHOW ERRORS display the content of
  the warning list.

  (#7) The GET DIAGNOSTICS statement (planned, not implemented yet) will
  also read the content of:
  - the top level statement condition area (when executed in a query),
  - a sub statement (when executed in a stored program)
  and return the data stored in a Sql_condition.

  (#8) The RESIGNAL statement reads the Sql_condition caught by an exception
  handler, to raise a new or modified condition (in #3).

  The big picture
  ---------------
                                                              --------------
                                                              |            ^
                                                              V            |
  my_error(#1)                 SIGNAL(#2)                 RESIGNAL(#3)     |
      |(#A)                       |(#B)                       |(#C)        |
      |                           |                           |            |
      ----------------------------|----------------------------            |
                                  |                                        |
                                  V                                        |
                           Sql_condition(#4)                                 |
                                  |                                        |
                                  |                                        |
                                  V                                        |
                           Warning_info(#5)                                |
                                  |                                        |
          -----------------------------------------------------            |
          |                       |                           |            |
          |                       |                           |            |
          |                       |                           |            |
          V                       V                           V            |
   SHOW WARNINGS(#6)      GET DIAGNOSTICS(#7)              RESIGNAL(#8)    |
          |  |                    |                           |            |
          |  --------             |                           V            |
          |         |             |                           --------------
          V         |             |
      Connectors    |             |
          |         |             |
          -------------------------
                    |
                    V
             Client application

  Current implementation status
  -----------------------------

  (#1) (my_error) produces data in the 'error_message_charset_info' CHARSET

  (#2) and (#3) (SIGNAL, RESIGNAL) produces data internally in UTF8

  (#6) (SHOW WARNINGS) produces data in the 'error_message_charset_info' CHARSET

  (#7) (GET DIAGNOSTICS) is not implemented.

  (#8) (RESIGNAL) produces data internally in UTF8 (see #3)

  As a result, the design choice for (#4) and (#5) is to store data in
  the 'error_message_charset_info' CHARSET, to minimize impact on the code base.
  This is implemented by using 'String Sql_condition::m_message_text'.

  The UTF8 -> error_message_charset_info conversion is implemented in
  Sql_cmd_common_signal::eval_signal_informations() (for path #B and #C).

  Future work
  -----------

  - Change (#1) (my_error) to generate errors in UTF8.
    See WL#751 (Recoding of error messages)

  - Change (#4 and #5) to store message text in UTF8 natively.
    In practice, this means changing the type of the message text to
    '<UTF8 String 128 class> Sql_condition::m_message_text', and is a direct
    consequence of WL#751.

  - Implement (#9) (GET DIAGNOSTICS).
    See WL#2111 (Stored Procedures: Implement GET DIAGNOSTICS)
*/


static void copy_string(MEM_ROOT *mem_root, String* dst, const String* src)
{
  size_t len= src->length();
  if (len)
  {
    char* copy= (char*) alloc_root(mem_root, len + 1);
    if (copy)
    {
      memcpy(copy, src->ptr(), len);
      copy[len]= '\0';
      dst->set(copy, len, src->charset());
    }
  }
  else
    dst->length(0);
}

void
Sql_condition::copy_opt_attributes(const Sql_condition *cond)
{
  DBUG_ASSERT(this != cond);
  copy_string(m_mem_root, & m_class_origin, & cond->m_class_origin);
  copy_string(m_mem_root, & m_subclass_origin, & cond->m_subclass_origin);
  copy_string(m_mem_root, & m_constraint_catalog, & cond->m_constraint_catalog);
  copy_string(m_mem_root, & m_constraint_schema, & cond->m_constraint_schema);
  copy_string(m_mem_root, & m_constraint_name, & cond->m_constraint_name);
  copy_string(m_mem_root, & m_catalog_name, & cond->m_catalog_name);
  copy_string(m_mem_root, & m_schema_name, & cond->m_schema_name);
  copy_string(m_mem_root, & m_table_name, & cond->m_table_name);
  copy_string(m_mem_root, & m_column_name, & cond->m_column_name);
  copy_string(m_mem_root, & m_cursor_name, & cond->m_cursor_name);
  m_row_number= cond->m_row_number;
}


void
Sql_condition::set_builtin_message_text(const char* str)
{
  /*
    See the comments
     "Design notes about Sql_condition::m_message_text."
  */
  const char* copy;

  copy= m_mem_root ? strdup_root(m_mem_root, str) : str;
  m_message_text.set(copy, strlen(copy), error_message_charset_info);
  DBUG_ASSERT(! m_message_text.is_alloced());
}

const char*
Sql_condition::get_message_text() const
{
  return m_message_text.ptr();
}

int
Sql_condition::get_message_octet_length() const
{
  return m_message_text.length();
}


void Sql_state_errno_level::assign_defaults(THD *thd,
                                            const Sql_state_errno *from)
{
  DBUG_ASSERT(from);
  int sqlerrno= from->get_sql_errno();
  /*
    SIGNAL is restricted in sql_yacc.yy to only signal SQLSTATE conditions.
  */
  DBUG_ASSERT(from->has_sql_state());
  set_sqlstate(from);
  /* SQLSTATE class "00": illegal, rejected in the parser. */
  DBUG_ASSERT(m_sqlstate[0] != '0' || get_sqlstate()[1] != '0');

  if (Sql_state::is_warning())        /* SQLSTATE class "01": warning. */
  {
    m_level= Sql_condition::WARN_LEVEL_WARN;
    m_sql_errno= sqlerrno ? sqlerrno : ER_SIGNAL_WARN;
  }
  else if (Sql_state::is_not_found()) /* SQLSTATE class "02": not found. */
  {
    m_level= Sql_condition::WARN_LEVEL_ERROR;
    if (sqlerrno)
      m_sql_errno= sqlerrno;
    else
    {
      if ((thd->in_sub_stmt & (SUB_STMT_TRIGGER | SUB_STMT_BEFORE_TRIGGER)) ==
          (SUB_STMT_TRIGGER | SUB_STMT_BEFORE_TRIGGER) &&
          strcmp(get_sqlstate(), "02TRG") == 0)
        m_sql_errno= ER_SIGNAL_SKIP_ROW_FROM_TRIGGER;
      else
        m_sql_errno= ER_SIGNAL_NOT_FOUND;
    }
  }
  else                               /* other SQLSTATE classes : error. */
  {
    m_level= Sql_condition::WARN_LEVEL_ERROR;
    m_sql_errno= sqlerrno ? sqlerrno : ER_SIGNAL_EXCEPTION;
  }
}


void Sql_condition::assign_defaults(THD *thd, const Sql_state_errno *from)
{
  if (from)
    Sql_state_errno_level::assign_defaults(thd, from);
  if (!get_message_text())
    set_builtin_message_text(ER(get_sql_errno()));
}


Diagnostics_area::Diagnostics_area(bool initialize)
  : is_bulk_execution(0), m_main_wi(0, false, initialize)
{
  push_warning_info(&m_main_wi);

  reset_diagnostics_area();
}

Diagnostics_area::Diagnostics_area(ulonglong warning_info_id,
                                   bool allow_unlimited_warnings,
                                   bool initialize)
  : is_bulk_execution(0),
  m_main_wi(warning_info_id, allow_unlimited_warnings, initialize)
{
  push_warning_info(&m_main_wi);

  reset_diagnostics_area();
}

/**
  Clear this diagnostics area.

  Normally called at the end of a statement.
*/

void
Diagnostics_area::reset_diagnostics_area()
{
  DBUG_ENTER("reset_diagnostics_area");
#ifdef DBUG_OFF
  m_can_overwrite_status= FALSE;
  /** Don't take chances in production */
  m_message[0]= '\0';
  Sql_state_errno::clear();
  Sql_user_condition_identity::clear();
  m_last_insert_id= 0;
  if (!is_bulk_op())
  {
    m_affected_rows= 0;
    m_statement_warn_count= 0;
  }
#endif
  get_warning_info()->clear_error_condition();
  set_is_sent(false);
  /*
    For BULK DML operations (e.g. UPDATE) the data member m_status
    has the value DA_OK_BULK. Keep this value in order to handle
    m_affected_rows, m_statement_warn_count in correct way. Else,
    the number of rows and the number of warnings affected by
    the last statement executed as part of a trigger fired by the dml
    (e.g. UPDATE statement fires a trigger on AFTER UPDATE) would counts
    rows modified by trigger's statement.
  */
  m_status= is_bulk_op() ? DA_OK_BULK : DA_EMPTY;
  DBUG_VOID_RETURN;
}


/**
  Set OK status -- ends commands that do not return a
  result set, e.g. INSERT/UPDATE/DELETE.
*/

void
Diagnostics_area::set_ok_status(ulonglong affected_rows,
                                ulonglong last_insert_id,
                                const char *message)
{
  DBUG_ENTER("set_ok_status");
  DBUG_ASSERT(!is_set() || (m_status == DA_OK_BULK && is_bulk_op()));
  /*
    In production, refuse to overwrite an error or a custom response
    with an OK packet.
  */
  if (unlikely(is_error() || is_disabled()))
    return;
  /*
    When running a bulk operation, m_status will be DA_OK for the first
    operation and set to DA_OK_BULK for all following operations.
  */
  if (m_status == DA_OK_BULK)
  {
    m_statement_warn_count+= current_statement_warn_count();
    m_affected_rows+= affected_rows;
  }
  else
  {
    m_statement_warn_count= current_statement_warn_count();
    m_affected_rows= affected_rows;
    m_status= (is_bulk_op() ? DA_OK_BULK : DA_OK);
  }
  m_last_insert_id= last_insert_id;
  if (message)
    strmake_buf(m_message, message);
  else
    m_message[0]= '\0';
  DBUG_VOID_RETURN;
}


/**
  Set EOF status.
*/

void
Diagnostics_area::set_eof_status(THD *thd)
{
  DBUG_ENTER("set_eof_status");
  /* Only allowed to report eof if has not yet reported an error */
  DBUG_ASSERT(!is_set() || (m_status == DA_EOF_BULK && is_bulk_op()));
  /*
    In production, refuse to overwrite an error or a custom response
    with an EOF packet.
  */
  if (unlikely(is_error() || is_disabled()))
    return;

  /*
    If inside a stored procedure, do not return the total
    number of warnings, since they are not available to the client
    anyway.
  */
  if (m_status == DA_EOF_BULK)
  {
    if (!thd->spcont)
      m_statement_warn_count+= current_statement_warn_count();
  }
  else
  {
    if (thd->spcont)
    {
      m_statement_warn_count= 0;
      m_affected_rows= 0;
    }
    else
      m_statement_warn_count= current_statement_warn_count();
    m_status= (is_bulk_op() ? DA_EOF_BULK : DA_EOF);
  }

  DBUG_VOID_RETURN;
}

/**
  Set ERROR status in the Diagnostics Area. This function should be used to
  report fatal errors (such as out-of-memory errors) when no further
  processing is possible.

  @param sql_errno        SQL-condition error number
*/

void
Diagnostics_area::set_error_status(uint sql_errno)
{
  set_error_status(sql_errno,
                   ER(sql_errno),
                   mysql_errno_to_sqlstate(sql_errno),
                   Sql_user_condition_identity(),
                   NULL);
}


/**
  Set ERROR status in the Diagnostics Area.

  @note error_condition may be NULL. It happens if a) OOM error is being
  reported; or b) when Warning_info is full.

  @param sql_errno        SQL-condition error number
  @param message          SQL-condition message
  @param sqlstate         SQL-condition state
  @param ucid             User defined condition identity
  @param error_condition  SQL-condition object representing the error state

  @note Note, that error_condition may be NULL. It happens if a) OOM error is
  being reported; or b) when Warning_info is full.
*/

void
Diagnostics_area::set_error_status(uint sql_errno,
                                   const char *message,
                                   const char *sqlstate,
                                   const Sql_user_condition_identity &ucid,
                                   const Sql_condition *error_condition)
{
  DBUG_ENTER("set_error_status");
  DBUG_PRINT("enter", ("error: %d", sql_errno));
  /*
    Only allowed to report error if has not yet reported a success
    The only exception is when we flush the message to the client,
    an error can happen during the flush.
  */
  DBUG_ASSERT(! is_set() || m_can_overwrite_status);

  // message must be set properly by the caller.
  DBUG_ASSERT(message);

  // sqlstate must be set properly by the caller.
  DBUG_ASSERT(sqlstate);

#ifdef DBUG_OFF
  /*
    In production, refuse to overwrite a custom response with an
    ERROR packet.
  */
  if (is_disabled())
    return;
#endif

  Sql_state_errno::set(sql_errno, sqlstate);
  Sql_user_condition_identity::set(ucid);
  strmake_buf(m_message, message);

  get_warning_info()->set_error_condition(error_condition);

  m_status= DA_ERROR;
  DBUG_VOID_RETURN;
}

/**
  Mark the diagnostics area as 'DISABLED'.

  This is used in rare cases when the COM_ command at hand sends a response
  in a custom format. One example is the query cache, another is
  COM_STMT_PREPARE.
*/

void
Diagnostics_area::disable_status()
{
  DBUG_ENTER("disable_status");
  DBUG_ASSERT(! is_set());
  m_status= DA_DISABLED;
  DBUG_VOID_RETURN;
}

Warning_info::Warning_info(ulonglong warn_id_arg,
                           bool allow_unlimited_warnings, bool initialize)
  :m_current_statement_warn_count(0),
  m_current_row_for_warning(0),
  m_warn_id(warn_id_arg),
  m_error_condition(NULL),
  m_allow_unlimited_warnings(allow_unlimited_warnings),
  initialized(0),
  m_read_only(FALSE)
{
  m_warn_list.empty();
  memset(m_warn_count, 0, sizeof(m_warn_count));
  if (initialize)
    init();
}

void Warning_info::init()
{
  /* Initialize sub structures */
  DBUG_ASSERT(initialized == 0);
  init_sql_alloc(PSI_INSTRUMENT_ME, &m_warn_root, WARN_ALLOC_BLOCK_SIZE,
                 WARN_ALLOC_PREALLOC_SIZE, MYF(MY_THREAD_SPECIFIC));
  initialized= 1;
}

void Warning_info::free_memory()
{
  if (initialized)
    free_root(&m_warn_root,MYF(0));
}

Warning_info::~Warning_info()
{
  free_memory();
}


bool Warning_info::has_sql_condition(const char *message_str, size_t message_length) const
{
  Diagnostics_area::Sql_condition_iterator it(m_warn_list);
  const Sql_condition *err;

  while ((err= it++))
  {
    if (strncmp(message_str, err->get_message_text(), message_length) == 0)
      return true;
  }

  return false;
}

bool Warning_info::has_sql_condition(uint sql_errno) const
{
  Diagnostics_area::Sql_condition_iterator it(m_warn_list);
  const Sql_condition *err;

  while ((err = it++))
  {
    if (err->get_sql_errno() == sql_errno)
      return true;
  }
  return false;
}

void Warning_info::clear(ulonglong new_id)
{
  id(new_id);
  m_warn_list.empty();
  m_marked_sql_conditions.empty();
  free_memory();
  memset(m_warn_count, 0, sizeof(m_warn_count));
  m_current_statement_warn_count= 0;
  m_current_row_for_warning= 0;
  clear_error_condition();
}

void Warning_info::append_warning_info(THD *thd, const Warning_info *source)
{
  const Sql_condition *err;
  Diagnostics_area::Sql_condition_iterator it(source->m_warn_list);
  const Sql_condition *src_error_condition = source->get_error_condition();

  while ((err= it++))
  {
    // Do not use ::push_warning() to avoid invocation of THD-internal-handlers.
    Sql_condition *new_error= Warning_info::push_warning(thd, err);

    if (src_error_condition && src_error_condition == err)
      set_error_condition(new_error);

    if (source->is_marked_for_removal(err))
      mark_condition_for_removal(new_error);
  }
}


/**
  Copy Sql_conditions that are not WARN_LEVEL_ERROR from the source
  Warning_info to the current Warning_info.

  @param thd    Thread context.
  @param sp_wi  Stored-program Warning_info
  @param thd     Thread context.
  @param src_wi  Warning_info to copy from.
*/
void Diagnostics_area::copy_non_errors_from_wi(THD *thd,
                                               const Warning_info *src_wi)
{
  Sql_condition_iterator it(src_wi->m_warn_list);
  const Sql_condition *cond;
  Warning_info *wi= get_warning_info();

  while ((cond= it++))
  {
    if (cond->get_level() == Sql_condition::WARN_LEVEL_ERROR)
      continue;

    Sql_condition *new_condition= wi->push_warning(thd, cond);

    if (src_wi->is_marked_for_removal(cond))
      wi->mark_condition_for_removal(new_condition);
  }
}


void Warning_info::mark_sql_conditions_for_removal()
{
  Sql_condition_list::Iterator it(m_warn_list);
  Sql_condition *cond;

  while ((cond= it++))
    mark_condition_for_removal(cond);
}


void Warning_info::remove_marked_sql_conditions()
{
  List_iterator_fast<Sql_condition> it(m_marked_sql_conditions);
  Sql_condition *cond;

  while ((cond= it++))
  {
    m_warn_list.remove(cond);
    m_warn_count[cond->get_level()]--;
    m_current_statement_warn_count--;
    if (cond == m_error_condition)
      m_error_condition= NULL;
  }

  m_marked_sql_conditions.empty();
}


bool Warning_info::is_marked_for_removal(const Sql_condition *cond) const
{
  List_iterator_fast<Sql_condition> it(
    const_cast<List<Sql_condition>&> (m_marked_sql_conditions));
  Sql_condition *c;

  while ((c= it++))
  {
    if (c == cond)
      return true;
  }

  return false;
}


void Warning_info::reserve_space(THD *thd, uint count)
{
  while (m_warn_list.elements() &&
         (m_warn_list.elements() + count) > thd->variables.max_error_count)
    m_warn_list.remove(m_warn_list.front());
}

Sql_condition *Warning_info::push_warning(THD *thd,
                                          const Sql_condition_identity *value,
                                          const char *msg,
                                          ulong current_row_number)
{
  Sql_condition *cond= NULL;

  if (! m_read_only)
  {
    if (m_allow_unlimited_warnings ||
        m_warn_list.elements() < thd->variables.max_error_count)
    {
      cond= new (& m_warn_root) Sql_condition(& m_warn_root, *value, msg,
                                              current_row_number);
      if (cond)
        m_warn_list.push_back(cond);
    }
    m_warn_count[(uint) value->get_level()]++;
  }

  m_current_statement_warn_count++;
  return cond;
}


Sql_condition *Warning_info::push_warning(THD *thd,
                                          const Sql_condition *sql_condition)
{
  Sql_condition *new_condition= push_warning(thd, sql_condition,
                                             sql_condition->get_message_text(),
                                             sql_condition->m_row_number);

  if (new_condition)
    new_condition->copy_opt_attributes(sql_condition);

  return new_condition;
}

/*
  Push the warning to error list if there is still room in the list

  SYNOPSIS
    push_warning()
    thd			Thread handle
    level		Severity of warning (note, warning)
    code		Error number
    msg			Clear error message
*/

void push_warning(THD *thd, Sql_condition::enum_warning_level level,
                  uint code, const char *msg)
{
  DBUG_ENTER("push_warning");
  DBUG_PRINT("enter", ("code: %d, msg: %s", code, msg));

  /*
    Calling push_warning/push_warning_printf with a level of
    WARN_LEVEL_ERROR *is* a bug.  Either use my_printf_error(),
    my_error(), or WARN_LEVEL_WARN.
  */
  DBUG_ASSERT(level != Sql_condition::WARN_LEVEL_ERROR);

  if (level == Sql_condition::WARN_LEVEL_ERROR)
    level= Sql_condition::WARN_LEVEL_WARN;

  DBUG_ASSERT(msg[strlen(msg)-1] != '\n');
  (void) thd->raise_condition(code, "\0\0\0\0\0", level, msg);

  /* Make sure we also count warnings pushed after calling set_ok_status(). */
  thd->get_stmt_da()->increment_warning();

  DBUG_VOID_RETURN;
}


/*
  Push the warning to error list if there is still room in the list

  SYNOPSIS
    push_warning_printf()
    thd			Thread handle
    level		Severity of warning (note, warning)
    code		Error number
    msg			Clear error message
*/

void push_warning_printf(THD *thd, Sql_condition::enum_warning_level level,
			 uint code, const char *format, ...)
{
  va_list args;
  char    warning[MYSQL_ERRMSG_SIZE];
  DBUG_ENTER("push_warning_printf");
  DBUG_PRINT("enter",("warning: %u", code));

  DBUG_ASSERT(code != 0);
  DBUG_ASSERT(format != NULL);

  va_start(args,format);
  my_vsnprintf_ex(&my_charset_utf8mb3_general_ci, warning,
                  sizeof(warning), format, args);
  va_end(args);
  push_warning(thd, level, code, warning);
  DBUG_VOID_RETURN;
}


/*
  Send all notes, errors or warnings to the client in a result set

  SYNOPSIS
    mysqld_show_warnings()
    thd			Thread handler
    levels_to_show	Bitmap for which levels to show

  DESCRIPTION
    Takes into account the current LIMIT

  RETURN VALUES
    FALSE ok
    TRUE  Error sending data to client
*/

const LEX_CSTRING warning_level_names[]=
{
  { STRING_WITH_LEN("Note") },
  { STRING_WITH_LEN("Warning") },
  { STRING_WITH_LEN("Error") },
  { STRING_WITH_LEN("?") }
};

bool mysqld_show_warnings(THD *thd, ulong levels_to_show)
{
  List<Item> field_list;
  MEM_ROOT *mem_root= thd->mem_root;
  const Sql_condition *err;
  SELECT_LEX *sel= thd->lex->first_select_lex();
  SELECT_LEX_UNIT *unit= &thd->lex->unit;
  ha_rows idx;
  Protocol *protocol=thd->protocol;
  DBUG_ENTER("mysqld_show_warnings");

  DBUG_ASSERT(thd->get_stmt_da()->is_warning_info_read_only());

  field_list.push_back(new (mem_root)
                       Item_empty_string(thd, "Level", 7),
                       mem_root);
  field_list.push_back(new (mem_root)
                       Item_return_int(thd, "Code", 4, MYSQL_TYPE_LONG),
                       mem_root);
  field_list.push_back(new (mem_root)
                       Item_empty_string(thd, "Message", MYSQL_ERRMSG_SIZE),
                       mem_root);

  if (protocol->send_result_set_metadata(&field_list,
                                         Protocol::SEND_NUM_ROWS |
                                         Protocol::SEND_EOF))
    DBUG_RETURN(TRUE);

  unit->set_limit(sel);

  Diagnostics_area::Sql_condition_iterator it=
    thd->get_stmt_da()->sql_conditions();
  for (idx= 0; (err= it++) ; idx++)
  {
    /* Skip levels that the user is not interested in */
    if (!(levels_to_show & ((ulong) 1 << err->get_level())))
      continue;
    if (unit->lim.check_offset(idx))
      continue;                             // using limit offset,count
    if (idx >= unit->lim.get_select_limit())
      break;
    protocol->prepare_for_resend();
    protocol->store(warning_level_names[err->get_level()].str,
		    warning_level_names[err->get_level()].length,
                    system_charset_info);
    protocol->store((uint32) err->get_sql_errno());
    protocol->store_warning(err->get_message_text(),
                            err->get_message_octet_length());
    if (protocol->write())
      DBUG_RETURN(TRUE);
  }
  my_eof(thd);

  thd->get_stmt_da()->set_warning_info_read_only(FALSE);

  DBUG_RETURN(FALSE);
}


/**
  This replaces U+0000 to '\0000', so the result error message string:
  - is a good null-terminated string
  - presents the entire data
  For example:
    SELECT CAST(_latin1 0x610062 AS SIGNED);
  returns a warning:
    Truncated incorrect INTEGER value: 'a\0000b'
  Notice, the 0x00 byte is replaced to a 5-byte long string '\0000',
  while 'a' and 'b' are printed as is.
*/
extern "C" int my_wc_mb_utf8_null_terminated(CHARSET_INFO *cs,
                                             my_wc_t wc, uchar *r, uchar *e)
{
  return wc == '\0' ?
         cs->wc_to_printable(wc, r, e) :
         my_charset_utf8mb3_handler.wc_mb(cs, wc, r, e);
}


/**
   Convert value for dispatch to error message(see WL#751).

   @param to          buffer for converted string
   @param to_length   size of the buffer
   @param from        string which should be converted
   @param from_length string length
   @param from_cs     charset from convert
 
   @retval
   result string length
*/

size_t err_conv(char *buff, uint to_length, const char *from,
                uint from_length, CHARSET_INFO *from_cs)
{
  char *to= buff;
  const char *from_start= from;
  size_t res;

  DBUG_ASSERT(to_length > 0);
  to_length--;
  if (from_cs == &my_charset_bin)
  {
    uchar char_code;
    res= 0;
    while (1)
    {
      if ((uint)(from - from_start) >= from_length ||
          res >= to_length)
      {
        *to= 0;
        break;
      }

      char_code= ((uchar) *from);
      if (char_code >= 0x20 && char_code <= 0x7E)
      {
        *to++= char_code;
        from++;
        res++;
      }
      else
      {
        if (res + 4 >= to_length)
        {
          *to= 0;
          break;
        }
        res+= my_snprintf(to, 5, "\\x%02X", (uint) char_code);
        to+=4;
        from++;
      }
    }
  }
  else
  {
    uint errors;
    res= my_convert_using_func(to, to_length, system_charset_info,
                               my_wc_mb_utf8_null_terminated,
                               from, from_length, from_cs,
                               from_cs->cset->mb_wc,
                               &errors);
    to[res]= 0;
  }
  return res;
}


/**
   Convert string for dispatch to client(see WL#751).

   @param to          buffer to convert
   @param to_length   buffer length
   @param to_cs       charset to convert
   @param from        string from convert
   @param from_length string length
   @param from_cs     charset from convert
   @param errors      count of errors during convertion

   @retval
   length of converted string
*/

size_t convert_error_message(char *to, size_t to_length, CHARSET_INFO *to_cs,
                             const char *from, size_t from_length,
                             CHARSET_INFO *from_cs, uint *errors)
{
  DBUG_ASSERT(to_length > 0);
  /* Make room for the null terminator. */
  to_length--;

  if (!to_cs || to_cs == &my_charset_bin)
    to_cs= system_charset_info;
  uint32 cnv_length= my_convert_using_func(to, to_length,
                                           to_cs,
                                           to_cs->cset->wc_to_printable,
                                           from, from_length,
                                           from_cs, from_cs->cset->mb_wc,
                                           errors);
  DBUG_ASSERT(to_length >= cnv_length);
  to[cnv_length]= '\0';
  return cnv_length;
}


/**
  Sanity check for SQLSTATEs. The function does not check if it's really an
  existing SQL-state (there are just too many), it just checks string length and
  looks for bad characters.

  @param sqlstate the condition SQLSTATE.

  @retval true if it's ok.
  @retval false if it's bad.
*/

bool is_sqlstate_valid(const LEX_CSTRING *sqlstate)
{
  if (sqlstate->length != 5)
    return false;

  for (int i= 0 ; i < 5 ; ++i)
  {
    char c = sqlstate->str[i];

    if ((c < '0' || '9' < c) &&
	(c < 'A' || 'Z' < c))
      return false;
  }

  return true;
}


void convert_error_to_warning(THD *thd)
{
  DBUG_ASSERT(thd->is_error());
  push_warning(thd, Sql_condition::WARN_LEVEL_WARN,
               thd->get_stmt_da()->sql_errno(),
               thd->get_stmt_da()->message());
  thd->clear_error();
}
