/*
 * sql_firebird.c Part of Firebird rlm_sql driver
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * Copyright 2006  The FreeRADIUS server project
 * Copyright 2006  Vitaly Bodzhgua <vitaly@eastera.net>
 */

#include <freeradius-devel/ident.h>
RCSID("$Id$")

#include "sql_fbapi.h"


/* Forward declarations */
static const char *sql_error(rlm_sql_handle_t *handle, rlm_sql_config_t *config);
static int sql_free_result(rlm_sql_handle_t *handle, rlm_sql_config_t *config);
static int sql_affected_rows(rlm_sql_handle_t *handle, rlm_sql_config_t *config);
static int sql_num_fields(rlm_sql_handle_t *handle, rlm_sql_config_t *config);
static int sql_finish_query(rlm_sql_handle_t *handle, rlm_sql_config_t *config);

static int sql_socket_destructor(void *c)
{
	rlm_sql_firebird_conn_t *conn = c;
	int i;
	
	DEBUG2("rlm_sql_firebird: socket destructor called, closing socket");
	
	fb_commit(conn);
	if (conn->dbh) {
		fb_free_statement(conn);
		isc_detach_database(conn->status, &(conn->dbh));
		
		if (fb_lasterror(conn)) {
			DEBUGW("rlm_sql_firebird: Got error "
			       "when closing socket: %s", conn->lasterror);
		}
	}
	
#ifdef _PTHREAD_H
	pthread_mutex_destroy (&conn->mut);
#endif

	for (i=0; i < conn->row_fcount; i++) {
		free(conn->row[i]);
	}
	
	free(conn->row);
	free(conn->row_sizes);
	fb_free_sqlda(conn->sqlda_out);
	
	free(conn->sqlda_out);
	free(conn->tpb);
	free(conn->dpb);
	
	if (conn->lasterror) {
		free(conn->lasterror);
	}

	return 0;
}

/** Establish connection to the db
 *
 */
static int sql_socket_init(rlm_sql_handle_t *handle, rlm_sql_config_t *config) {
	rlm_sql_firebird_conn_t	*conn;
	
	long res;

	MEM(conn = handle->conn = talloc_zero(handle, rlm_sql_firebird_conn_t));
	talloc_set_destructor((void *) conn, sql_socket_destructor);

	res = fb_init_socket(conn);
	if (res) {
		return -1;
	}
	
	if (fb_connect(conn,config)) {
		radlog(L_ERR, "rlm_sql_firebird: Connection failed %s\n",
		       conn->lasterror);
		
		return SQL_DOWN;
	}

	return 0;
}

/** Issue a non-SELECT query (ie: update/delete/insert) to the database.
 *
 */
static int sql_query(rlm_sql_handle_t *handle, UNUSED rlm_sql_config_t *config,
		     char *querystr) {
	rlm_sql_firebird_conn_t *conn = handle->conn;
	
	int deadlock = 0;

#ifdef _PTHREAD_H
	pthread_mutex_lock(&conn->mut);
#endif

	try_again:
	/*
	 *	Try again query when deadlock, beacuse in any case it
	 *	will be retried.
	 */
 	if (fb_sql_query(conn,querystr)) {
		/* but may be lost for short sessions */
   		if ((conn->sql_code == DEADLOCK_SQL_CODE) &&
   		    !deadlock) {
	  		radlog(L_DBG,"conn_id deadlock. Retry query %s",
	  		       querystr);
	  		
			/*
			 *	@todo For non READ_COMMITED transactions put
			 *	rollback here
			 *	fb_rollback(conn);
			 */
	  		deadlock = 1;
	  		goto try_again;
	  	}
  	
		radlog(L_ERR, "conn_id rlm_sql_firebird,sql_query error: "
		       "sql_code=%li, error='%s', query=%s",
		       (long int) conn->sql_code,
		       conn->lasterror,
		       querystr);

		if (conn->sql_code == DOWN_SQL_CODE) {
			return SQL_DOWN;
		}
	
		/* Free problem query */
		if (fb_rollback(conn)) {
			//assume the network is down if rollback had failed
			radlog(L_ERR,"Fail to rollback transaction after "
			       "previous error. Error: %s",
			       conn->lasterror);
		
			return SQL_DOWN;
		}
		//   conn->in_use=0;
		return -1;
   	}

	if (conn->statement_type != isc_info_sql_stmt_select) {
		if (fb_commit(conn)) {
			return -1;
		}
	}

	return 0;
}

/** Issue a select query to the database.
 *
 */
static int sql_select_query(rlm_sql_handle_t *handle, rlm_sql_config_t *config,
			    char *querystr) {
	return sql_query(handle, config, querystr);
}

/** Returns a result set for the query.
 *
 */
static int sql_store_result(UNUSED rlm_sql_handle_t *handle,
			    UNUSED rlm_sql_config_t *config) {
	return 0;
}

/** Returns number of columns from query.
 *
 */
static int sql_num_fields(rlm_sql_handle_t *handle,
			  UNUSED rlm_sql_config_t *config) {
	return ((rlm_sql_firebird_conn_t *) handle->conn)->sqlda_out->sqld;
}

/** Returns number of rows in query.
 *
 */
static int sql_num_rows(rlm_sql_handle_t *handle, rlm_sql_config_t *config) {
	return sql_affected_rows(handle, config);
}

/** Returns an individual row.
 *
 */
static int sql_fetch_row(rlm_sql_handle_t *handle, UNUSED rlm_sql_config_t *config)
{
	rlm_sql_firebird_conn_t *conn = handle->conn;
	int res;
	
	handle->row = NULL;
	
	if (conn->statement_type != isc_info_sql_stmt_exec_procedure) {
		res = fb_fetch(conn);
		if (res == 100) {
			return 0;
	 	}
	 	
	 	if (res) {
	  		radlog(L_ERR, "rlm_sql_firebird. Fetch problem:'%s'",
	  		       conn->lasterror);
	  		
	   		return -1;
	 	}
	} else {
		conn->statement_type=0;
	}
	
	fb_store_row(conn);

	handle->row = conn->row;
	
	return 0;
}

/** End the select query, such as freeing memory or result.
 *
 */
static int sql_finish_select_query(rlm_sql_handle_t *handle,
				   UNUSED rlm_sql_config_t *config) {
				
	rlm_sql_firebird_conn_t *conn = (rlm_sql_firebird_conn_t *) handle->conn;
	
	fb_commit(conn);
	fb_close_cursor(conn);
	
	return 0;
}

/** End the query
 *
 */
static int sql_finish_query(rlm_sql_handle_t *handle,
			    rlm_sql_config_t *config) {
	sql_free_result(handle, config);
	
	return 0;
}

/** Frees memory allocated for a result set.
 *
 */
static int sql_free_result(UNUSED rlm_sql_handle_t *handle,
			   UNUSED rlm_sql_config_t *config) {
	return 0;
}

/** Closes an open database connection and cleans up any open handles.
 *
 */
static int sql_close(rlm_sql_handle_t *handle, UNUSED rlm_sql_config_t *config)
{
	fb_destroy_socket((rlm_sql_firebird_conn_t *) handle->conn);
	return 0;
}

/** Returns error associated with connection.
 *
 */
static const char *sql_error(rlm_sql_handle_t *handle,
			     UNUSED rlm_sql_config_t *config) {
	rlm_sql_firebird_conn_t *conn = handle->conn;
	
	return conn->lasterror;
}

/** Return the number of rows affected by the query (update, or insert)
 *
 */
static int sql_affected_rows(rlm_sql_handle_t *handle, rlm_sql_config_t *config) {
	int affected_rows=fb_affected_rows(handle->conn);
	
	if (affected_rows < 0) {
		radlog(L_ERR, "sql_affected_rows, rlm_sql_firebird. error:%s\n",
		       sql_error(handle,config));
	}
	
	return affected_rows;
}

/* Exported to rlm_sql */
rlm_sql_module_t rlm_sql_firebird = {
	"rlm_sql_firebird",
	NULL,
	sql_socket_init,
	sql_query,
	sql_select_query,
	sql_store_result,
	sql_num_fields,
	sql_num_rows,
	sql_fetch_row,
	sql_free_result,
	sql_error,
	sql_finish_query,
	sql_finish_select_query,
	sql_affected_rows
};