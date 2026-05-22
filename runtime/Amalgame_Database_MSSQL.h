/*
 * Amalgame Standard Library — Amalgame.Database.MSSQL
 * Copyright (c) 2026 Bastien MOUGET
 * https://github.com/amalgame-lang/Amalgame
 *
 * Microsoft SQL Server binding — dynamic-linked to unixODBC (the
 * driver manager) and the vendor-distributed *ODBC Driver for SQL
 * Server* (Microsoft, free redistributable). No vendored
 * implementation; the user binary links against the OS-provided
 * libodbc.so / libodbc.dylib / odbc32.dll plus the MS driver.
 *
 * Works against on-prem SQL Server (2017+), Azure SQL Database,
 * and Azure SQL Managed Instance. The wire protocol is TDS, fully
 * handled inside the driver.
 *
 * Surface (v1):
 *   Open(connStr) / Close / IsOpen / LastError     lifecycle + diag
 *   Exec(sql)                                       DDL / INSERT / UPDATE / DELETE
 *   QueryAll(sql) -> List<List<string>>             SELECT all rows × cols
 *   Changes()                                       rows affected by last Exec
 *   ServerVersion()                                 SQL Server version string
 *
 * Connection string follows the ODBC keyword=value syntax that
 * SQLDriverConnect expects:
 *
 *   "Driver={ODBC Driver 18 for SQL Server};Server=localhost,1433;
 *    Database=master;UID=sa;PWD=s3cret;TrustServerCertificate=yes"
 *
 *   "Driver={ODBC Driver 18 for SQL Server};Server=tcp:myserver.database.windows.net,1433;
 *    Database=mydb;Authentication=ActiveDirectoryIntegrated"
 *
 * NULL cells in QueryAll materialise as the empty string —
 * callers that need to distinguish NULL from `""` should wait
 * for v2's parameter binding + typed accessors.
 *
 * Threading: AmalgameMSSQL* is single-owner. ODBC handles are
 * not safely sharable across threads under load. Distinct
 * handles per thread are fine.
 *
 * Memory: ODBC statement handles are SQLFreeHandle'd as soon as
 * we've copied the data we need. The connection handle and env
 * handle live for the lifetime of the AmalgameMSSQL* and are
 * released in Close(). GC finalizer registered so a leaked
 * handle still releases everything eventually.
 *
 * Out of scope (v2):
 *   - SQLBindParameter parameter binding (?-placeholders)
 *   - SQLPrepare + repeated SQLExecute statement reuse
 *   - Typed column accessors (the current API stringifies every
 *     cell via SQLGetData(SQL_C_CHAR))
 *   - Bulk insert (SQLBulkOperations / bcp_*)
 *   - Async query mode (SQL_ATTR_ASYNC_ENABLE)
 *   - Table-valued parameters
 *   - First-class AAD / managed identity helpers (works today
 *     via the connection-string `Authentication=` keyword)
 */

#ifndef AMALGAME_DATABASE_MSSQL_H
#define AMALGAME_DATABASE_MSSQL_H

#include "_runtime.h"
#include "Amalgame_Collections.h"

/* Resolve <sql.h> / <sqlext.h> across the common system layouts.
 * unixODBC installs them as <sql.h> / <sqlext.h> directly on
 * every Linux distro and macOS Homebrew prefix; some MSYS2
 * setups nest them under <unixodbc/...>. */
#if defined(__has_include)
#  if __has_include(<sql.h>)
#    include <sql.h>
#    include <sqlext.h>
#  elif __has_include(<unixodbc/sql.h>)
#    include <unixodbc/sql.h>
#    include <unixodbc/sqlext.h>
#  else
#    error "sql.h not found. Install unixodbc-dev / unixODBC-devel."
#  endif
#else
#  include <sql.h>
#  include <sqlext.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct AmalgameMSSQL {
    SQLHENV  env;          /* environment handle; NULL when closed */
    SQLHDBC  dbc;          /* connection handle; NULL when closed */
    char*    last_error;   /* GC-strdup'd, or NULL */
    i64      last_changes; /* rows affected by the last Exec */
} AmalgameMSSQL;

/* ── Helpers ────────────────────────────────────────── */

static inline code_string _amms_err_dup(const char* msg) {
    if (!msg) return NULL;
    size_t n = strlen(msg);
    char* p = (char*) code_alloc(n + 1);
    memcpy(p, msg, n + 1);
    return p;
}

/* Walk ODBC's diagnostic-record chain for a given handle and
 * concatenate every message into a single GC-strdup'd string.
 * Drops the trailing newline so the result composes cleanly. */
static inline code_string _amms_err_from_handle(SQLSMALLINT htype, SQLHANDLE h) {
    if (!h) return _amms_err_dup("null handle");
    char buf[4096];
    size_t off = 0;
    buf[0] = '\0';
    SQLINTEGER  native = 0;
    SQLCHAR     state[8];
    SQLCHAR     msg[1024];
    SQLSMALLINT mlen = 0;
    SQLSMALLINT i = 1;
    while (SQLGetDiagRec(htype, h, i, state, &native, msg, sizeof(msg), &mlen)
           == SQL_SUCCESS) {
        int wrote = snprintf(buf + off, sizeof(buf) - off,
                             "%s[%s] %s",
                             (i == 1) ? "" : "; ",
                             (const char*) state,
                             (const char*) msg);
        if (wrote < 0 || (size_t) wrote >= sizeof(buf) - off) break;
        off += (size_t) wrote;
        i++;
    }
    if (off == 0) return _amms_err_dup("");
    /* Strip trailing \r\n the driver often appends. */
    while (off > 0 && (buf[off - 1] == '\n' || buf[off - 1] == '\r')) off--;
    buf[off] = '\0';
    return _amms_err_dup(buf);
}

/* GC finalizer — releases ODBC handles if the user dropped the
 * AmalgameMSSQL* without calling Close. */
static void _amms_finalize(void* obj, void* cd) {
    (void) cd;
    AmalgameMSSQL* db = (AmalgameMSSQL*) obj;
    if (!db) return;
    if (db->dbc) {
        SQLDisconnect(db->dbc);
        SQLFreeHandle(SQL_HANDLE_DBC, db->dbc);
        db->dbc = NULL;
    }
    if (db->env) {
        SQLFreeHandle(SQL_HANDLE_ENV, db->env);
        db->env = NULL;
    }
}

static inline AmalgameMSSQL* _amms_alloc(void) {
    AmalgameMSSQL* db =
        (AmalgameMSSQL*) GC_MALLOC(sizeof(AmalgameMSSQL));
    db->env          = NULL;
    db->dbc          = NULL;
    db->last_error   = NULL;
    db->last_changes = 0;
    GC_register_finalizer(db, _amms_finalize, NULL, NULL, NULL);
    return db;
}

/* ── Lifecycle ──────────────────────────────────────── */

static inline AmalgameMSSQL* Amalgame_Database_MSSQL_Open(code_string connStr) {
    AmalgameMSSQL* db = _amms_alloc();
    if (!connStr) {
        db->last_error = _amms_err_dup("null connection string");
        return db;
    }

    SQLRETURN rc = SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &db->env);
    if (!SQL_SUCCEEDED(rc)) {
        db->last_error = _amms_err_dup("SQLAllocHandle(ENV) failed");
        return db;
    }
    rc = SQLSetEnvAttr(db->env, SQL_ATTR_ODBC_VERSION,
                       (SQLPOINTER) SQL_OV_ODBC3, 0);
    if (!SQL_SUCCEEDED(rc)) {
        db->last_error = _amms_err_from_handle(SQL_HANDLE_ENV, db->env);
        SQLFreeHandle(SQL_HANDLE_ENV, db->env);
        db->env = NULL;
        return db;
    }
    rc = SQLAllocHandle(SQL_HANDLE_DBC, db->env, &db->dbc);
    if (!SQL_SUCCEEDED(rc)) {
        db->last_error = _amms_err_from_handle(SQL_HANDLE_ENV, db->env);
        SQLFreeHandle(SQL_HANDLE_ENV, db->env);
        db->env = NULL;
        return db;
    }

    SQLCHAR     outbuf[1024];
    SQLSMALLINT outlen = 0;
    rc = SQLDriverConnect(db->dbc, NULL,
                          (SQLCHAR*) connStr, SQL_NTS,
                          outbuf, sizeof(outbuf), &outlen,
                          SQL_DRIVER_NOPROMPT);
    if (!SQL_SUCCEEDED(rc)) {
        db->last_error = _amms_err_from_handle(SQL_HANDLE_DBC, db->dbc);
        SQLFreeHandle(SQL_HANDLE_DBC, db->dbc);
        SQLFreeHandle(SQL_HANDLE_ENV, db->env);
        db->dbc = NULL;
        db->env = NULL;
        return db;
    }
    return db;
}

static inline void Amalgame_Database_MSSQL_Close(AmalgameMSSQL* db) {
    if (!db) return;
    if (db->dbc) {
        SQLDisconnect(db->dbc);
        SQLFreeHandle(SQL_HANDLE_DBC, db->dbc);
        db->dbc = NULL;
    }
    if (db->env) {
        SQLFreeHandle(SQL_HANDLE_ENV, db->env);
        db->env = NULL;
    }
}

static inline code_bool Amalgame_Database_MSSQL_IsOpen(AmalgameMSSQL* db) {
    return (db && db->dbc) ? 1 : 0;
}

static inline code_string Amalgame_Database_MSSQL_LastError(AmalgameMSSQL* db) {
    if (!db || !db->last_error) return (code_string) "";
    return db->last_error;
}

/* ── Exec ───────────────────────────────────────────── */

/* Run a no-result SQL statement (DDL / INSERT / UPDATE / DELETE).
 * Allocates a fresh statement handle per call (v2 will reuse via
 * SQLPrepare). Updates Changes() with SQLRowCount. */
static inline code_bool Amalgame_Database_MSSQL_Exec(
        AmalgameMSSQL* db, code_string sql) {
    if (!db || !db->dbc) {
        if (db) db->last_error = _amms_err_dup("connection not open");
        return 0;
    }
    if (!sql) {
        db->last_error = _amms_err_dup("null sql");
        return 0;
    }
    SQLHSTMT stmt = NULL;
    SQLRETURN rc = SQLAllocHandle(SQL_HANDLE_STMT, db->dbc, &stmt);
    if (!SQL_SUCCEEDED(rc)) {
        db->last_error = _amms_err_from_handle(SQL_HANDLE_DBC, db->dbc);
        return 0;
    }
    rc = SQLExecDirect(stmt, (SQLCHAR*) sql, SQL_NTS);
    if (!SQL_SUCCEEDED(rc) && rc != SQL_NO_DATA) {
        db->last_error = _amms_err_from_handle(SQL_HANDLE_STMT, stmt);
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        return 0;
    }
    SQLLEN n = 0;
    if (SQL_SUCCEEDED(SQLRowCount(stmt, &n))) {
        db->last_changes = (i64) (n >= 0 ? n : 0);
    } else {
        db->last_changes = 0;
    }
    SQLFreeHandle(SQL_HANDLE_STMT, stmt);
    db->last_error = _amms_err_dup("");
    return 1;
}

/* ── QueryAll ───────────────────────────────────────── */

/* SELECT and return every row as a List<List<string>>. Each cell
 * is fetched via SQLGetData(SQL_C_CHAR), so every value flows
 * through the driver's text conversion (handles ints, decimals,
 * dates, GUIDs uniformly). NULL cells materialise as "".
 *
 * On error the outer list is empty and LastError is set. */
static inline AmalgameList* Amalgame_Database_MSSQL_QueryAll(
        AmalgameMSSQL* db, code_string sql) {
    AmalgameList* rows = AmalgameList_new();
    if (!db || !db->dbc) {
        if (db) db->last_error = _amms_err_dup("connection not open");
        return rows;
    }
    if (!sql) {
        db->last_error = _amms_err_dup("null sql");
        return rows;
    }
    SQLHSTMT stmt = NULL;
    SQLRETURN rc = SQLAllocHandle(SQL_HANDLE_STMT, db->dbc, &stmt);
    if (!SQL_SUCCEEDED(rc)) {
        db->last_error = _amms_err_from_handle(SQL_HANDLE_DBC, db->dbc);
        return rows;
    }
    rc = SQLExecDirect(stmt, (SQLCHAR*) sql, SQL_NTS);
    if (!SQL_SUCCEEDED(rc) && rc != SQL_NO_DATA) {
        db->last_error = _amms_err_from_handle(SQL_HANDLE_STMT, stmt);
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        return rows;
    }
    SQLSMALLINT ncols = 0;
    if (!SQL_SUCCEEDED(SQLNumResultCols(stmt, &ncols))) {
        db->last_error = _amms_err_from_handle(SQL_HANDLE_STMT, stmt);
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        return rows;
    }

    SQLLEN nrows = 0;
    while ((rc = SQLFetch(stmt)) == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO) {
        AmalgameList* one = AmalgameList_new();
        for (SQLSMALLINT j = 1; j <= ncols; j++) {
            /* Two-pass SQLGetData to size the buffer correctly for
             * cells longer than the initial probe. */
            char probe[256];
            SQLLEN ind = 0;
            SQLRETURN gd = SQLGetData(stmt, j, SQL_C_CHAR,
                                       probe, (SQLLEN) sizeof(probe), &ind);
            if (gd == SQL_NULL_DATA || ind == SQL_NULL_DATA) {
                char* dup = (char*) code_alloc(1);
                dup[0] = '\0';
                AmalgameList_add(one, (void*) dup);
                continue;
            }
            if (!SQL_SUCCEEDED(gd)) {
                /* On per-cell error, drop an empty string and keep going. */
                char* dup = (char*) code_alloc(1);
                dup[0] = '\0';
                AmalgameList_add(one, (void*) dup);
                continue;
            }
            if (gd == SQL_SUCCESS_WITH_INFO && ind > (SQLLEN) (sizeof(probe) - 1)) {
                /* Cell was truncated — allocate a buffer that fits the
                 * full payload (ind is the would-be length sans NUL),
                 * copy the probe head, and pull the remainder. */
                size_t total = (size_t) ind;
                char* full = (char*) code_alloc(total + 1);
                size_t got = sizeof(probe) - 1;
                memcpy(full, probe, got);
                SQLLEN ind2 = 0;
                SQLRETURN gd2 = SQLGetData(stmt, j, SQL_C_CHAR,
                                            full + got,
                                            (SQLLEN) (total - got + 1),
                                            &ind2);
                if (!SQL_SUCCEEDED(gd2)) {
                    full[got] = '\0';
                }
                AmalgameList_add(one, (void*) full);
            } else {
                size_t n = (ind > 0) ? (size_t) ind : strlen(probe);
                char* dup = (char*) code_alloc(n + 1);
                if (n > 0) memcpy(dup, probe, n);
                dup[n] = '\0';
                AmalgameList_add(one, (void*) dup);
            }
        }
        AmalgameList_add(rows, (void*) one);
        nrows++;
    }
    if (rc != SQL_NO_DATA && !SQL_SUCCEEDED(rc)) {
        db->last_error = _amms_err_from_handle(SQL_HANDLE_STMT, stmt);
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        return rows;
    }
    db->last_changes = (i64) nrows;
    SQLFreeHandle(SQL_HANDLE_STMT, stmt);
    db->last_error = _amms_err_dup("");
    return rows;
}

/* Rows affected by the last Exec, or row count of the last QueryAll. */
static inline i64 Amalgame_Database_MSSQL_Changes(AmalgameMSSQL* db) {
    return db ? db->last_changes : 0;
}

/* SQL Server version string via SQLGetInfo(SQL_DBMS_VER) — e.g.
 * "16.00.4115" for SQL Server 2022, "15.00.4153" for 2019. Empty
 * when the connection is closed. */
static inline code_string Amalgame_Database_MSSQL_ServerVersion(AmalgameMSSQL* db) {
    if (!db || !db->dbc) return (code_string) "";
    char buf[128];
    SQLSMALLINT outlen = 0;
    SQLRETURN rc = SQLGetInfo(db->dbc, SQL_DBMS_VER,
                              buf, (SQLSMALLINT) sizeof(buf), &outlen);
    if (!SQL_SUCCEEDED(rc) || outlen <= 0) return (code_string) "";
    buf[outlen] = '\0';
    return _amms_err_dup(buf);
}

#endif /* AMALGAME_DATABASE_MSSQL_H */
