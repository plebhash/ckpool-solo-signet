/*
 * Copyright 2003-2014 Andrew Smith
 * Copyright 2014 Con Kolivas
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 3 of the License, or (at your option)
 * any later version.  See COPYING for more details.
 */

#include "config.h"

#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <jansson.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <regex.h>
#ifdef HAVE_LIBPQ_FE_H
#include <libpq-fe.h>
#elif defined (HAVE_POSTGRESQL_LIBPQ_FE_H)
#include <postgresql/libpq-fe.h>
#endif

#include "ckpool.h"
#include "libckpool.h"

#include "klist.h"
#include "ktree.h"

// TODO: a lot of the tree access isn't locked
// will need to be if threading is required

static char *db_user;
static char *db_pass;

// size limit on the command string
#define ID_SIZ 31

#define TXT_BIG 256
#define TXT_MED 128
#define TXT_SML 64
#define TXT_FLAG 1

#define FLDSEP 0x02

// Ensure long long and int64_t are both 8 bytes (and thus also the same)
#define ASSERT1(condition) __maybe_unused static char sizeof_longlong_must_be_8[(condition)?1:-1]
#define ASSERT2(condition) __maybe_unused static char sizeof_int64_t_must_be_8[(condition)?1:-1]
ASSERT1(sizeof(long long) == 8);
ASSERT2(sizeof(int64_t) == 8);

#define PGOK(_res) ((_res) == PGRES_COMMAND_OK || \
			(_res) == PGRES_TUPLES_OK || \
			(_res) == PGRES_EMPTY_QUERY)

#define PGLOG(__LOG, __str, __rescode, __conn) do { \
		char *__ptr, *__buf = strdup(PQerrorMessage(__conn)); \
		__ptr = __buf + strlen(__buf) - 1; \
		while (__ptr >= __buf && (*__ptr == '\n' || *__ptr == '\r')) \
			*(__ptr--) = '\0'; \
		while (--__ptr >= __buf) \
			if (*__ptr == '\n' || *__ptr == '\r' || *__ptr == '\t') \
				*__ptr = ' '; \
		__LOG("%s(): %s failed (%d) '%s'", __func__, \
			__str, (int)rescode, __buf); \
		free(__buf); \
	} while (0)

#define PGLOGERR(_str, _rescode, _conn) PGLOG(LOGERR, _str, _rescode, _conn)

/* N.B. STRNCPY() truncates, whereas txt_to_str() aborts ckdb if src > trg
 * If copying data from the DB, code should always use txt_to_str() since
 * data should never be lost/truncated if it came from the DB -
 * that simply implies a code bug or a database change that must be fixed */
#define STRNCPY(trg, src) do { \
		strncpy((char *)(trg), (char *)(src), sizeof(trg)); \
		trg[sizeof(trg) - 1] = '\0'; \
	} while (0)

#define STRNCPYSIZ(trg, src, siz) do { \
		strncpy((char *)(trg), (char *)(src), siz); \
		trg[siz - 1] = '\0'; \
	} while (0)

#define APPEND_REALLOC(_dst, _dstoff, _dstsiz, _src) do { \
		size_t _srclen = strlen(_src); \
		if ((_dstoff) + _srclen >= (_dstsiz)) { \
			_dstsiz += 1024; \
			_dst = realloc(_dst, _dstsiz); \
			if (!(_dst)) \
				quithere(1, "realloc (%d) OOM", (int)_dstsiz); \
		} \
		strcpy((_dst)+(_dstoff), _src); \
		_dstoff += _srclen; \
	} while(0)

enum data_type {
	TYPE_STR,
	TYPE_BIGINT,
	TYPE_INT,
	TYPE_TV,
	TYPE_BLOB,
	TYPE_DOUBLE
};

#define TXT_TO_STR(__nam, __fld, __data) txt_to_str(__nam, __fld, (__data), sizeof(__data))
#define TXT_TO_BIGINT(__nam, __fld, __data) txt_to_bigint(__nam, __fld, &(__data), sizeof(__data))
#define TXT_TO_INT(__nam, __fld, __data) txt_to_int(__nam, __fld, &(__data), sizeof(__data))
#define TXT_TO_TV(__nam, __fld, __data) txt_to_tv(__nam, __fld, &(__data), sizeof(__data))
#define TXT_TO_BLOB(__nam, __fld, __data) txt_to_blob(__nam, __fld, __data)
#define TXT_TO_DOUBLE(__nam, __fld, __data) txt_to_double(__nam, __fld, &(__data), sizeof(__data))

#define PQ_GET_FLD(__res, __row, __name, __fld, __ok) do { \
		int __col = PQfnumber(__res, __name); \
		if (__col == -1) { \
			LOGERR("%s(): Unknown field '%s' row %d", __func__, __name, __row); \
			__ok = false; \
		} else \
			__fld = PQgetvalue(__res, __row, __col); \
	} while (0)

// HISTORY FIELDS
#define HISTORYDATECONTROL ",createdate,createby,createcode,createinet,expirydate"
#define HISTORYDATECOUNT 5

#define HISTORYDATEFLDS(_res, _row, _data, _ok) do { \
		char *_fld; \
		PQ_GET_FLD(_res, _row, "createdate", _fld, _ok); \
		if (!_ok) \
			break; \
		TXT_TO_TV("createdate", _fld, (_data)->createdate); \
		PQ_GET_FLD(_res, _row, "createby", _fld, _ok); \
		if (!_ok) \
			break; \
		TXT_TO_STR("createby", _fld, (_data)->createby); \
		PQ_GET_FLD(_res, _row, "createcode", _fld, _ok); \
		if (!_ok) \
			break; \
		TXT_TO_STR("createcode", _fld, (_data)->createcode); \
		PQ_GET_FLD(_res, _row, "createinet", _fld, _ok); \
		if (!_ok) \
			break; \
		TXT_TO_STR("createinet", _fld, (_data)->createinet); \
		PQ_GET_FLD(_res, _row, "expirydate", _fld, _ok); \
		if (!_ok) \
			break; \
		TXT_TO_TV("expirydate", _fld, (_data)->expirydate); \
	} while (0)

#define HISTORYDATECONTROLFIELDS \
	tv_t createdate; \
	char createby[TXT_SML+1]; \
	char createcode[TXT_MED+1]; \
	char createinet[TXT_MED+1]; \
	tv_t expirydate

#define HISTORYDATEPARAMS(_params, _his_pos, _row) do { \
		_params[_his_pos++] = tv_to_buf(&(_row->createdate), NULL, 0); \
		_params[_his_pos++] = str_to_buf(_row->createby, NULL, 0); \
		_params[_his_pos++] = str_to_buf(_row->createcode, NULL, 0); \
		_params[_his_pos++] = str_to_buf(_row->createinet, NULL, 0); \
		_params[_his_pos++] = tv_to_buf(&(_row->expirydate), NULL, 0); \
	} while (0)

// 6-Jun-6666 06:06:06+00
#define DEFAULT_EXPIRY 148204965966L
// 1-Jun-6666 00:00:00+00
#define COMPARE_EXPIRY 148204512000L

static const tv_t default_expiry = { DEFAULT_EXPIRY, 0L };

// 31-Dec-9999 23:59:59+00
#define DATE_S_EOT 253402300799L
#define DATE_uS_EOT 0L
static const tv_t date_eot = { DATE_S_EOT, DATE_uS_EOT };

#define HISTORYDATEINIT(_row, _now, _by, _code, _inet) do { \
		_row->createdate.tv_sec = (_now)->tv_sec; \
		_row->createdate.tv_usec = (_now)->tv_usec; \
		STRNCPY(_row->createby, _by); \
		STRNCPY(_row->createcode, _code); \
		STRNCPY(_row->createinet, _inet); \
		_row->expirydate.tv_sec = default_expiry.tv_sec; \
		_row->expirydate.tv_usec = default_expiry.tv_usec; \
	} while (0)

// Override _row defaults if transfer fields are present
#define HISTORYDATETRANSFER(_row) do { \
		K_ITEM *item; \
		item = optional_name("createdate", 10, NULL); \
		if (item) { \
			long sec, usec; \
			int n; \
			n = sscanf(DATA_TRANSFER(item)->data, "%ld,%ld", &sec, &usec); \
			if (n > 0) { \
				_row->createdate.tv_sec = (time_t)sec; \
				if (n > 1) \
					_row->createdate.tv_usec = usec; \
				else \
					_row->createdate.tv_usec = 0L; \
			} \
		} \
		item = optional_name("createby", 1, NULL); \
		if (item) \
			STRNCPY(_row->createby, DATA_TRANSFER(item)->data); \
		item = optional_name("createcode", 1, NULL); \
		if (item) \
			STRNCPY(_row->createcode, DATA_TRANSFER(item)->data); \
		item = optional_name("createinet", 1, NULL); \
		if (item) \
			STRNCPY(_row->createinet, DATA_TRANSFER(item)->data); \
	} while (0)

// MODIFY FIELDS
#define MODIFYDATECONTROL ",createdate,createby,createcode,createinet" \
			  ",modifydate,modifyby,modifycode,modifyinet"
#define MODIFYDATECOUNT 8

#define MODIFYDATEFLDS(_res, _row, _data, _ok) do { \
		char *_fld; \
		PQ_GET_FLD(_res, _row, "createdate", _fld, _ok); \
		if (!_ok) \
			break; \
		TXT_TO_TV("createdate", _fld, (_data)->createdate); \
		PQ_GET_FLD(_res, _row, "createby", _fld, _ok); \
		if (!_ok) \
			break; \
		TXT_TO_STR("createby", _fld, (_data)->createby); \
		PQ_GET_FLD(_res, _row, "createcode", _fld, _ok); \
		if (!_ok) \
			break; \
		TXT_TO_STR("createcode", _fld, (_data)->createcode); \
		PQ_GET_FLD(_res, _row, "createinet", _fld, _ok); \
		if (!_ok) \
			break; \
		TXT_TO_STR("createinet", _fld, (_data)->createinet); \
		PQ_GET_FLD(_res, _row, "modifydate", _fld, _ok); \
		if (!_ok) \
			break; \
		TXT_TO_TV("modifydate", _fld, (_data)->modifydate); \
		PQ_GET_FLD(_res, _row, "modifyby", _fld, _ok); \
		if (!_ok) \
			break; \
		TXT_TO_STR("modifyby", _fld, (_data)->modifyby); \
		PQ_GET_FLD(_res, _row, "modifycode", _fld, _ok); \
		if (!_ok) \
			break; \
		TXT_TO_STR("modifycode", _fld, (_data)->modifycode); \
		PQ_GET_FLD(_res, _row, "modifyinet", _fld, _ok); \
		if (!_ok) \
			break; \
		TXT_TO_STR("modifyinet", _fld, (_data)->modifyinet); \
	} while (0)

#define MODIFYDATECONTROLFIELDS \
	tv_t createdate; \
	char createby[TXT_SML+1]; \
	char createcode[TXT_MED+1]; \
	char createinet[TXT_MED+1]; \
	tv_t modifydate; \
	char modifyby[TXT_SML+1]; \
	char modifycode[TXT_MED+1]; \
	char modifyinet[TXT_MED+1]

#define MODIFYDATEPARAMS(_params, _mod_pos, _row) do { \
		_params[_mod_pos++] = tv_to_buf(&(_row->createdate), NULL, 0); \
		_params[_mod_pos++] = str_to_buf(_row->createby, NULL, 0); \
		_params[_mod_pos++] = str_to_buf(_row->createcode, NULL, 0); \
		_params[_mod_pos++] = str_to_buf(_row->createinet, NULL, 0); \
		_params[_mod_pos++] = tv_to_buf(&(_row->modifydate), NULL, 0); \
		_params[_mod_pos++] = str_to_buf(_row->modifyby, NULL, 0); \
		_params[_mod_pos++] = str_to_buf(_row->modifycode, NULL, 0); \
		_params[_mod_pos++] = str_to_buf(_row->modifyinet, NULL, 0); \
	} while (0)

#define MODIFYDATEINIT(_row, _now, _by, _code, _inet) do { \
		_row->createdate.tv_sec = (_now)->tv_sec; \
		_row->createdate.tv_usec = (_now)->tv_usec; \
		STRNCPY(_row->createby, _by); \
		STRNCPY(_row->createcode, _code); \
		STRNCPY(_row->createinet, _inet); \
		_row->modifydate.tv_sec = 0; \
		_row->modifydate.tv_usec = 0; \
		_row->modifyby[0] = '\0'; \
		_row->modifycode[0] = '\0'; \
		_row->modifyinet[0] = '\0'; \
	} while (0)

// SIMPLE FIELDS
#define SIMPLEDATECONTROL ",createdate,createby,createcode,createinet"
#define SIMPLEDATECOUNT 4

#define SIMPLEDATEFLDS(_res, _row, _data, _ok) do { \
		char *_fld; \
		PQ_GET_FLD(_res, _row, "createdate", _fld, _ok); \
		if (!_ok) \
			break; \
		TXT_TO_TV("createdate", _fld, (_data)->createdate); \
		PQ_GET_FLD(_res, _row, "createby", _fld, _ok); \
		if (!_ok) \
			break; \
		TXT_TO_STR("createby", _fld, (_data)->createby); \
		PQ_GET_FLD(_res, _row, "createcode", _fld, _ok); \
		if (!_ok) \
			break; \
		TXT_TO_STR("createcode", _fld, (_data)->createcode); \
		PQ_GET_FLD(_res, _row, "createinet", _fld, _ok); \
		if (!_ok) \
			break; \
		TXT_TO_STR("createinet", _fld, (_data)->createinet); \
	} while (0)

#define SIMPLEDATECONTROLFIELDS \
	tv_t createdate; \
	char createby[TXT_SML+1]; \
	char createcode[TXT_MED+1]; \
	char createinet[TXT_MED+1]

#define SIMPLEDATEPARAMS(_params, _his_pos, _row) do { \
		_params[_his_pos++] = tv_to_buf(&(_row->createdate), NULL, 0); \
		_params[_his_pos++] = str_to_buf(_row->createby, NULL, 0); \
		_params[_his_pos++] = str_to_buf(_row->createcode, NULL, 0); \
		_params[_his_pos++] = str_to_buf(_row->createinet, NULL, 0); \
	} while (0)

#define SIMPLEDATEINIT(_row, _now, _by, _code, _inet) do { \
		_row->createdate.tv_sec = (_now)->tv_sec; \
		_row->createdate.tv_usec = (_now)->tv_usec; \
		STRNCPY(_row->createby, _by); \
		STRNCPY(_row->createcode, _code); \
		STRNCPY(_row->createinet, _inet); \
	} while (0)

// Override _row defaults if transfer fields are present
#define SIMPLEDATETRANSFER(_row) do { \
		K_ITEM *item; \
		item = optional_name("createdate", 10, NULL); \
		if (item) { \
			long sec, usec; \
			int n; \
			n = sscanf(DATA_TRANSFER(item)->data, "%ld,%ld", &sec, &usec); \
			if (n > 0) { \
				_row->createdate.tv_sec = (time_t)sec; \
				if (n > 1) \
					_row->createdate.tv_usec = usec; \
				else \
					_row->createdate.tv_usec = 0L; \
			} \
		} \
		item = optional_name("createby", 1, NULL); \
		if (item) \
			STRNCPY(_row->createby, DATA_TRANSFER(item)->data); \
		item = optional_name("createcode", 1, NULL); \
		if (item) \
			STRNCPY(_row->createcode, DATA_TRANSFER(item)->data); \
		item = optional_name("createinet", 1, NULL); \
		if (item) \
			STRNCPY(_row->createinet, DATA_TRANSFER(item)->data); \
	} while (0)

// For easy parameter constant strings
#define PQPARAM1  "$1"
#define PQPARAM2  "$1,$2"
#define PQPARAM3  "$1,$2,$3"
#define PQPARAM4  "$1,$2,$3,$4"
#define PQPARAM5  "$1,$2,$3,$4,$5"
#define PQPARAM6  "$1,$2,$3,$4,$5,$6"
#define PQPARAM7  "$1,$2,$3,$4,$5,$6,$7"
#define PQPARAM8  "$1,$2,$3,$4,$5,$6,$7,$8"
#define PQPARAM9  "$1,$2,$3,$4,$5,$6,$7,$8,$9"
#define PQPARAM10 "$1,$2,$3,$4,$5,$6,$7,$8,$9,$10"
#define PQPARAM11 "$1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11"
#define PQPARAM12 "$1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12"
#define PQPARAM13 "$1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12,$13"
#define PQPARAM14 "$1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12,$13,$14"
#define PQPARAM15 "$1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12,$13,$14,$15"
#define PQPARAM16 "$1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12,$13,$14,$15,$16"
#define PQPARAM17 "$1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12,$13,$14,$15,$16,$17"

#define PARCHK(_par, _params) do { \
		if (_par != (int)(sizeof(_params)/sizeof(_params[0]))) { \
			quithere(1, "params[] usage (%d) != size (%d)", \
					_par, (int)(sizeof(_params)/sizeof(_params[0]))); \
		} \
	} while (0)


static const char *userpatt = "^[!-~]*$"; // no spaces
static const char *mailpatt = "^[A-Za-z0-9_-][A-Za-z0-9_\\.-]*@[A-Za-z0-9][A-Za-z0-9\\.]*[A-Za-z0-9]$";
static const char *idpatt = "^[_A-Za-z][_A-Za-z0-9]*$";
static const char *intpatt = "^[0-9][0-9]*$";
static const char *hashpatt = "^[A-Fa-f0-9]*$";

#define JSON_TRANSFER "json="
#define JSON_TRANSFER_LEN (sizeof(JSON_TRANSFER)-1)

// JSON Methods
#define METHOD_WORKINFO "workinfo"
#define METHOD_SHARES "shares"
#define METHOD_SHAREERRORS "shareerror"
#define METHOD_AUTH "authorise"

// LOGFILE codes - should be in libckpool.h ... with the file logging code
#define CODE_WORKINFO "W"
#define CODE_SHARES "S"
#define CODE_SHAREERRORSS "E"

// TRANSFER
#define NAME_SIZE 63
#define VALUE_SIZE 1023
typedef struct transfer {
	char name[NAME_SIZE+1];
	char value[VALUE_SIZE+1];
	char *data;
} TRANSFER;

#define ALLOC_TRANSFER 1024
#define LIMIT_TRANSFER 0
#define DATA_TRANSFER(_item) ((TRANSFER *)(_item->data))

static K_TREE *transfer_root;
static K_LIST *transfer_list;
static K_STORE *transfer_store;

// USERS
typedef struct users {
	int64_t userid;
	char username[TXT_BIG+1];
	char emailaddress[TXT_BIG+1];
	tv_t joineddate;
	char passwordhash[TXT_BIG+1];
	char secondaryuserid[TXT_SML+1];
	HISTORYDATECONTROLFIELDS;
} USERS;

#define ALLOC_USERS 1024
#define LIMIT_USERS 0
#define DATA_USERS(_item) ((USERS *)(_item->data))

static K_TREE *users_root;
static K_TREE *userid_root;
static K_LIST *users_list;
static K_STORE *users_store;

/* TODO: for account settings - but do we want manual/auto payouts?
// USERACCOUNTS
typedef struct useraccounts {
	int64_t userid;
	int64_t payoutlimit;
	char autopayout[TXT_FLG+1];
	HISTORYDATECONTROLFIELDS;
} USERACCOUNTS;

#define ALLOC_USERACCOUNTS 1024
#define LIMIT_USERACCOUNTS 0
#define DATA_USERACCOUNTS(_item) ((USERACCOUNTS *)(_item->data))

static K_TREE *useraccounts_root;
static K_LIST *useraccounts_list;
static K_STORE *useraccounts_store;
*/

// WORKERS
typedef struct workers {
	int64_t workerid;
	int64_t userid;
	char workername[TXT_BIG+1]; // includes username
	int32_t difficultydefault;
	char idlenotificationenabled[TXT_FLAG+1];
	int32_t idlenotificationtime;
	HISTORYDATECONTROLFIELDS;
} WORKERS;

#define ALLOC_WORKERS 1024
#define LIMIT_WORKERS 0
#define DATA_WORKERS(_item) ((WORKERS *)(_item->data))

static K_TREE *workers_root;
static K_LIST *workers_list;
static K_STORE *workers_store;

#define STRINT(x) STRINT2(x)
#define STRINT2(x) #x

#define DIFFICULTYDEFAULT_MIN 10
#define DIFFICULTYDEFAULT_MAX 1000000
#define DIFFICULTYDEFAULT_DEF DIFFICULTYDEFAULT_MIN
#define DIFFICULTYDEFAULT_DEF_STR STRINT(DIFFICULTYDEFAULT_DEF)
#define IDLENOTIFICATIONENABLED "y"
#define IDLENOTIFICATIONDISABLED " "
#define IDLENOTIFICATIONENABLED_DEF IDLENOTIFICATIONDISABLED
#define IDLENOTIFICATIONTIME_MIN 10
#define IDLENOTIFICATIONTIME_MAX 60
#define IDLENOTIFICATIONTIME_DEF IDLENOTIFICATIONTIME_MIN
#define IDLENOTIFICATIONTIME_DEF_STR STRINT(IDLENOTIFICATIONTIME_DEF)

/* unused yet
// PAYMENTADDRESSES
typedef struct paymentaddresses {
	int64_t paymentaddressid;
	int64_t userid;
	char payaddress[TXT_BIG+1];
	int32_t payratio;
	HISTORYDATECONTROLFIELDS;
} PAYMENTADDRESSES;

#define ALLOC_PAYMENTADDRESSES 1024
#define LIMIT_PAYMENTADDRESSES 0
#define DATA_PAYMENTADDRESSES(_item) ((PAYMENTADDRESSES *)(_item->data))

static K_TREE *paymentaddresses_root;
static K_LIST *paymentaddresses_list;
static K_STORE *paymentaddresses_store;
*/

// PAYMENTS
typedef struct payments {
	int64_t paymentid;
	int64_t userid;
	tv_t paydate;
	char payaddress[TXT_BIG+1];
	char originaltxn[TXT_BIG+1];
	int64_t amount;
	char committxn[TXT_BIG+1];
	char commitblockhash[TXT_BIG+1];
	HISTORYDATECONTROLFIELDS;
} PAYMENTS;

#define ALLOC_PAYMENTS 1024
#define LIMIT_PAYMENTS 0
#define DATA_PAYMENTS(_item) ((PAYMENTS *)(_item->data))

static K_TREE *payments_root;
static K_LIST *payments_list;
static K_STORE *payments_store;

/* unused yet
// ACCOUNTBALANCE
typedef struct accountbalance {
	int64_t userid;
	int64_t confirmedpaid;
	int64_t confirmedunpaid;
	int64_t pendingconfirm;
	int32_t heightupdate;
	HISTORYDATECONTROLFIELDS;
} ACCOUNTBALANCE;

#define ALLOC_ACCOUNTBALANCE 1024
#define LIMIT_ACCOUNTBALANCE 0
#define DATA_ACCOUNTBALANCE(_item) ((ACCOUNTBALANCE *)(_item->data))

static K_TREE *accountbalance_root;
static K_LIST *accountbalance_list;
static K_STORE *accountbalance_store;

// ACCOUNTADJUSTMENT
typedef struct accountbalance {
	int64_t userid;
	char authority[TXT_BIG+1];
	char *reason;
	int64_t amount;
	HISTORYDATECONTROLFIELDS;
} ACCOUNTADJUSTMENT;

#define ALLOC_ACCOUNTADJUSTMENT 100
#define LIMIT_ACCOUNTADJUSTMENT 0
#define DATA_ACCOUNTADJUSTMENT(_item) ((ACCOUNTADJUSTMENT *)(_item->data))

static K_TREE *accountbalance_root;
static K_LIST *accountbalance_list;
static K_STORE *accountbalance_store;
*/

// IDCONTROL
typedef struct idcontrol {
	char idname[TXT_SML+1];
	int64_t lastid;
	MODIFYDATECONTROLFIELDS;
} IDCONTROL;

#define ALLOC_IDCONTROL 16
#define LIMIT_IDCONTROL 0
#define DATA_IDCONTROL(_item) ((IDCONTROL *)(_item->data))

// These are only used for db access - not stored in memory
//static K_TREE *idcontrol_root;
static K_LIST *idcontrol_list;
static K_STORE *idcontrol_store;

/* unused yet
// OPTIONCONTROL
typedef struct optioncontrol {
	char optionname[TXT_SML+1];
	char *optionvalue;
	tv_t activationdate;
	int32_t activationheight;
	HISTORYDATECONTROLFIELDS;
} OPTIONCONTROL;

#define ALLOC_OPTIONCONTROL 64
#define LIMIT_OPTIONCONTROL 0
#define DATA_OPTIONCONTROL(_item) ((OPTIONCONTROL *)(_item->data))

static K_TREE *optioncontrol_root;
static K_LIST *optioncontrol_list;
static K_STORE *optioncontrol_store;
*/

// TODO: aging/discarding workinfo,shares
// WORKINFO id.sharelog.json={...}
typedef struct workinfo {
	int64_t workinfoid;
	char poolinstance[TXT_BIG+1];
	char *transactiontree;
	char *merklehash;
	char prevhash[TXT_BIG+1];
	char coinbase1[TXT_BIG+1];
	char coinbase2[TXT_BIG+1];
	char version[TXT_SML+1];
	char bits[TXT_SML+1];
	char ntime[TXT_SML+1];
	int64_t reward;
	HISTORYDATECONTROLFIELDS;
} WORKINFO;

// ~10 hrs
#define ALLOC_WORKINFO 1400
#define LIMIT_WORKINFO 0
#define DATA_WORKINFO(_item) ((WORKINFO *)(_item->data))

static K_TREE *workinfo_root;
static K_LIST *workinfo_list;
static K_STORE *workinfo_store;

// SHARES id.sharelog.json={...}
typedef struct shares {
	int64_t workinfoid;
	int64_t userid;
	char workername[TXT_BIG+1];
	int32_t clientid;
	char enonce1[TXT_SML+1];
	char nonce2[TXT_BIG+1];
	char nonce[TXT_SML+1];
	double diff;
	double sdiff;
	int32_t errn;
	char error[TXT_SML+1];
	char secondaryuserid[TXT_SML+1];
	HISTORYDATECONTROLFIELDS;
} SHARES;

#define ALLOC_SHARES 10000
#define LIMIT_SHARES 0
#define DATA_SHARES(_item) ((SHARES *)(_item->data))

static K_TREE *shares_root;
static K_LIST *shares_list;
static K_STORE *shares_store;

// SHAREERRORS id.sharelog.json={...}
typedef struct shareerrorss {
	int64_t workinfoid;
	int64_t userid;
	char workername[TXT_BIG+1];
	int32_t clientid;
	int32_t errn;
	char error[TXT_SML+1];
	char secondaryuserid[TXT_SML+1];
	HISTORYDATECONTROLFIELDS;
} SHAREERRORS;

#define ALLOC_SHAREERRORS 10000
#define LIMIT_SHAREERRORS 0
#define DATA_SHAREERRORS(_item) ((SHAREERRORS *)(_item->data))

static K_TREE *shareerrors_root;
static K_LIST *shareerrors_list;
static K_STORE *shareerrors_store;

/*
// SHARESUMMARY
typedef struct sharesummary {
	int64_t userid;
	char workername[TXT_BIG+1];
	int64_t workinfoid;
	int64_t diff_acc;
	int64_t diff_sta;
	int64_t diff_dup;
	int64_t diff_low;
	int64_t diff_rej;
	int64_t share_acc;
	int64_t share_sta;
	int64_t share_dup;
	int64_t share_low;
	int64_t share_rej;
	tv_t first_share;
	tv_t last_share;
	char complete[TXT_FLAG+1];
	MODIFYDATECONTROLFIELDS;
} SHARESUMMARY;

#define ALLOC_SHARESUMMARY 10000
#define LIMIT_SHARESUMMARY 0
#define DATA_SHARESUMMARY(_item) ((SHARESUMMARY *)(_item->data))

static K_TREE *sharesummary_root;
static K_LIST *sharesummary_list;
static K_STORE *sharesummary_store;

// BLOCKSUMMARY
typedef struct blocksummary {
	int32_t height;
	char blockhash[TXT_BIG+1];
	int64_t userid;
	char workername[TXT_BIG+1];
	int64_t diff_acc;
	int64_t diff_sta;
	int64_t diff_dup;
	int64_t diff_low;
	int64_t diff_rej;
	int64_t share_acc;
	int64_t share_sta;
	int64_t share_dup;
	int64_t share_low;
	int64_t share_rej;
	tv_t first_share;
	tv_t last_share;
	char complete[TXT_FLAG+1];
	MODIFYDATECONTROLFIELDS;
} BLOCKSUMMARY;

#define ALLOC_BLOCKSUMMARY 10000
#define LIMIT_BLOCKSUMMARY 0
#define DATA_BLOCKSUMMARY(_item) ((BLOCKSUMMARY *)(_item->data))

static K_TREE *blocksummary_root;
static K_LIST *blocksummary_list;
static K_STORE *blocksummary_store;

// BLOCKS
typedef struct blocks {
	int32_t height;
	char blockhash[TXT_BIG+1];
	int64_t workinfoid;
	int64_t userid;
	char workername[TXT_BIG+1];
	int32_t clientid;
	char enonce1[TXT_SML+1];
	char nonce2[TXT_BIG+1];
	char nonce[TXT_SML+1];
	int64_t reward;
	char confirmed[TXT_FLAG+1];
	HISTORYDATECONTROLFIELDS;
} BLOCKS;

#define ALLOC_BLOCKS 10000
#define LIMIT_BLOCKS 0
#define DATA_BLOCKS ((BLOCKS *)(_item->data))

static K_TREE *blocks_root;
static K_LIST *blocks_list;
static K_STORE *blocks_store;

// MININGPAYOUTS
typedef struct miningpayouts {
	int64_t miningpayoutid;
	int64_t userid;
	int32_t height;
	char blockhash[TXT_BIG+1];
	int64_t amount;
	HISTORYDATECONTROLFIELDS;
} MININGPAYOUTS;

#define ALLOC_MININGPAYOUTS 1000
#define LIMIT_MININGPAYOUTS 0
#define DATA_MININGPAYOUTS(_item) ((MININGPAYOUTS *)(_item->data))

static K_TREE *miningpayouts_root;
static K_LIST *miningpayouts_list;
static K_STORE *miningpayouts_store;

// EVENTLOG
typedef struct eventlog {
	int64_t eventlogid;
	char eventlogcode[TXT_SML+1];
	char *eventlogdescription;
	HISTORYDATECONTROLFIELDS;
} EVENTLOG;

#define ALLOC_EVENTLOG 100
#define LIMIT_EVENTLOG 0
#define DATA_EVENTLOG(_item) ((EVENTLOG *)(_item->data))

static K_TREE *eventlog_root;
static K_LIST *eventlog_list;
static K_STORE *eventlog_store;
*/

// AUTHS
typedef struct auths {
	int64_t authid;
	int64_t userid;
	char workername[TXT_BIG+1];
	int32_t clientid;
	char enonce1[TXT_SML+1];
	char useragent[TXT_BIG+1];
	HISTORYDATECONTROLFIELDS;
} AUTHS;

#define ALLOC_AUTHS 1000
#define LIMIT_AUTHS 0
#define DATA_AUTHS(_item) ((AUTHS *)(_item->data))

static K_TREE *auths_root;
static K_LIST *auths_list;
static K_STORE *auths_store;

// POOLSTATS
// TODO: get every 1m: pool sending it
// so web page is kept up to date

// Store every > 9.5m?
#define STATS_PER (9.5*60.0)

typedef struct poolstats {
	char poolinstance[TXT_BIG+1];
	tv_t when;
	int32_t users;
	int32_t workers;
	double hashrate;
	double hashrate5m;
	double hashrate1hr;
	double hashrate24hr;
	SIMPLEDATECONTROLFIELDS;
} POOLSTATS;

#define ALLOC_POOLSTATS 10000
#define LIMIT_POOLSTATS 0
#define DATA_POOLSTATS(_item) ((POOLSTATS *)(_item->data))

static K_TREE *poolstats_root;
static K_LIST *poolstats_list;
static K_STORE *poolstats_store;

static void setnow(tv_t *now)
{
	ts_t spec;
	spec.tv_sec = 0;
	spec.tv_nsec = 0;
	clock_gettime(CLOCK_REALTIME, &spec);
	now->tv_sec = spec.tv_sec;
	now->tv_usec = spec.tv_nsec / 1000;
}

static double cmp_transfer(K_ITEM *a, K_ITEM *b)
{
	double c = (double)strcmp(DATA_TRANSFER(a)->name,
				  DATA_TRANSFER(b)->name);
	return c;
}

static K_ITEM *find_transfer(char *name)
{
	TRANSFER transfer;
	K_TREE_CTX ctx[1];
	K_ITEM look;

	STRNCPY(transfer.name, name);
	look.data = (void *)(&transfer);
	return find_in_ktree(transfer_root, &look, cmp_transfer, ctx);
}

static K_ITEM *optional_name(char *name, int len, char *patt)
{
	K_ITEM *item;
	char *value;
	regex_t re;
	int ret;

	item = find_transfer(name);
	if (!item)
		return NULL;

	value = DATA_TRANSFER(item)->data;
	if (!*value || (int)strlen(value) < len)
		return NULL;

	if (patt) {
		if (regcomp(&re, patt, REG_NOSUB) != 0)
			return NULL;

		ret = regexec(&re, value, (size_t)0, NULL, 0);
		regfree(&re);

		if (ret != 0)
			return NULL;
	}

	return item;
}

static K_ITEM *require_name(char *name, int len, char *patt, char *reply, size_t siz)
{
	K_ITEM *item;
	char *value;
	regex_t re;
	int ret;

	item = find_transfer(name);
	if (!item) {
		snprintf(reply, siz, "failed.missing %s", name);
		return NULL;
	}

	value = DATA_TRANSFER(item)->data;
	if (!*value || (int)strlen(value) < len) {
		snprintf(reply, siz, "failed.short %s", name);
		return NULL;
	}

	if (patt) {
		if (regcomp(&re, patt, REG_NOSUB) != 0) {
			snprintf(reply, siz, "failed.REC %s", name);
			return NULL;
		}

		ret = regexec(&re, value, (size_t)0, NULL, 0);
		regfree(&re);

		if (ret != 0) {
			snprintf(reply, siz, "failed.invalid %s", name);
			return NULL;
		}
	}

	return item;
}

static void txt_to_data(enum data_type typ, char *nam, char *fld, void *data, size_t siz)
{
	char *tmp;

	switch (typ) {
		case TYPE_STR:
			// A database field being bigger than local storage is a fatal error
			if (siz < (strlen(fld)+1)) {
				quithere(1, "Field %s structure size %d is smaller than db %d",
						nam, (int)siz, (int)strlen(fld)+1);
			}
			strcpy((char *)data, fld);
			break;
		case TYPE_BIGINT:
			if (siz != sizeof(int64_t)) {
				quithere(1, "Field %s bigint incorrect structure size %d - should be %d",
						nam, (int)siz, (int)sizeof(int64_t));
			}
			*((long long *)data) = atoll(fld);
			break;
		case TYPE_INT:
			if (siz != sizeof(int32_t)) {
				quithere(1, "Field %s int incorrect structure size %d - should be %d",
						nam, (int)siz, (int)sizeof(int32_t));
			}
			*((int32_t *)data) = atoi(fld);
			break;
		case TYPE_TV:
			if (siz != sizeof(tv_t)) {
				quithere(1, "Field %s timeval incorrect structure size %d - should be %d",
						nam, (int)siz, (int)sizeof(tv_t));
			}
			unsigned int yyyy, mm, dd, HH, MM, SS, uS = 0, tz;
			struct tm tm;
			time_t tim;
			int n;
			n = sscanf(fld, "%u-%u-%u %u:%u:%u+%u",
					&yyyy, &mm, &dd, &HH, &MM, &SS, &tz);
			if (n != 7) {
				// allow uS
				n = sscanf(fld, "%u-%u-%u %u:%u:%u.%u+%u",
						&yyyy, &mm, &dd, &HH, &MM, &SS, &uS, &tz);
				if (n != 8) {
					quithere(1, "Field %s timeval unhandled date '%s' (%d)",
						 nam, fld, n);
				}
			}
			tm.tm_sec = (int)SS;
			tm.tm_min = (int)MM;
			tm.tm_hour = (int)HH;
			tm.tm_mday = (int)dd;
			tm.tm_mon = (int)mm - 1;
			tm.tm_year = (int)yyyy - 1900;
			tm.tm_isdst = -1;
			tim = mktime(&tm);
			// Fix TZ offsets errors
			if (tim > COMPARE_EXPIRY) {
				((tv_t *)data)->tv_sec = default_expiry.tv_sec;
				((tv_t *)data)->tv_usec = default_expiry.tv_usec;
			} else {
				((tv_t *)data)->tv_sec = tim;
				((tv_t *)data)->tv_usec = uS;
			}
			break;
		case TYPE_BLOB:
			tmp = strdup(fld);
			if (!tmp)
				quithere(1, "Field %s (%d) OOM", nam, (int)strlen(fld));
			*((char **)data) = tmp;
			break;
		case TYPE_DOUBLE:
			if (siz != sizeof(double)) {
				quithere(1, "Field %s int incorrect structure size %d - should be %d",
						nam, (int)siz, (int)sizeof(double));
			}
			*((double *)data) = atof(fld);
			break;
		default:
			quithere(1, "Unknown field %s (%d) to convert", nam, (int)typ);
			break;
	}
}

// N.B. STRNCPY* macros truncate, whereas this aborts ckdb if src > trg
static void txt_to_str(char *nam, char *fld, char data[], size_t siz)
{
	txt_to_data(TYPE_STR, nam, fld, (void *)data, siz);
}

static void txt_to_bigint(char *nam, char *fld, int64_t *data, size_t siz)
{
	txt_to_data(TYPE_BIGINT, nam, fld, (void *)data, siz);
}

static void txt_to_int(char *nam, char *fld, int32_t *data, size_t siz)
{
	txt_to_data(TYPE_INT, nam, fld, (void *)data, siz);
}

static void txt_to_tv(char *nam, char *fld, tv_t *data, size_t siz)
{
	txt_to_data(TYPE_TV, nam, fld, (void *)data, siz);
}

static void txt_to_blob(char *nam, char *fld, char *data)
{
	txt_to_data(TYPE_BLOB, nam, fld, (void *)(&data), 0);
}

static void txt_to_double(char *nam, char *fld, double *data, size_t siz)
{
	txt_to_data(TYPE_DOUBLE, nam, fld, (void *)data, siz);
}

static char *data_to_buf(enum data_type typ, void *data, char *buf, size_t siz)
{
	struct tm tm;
	char *buf2;

	if (!buf) {
		switch (typ) {
			case TYPE_STR:
			case TYPE_BLOB:
				siz = strlen((char *)data) + 1;
				break;
			case TYPE_BIGINT:
			case TYPE_INT:
			case TYPE_TV:
			case TYPE_DOUBLE:
				siz = 64; // More than big enough
				break;
			default:
				quithere(1, "Unknown field (%d) to convert", (int)typ);
				break;
		}

		buf = malloc(siz);
		if (!buf)
			quithere(1, "OOM (%d)", (int)siz);
	}

	switch (typ) {
		case TYPE_STR:
		case TYPE_BLOB:
			snprintf(buf, siz, "%s", (char *)data);
			break;
		case TYPE_BIGINT:
			snprintf(buf, siz, "%"PRId64, *((uint64_t *)data));
			break;
		case TYPE_INT:
			snprintf(buf, siz, "%"PRId32, *((uint32_t *)data));
			break;
		case TYPE_TV:
			buf2 = malloc(siz);
			if (!buf2)
				quithere(1, "OOM (%d)", (int)siz);
			localtime_r(&(((struct timeval *)data)->tv_sec), &tm);
			strftime(buf2, siz, "%Y-%m-%d %H:%M:%S", &tm);
			snprintf(buf, siz, "%s.%06ld", buf2,
					   (((struct timeval *)data)->tv_usec));
			free(buf2);
			break;
		case TYPE_DOUBLE:
			snprintf(buf, siz, "%f", *((double *)data));
			break;
	}

	return buf;
}

static char *str_to_buf(char data[], char *buf, size_t siz)
{
	return data_to_buf(TYPE_STR, (void *)data, buf, siz);
}

static char *bigint_to_buf(int64_t data, char *buf, size_t siz)
{
	return data_to_buf(TYPE_BIGINT, (void *)(&data), buf, siz);
}

static char *int_to_buf(int32_t data, char *buf, size_t siz)
{
	return data_to_buf(TYPE_INT, (void *)(&data), buf, siz);
}

static char *tv_to_buf(tv_t *data, char *buf, size_t siz)
{
	return data_to_buf(TYPE_TV, (void *)data, buf, siz);
}

/* unused yet
static char *blob_to_buf(char *data, char *buf, size_t siz)
{
	return data_to_buf(TYPE_BLOB, (void *)data, buf, siz);
}

static char *double_to_buf(double data, char *buf, size_t siz)
{
	return data_to_buf(TYPE_DOUBLE, (void *)(&data), buf, siz);
}
*/

static PGconn *dbconnect()
{
	char conninfo[128];
	PGconn *conn;

	snprintf(conninfo, sizeof(conninfo), "host=127.0.0.1 dbname=ckdb user=%s", db_user);

	conn = PQconnectdb(conninfo);
	if (PQstatus(conn) != CONNECTION_OK)
		quithere(1, "ERR: Failed to connect to db '%s'", PQerrorMessage(conn));

	return conn;
}

static int64_t nextid(PGconn *conn, char *idname, int64_t increment,
			tv_t *now, char *by, char *code, char *inet)
{
	ExecStatusType rescode;
	PGresult *res;
	char qry[1024];
	char *params[5];
	int par;
	int64_t lastid;
	char *field;
	bool ok;
	int n;

	lastid = 0;

	snprintf(qry, sizeof(qry), "select lastid from idcontrol "
				   "where idname='%s' for update",
				   idname);

	res = PQexec(conn, qry);
	rescode = PQresultStatus(res);
	if (!PGOK(rescode)) {
		PGLOGERR("Select", rescode, conn);
		goto cleanup;
	}

	n = PQnfields(res);
	if (n != 1) {
		LOGERR("%s(): Invalid field count - should be %d, but is %d",
			__func__, 1, n);
		goto cleanup;
	}

	n = PQntuples(res);
	if (n < 1) {
		LOGERR("%s(): No matching idname='%s'", __func__, idname);
		goto cleanup;
	}

	ok = true;
	PQ_GET_FLD(res, 0, "lastid", field, ok);
	if (!ok)
		goto cleanup;
	TXT_TO_BIGINT("lastid", field, lastid);

	PQclear(res);

	lastid += increment;
	snprintf(qry, sizeof(qry), "update idcontrol set "
				   "lastid=$1, modifydate=$2, modifyby=$3, "
				   "modifycode=$4, modifyinet=$5 "
				   "where idname='%s'", 
				   idname);

	par = 0;
	params[par++] = bigint_to_buf(lastid, NULL, 0);
	params[par++] = tv_to_buf(now, NULL, 0);
	params[par++] = str_to_buf(by, NULL, 0);
	params[par++] = str_to_buf(code, NULL, 0);
	params[par++] = str_to_buf(inet, NULL, 0);
	PARCHK(par, params);

	res = PQexecParams(conn, qry, par, NULL, (const char **)params, NULL, NULL, 0);
	rescode = PQresultStatus(res);
	if (!PGOK(rescode)) {
		PGLOGERR("Update", rescode, conn);
		lastid = 0;
	}

	for (n = 0; n < par; n++)
		free(params[n]);
cleanup:
	PQclear(res);
	return lastid;
}

// default tree order by username asc,expirydate desc
static double cmp_users(K_ITEM *a, K_ITEM *b)
{
	double c = strcmp(DATA_USERS(a)->username,
			  DATA_USERS(b)->username);
	if (c == 0.0) {
		c = tvdiff(&(DATA_USERS(b)->expirydate),
			   &(DATA_USERS(a)->expirydate));
	}
	return c;
}

// order by userid asc,expirydate desc
static double cmp_userid(K_ITEM *a, K_ITEM *b)
{
	double c = (double)(DATA_USERS(a)->userid) -
		   (double)(DATA_USERS(b)->userid);
	if (c == 0.0) {
		c = tvdiff(&(DATA_USERS(b)->expirydate),
			   &(DATA_USERS(a)->expirydate));
	}
	return c;
}

static K_ITEM *find_users(char *username)
{
	USERS users;
	K_TREE_CTX ctx[1];
	K_ITEM look;

	STRNCPY(users.username, username);
	users.expirydate.tv_sec = default_expiry.tv_sec;
	users.expirydate.tv_usec = default_expiry.tv_usec;

	look.data = (void *)(&users);
	return find_in_ktree(users_root, &look, cmp_users, ctx);
}

/* unused
static K_ITEM *find_userid(int64_t userid)
{
	USERS users;
	K_TREE_CTX ctx[1];
	K_ITEM look;

	users.userid = userid;
	users.expirydate.tv_sec = default_expiry.tv_sec;
	users.expirydate.tv_usec = default_expiry.tv_usec;

	look.data = (void *)(&users);
	return find_in_ktree(userid_root, &look, cmp_userid, ctx);
}
*/

static bool users_add(PGconn *conn, char *username, char *emailaddress, char *passwordhash,
			tv_t *now, char *by, char *code, char *inet)
{
	ExecStatusType rescode;
	PGresult *res;
	K_ITEM *item;
	int n;
	USERS *row;
	char *ins;
	char tohash[64];
	uint64_t hash;
	__maybe_unused uint64_t tmp;
	bool ok = false;
	char *params[6 + HISTORYDATECOUNT];
	int par;

	LOGDEBUG("%s(): add", __func__);

	K_WLOCK(users_list);
	item = k_unlink_head(users_list);
	K_WUNLOCK(users_list);

	row = DATA_USERS(item);

	row->userid = nextid(conn, "userid", (int64_t)(666 + (rand() % 334)),
				now, by, code, inet);
	if (row->userid == 0)
		goto unitem;

	// TODO: pre-check the username exists? (to save finding out via a DB error)

	STRNCPY(row->username, username);
	STRNCPY(row->emailaddress, emailaddress);
	STRNCPY(row->passwordhash, passwordhash);

	snprintf(tohash, sizeof(tohash), "%s&#%s", username, emailaddress);
	HASH_BER(tohash, strlen(tohash), 1, hash, tmp);
	__bin2hex(row->secondaryuserid, (void *)(&hash), sizeof(hash));

	HISTORYDATEINIT(row, now, by, code, inet);

	// copy createdate
	row->joineddate.tv_sec = row->createdate.tv_sec;
	row->joineddate.tv_usec = row->createdate.tv_usec;

	par = 0;
	params[par++] = bigint_to_buf(row->userid, NULL, 0);
	params[par++] = str_to_buf(row->username, NULL, 0);
	params[par++] = str_to_buf(row->emailaddress, NULL, 0);
	params[par++] = tv_to_buf(&(row->joineddate), NULL, 0);
	params[par++] = str_to_buf(row->passwordhash, NULL, 0);
	params[par++] = str_to_buf(row->secondaryuserid, NULL, 0);
	HISTORYDATEPARAMS(params, par, row);
	PARCHK(par, params);

	ins = "insert into users "
		"(userid,username,emailaddress,joineddate,passwordhash,"
		"secondaryuserid"
		HISTORYDATECONTROL ") values (" PQPARAM11 ")";

	res = PQexecParams(conn, ins, par, NULL, (const char **)params, NULL, NULL, 0);
	rescode = PQresultStatus(res);
	if (!PGOK(rescode)) {
		PGLOGERR("Insert", rescode, conn);
		goto unparam;
	}

	ok = true;
unparam:
	PQclear(res);
	for (n = 0; n < par; n++)
		free(params[n]);
unitem:
	K_WLOCK(users_list);
	if (!ok)
		k_add_head(users_list, item);
	else {
		users_root = add_to_ktree(users_root, item, cmp_users);
		userid_root = add_to_ktree(userid_root, item, cmp_userid);
		k_add_head(users_store, item);
	}
	K_WUNLOCK(users_list);

	return ok;
}

static bool users_fill(PGconn *conn)
{
	ExecStatusType rescode;
	PGresult *res;
	K_ITEM *item;
	int n, i;
	USERS *row;
	char *field;
	char *sel;
	int fields = 6;
	bool ok;

	LOGDEBUG("%s(): select", __func__);

	sel = "select "
		"userid,username,emailaddress,joineddate,passwordhash,"
		"secondaryuserid"
		HISTORYDATECONTROL
		" from users";
	res = PQexec(conn, sel);
	rescode = PQresultStatus(res);
	if (!PGOK(rescode)) {
		PGLOGERR("Select", rescode, conn);
		PQclear(res);
		return false;
	}

	n = PQnfields(res);
	if (n != (fields + HISTORYDATECOUNT)) {
		LOGERR("%s(): Invalid field count - should be %d, but is %d",
			__func__, fields + HISTORYDATECOUNT, n);
		PQclear(res);
		return false;
	}

	n = PQntuples(res);
	LOGDEBUG("%s(): tree build count %d", __func__, n);
	ok = true;
	K_WLOCK(users_list);
	for (i = 0; i < n; i++) {
		item = k_unlink_head(users_list);
		row = DATA_USERS(item);

		PQ_GET_FLD(res, i, "userid", field, ok);
		if (!ok)
			break;
		TXT_TO_BIGINT("userid", field, row->userid);

		PQ_GET_FLD(res, i, "username", field, ok);
		if (!ok)
			break;
		TXT_TO_STR("username", field, row->username);

		PQ_GET_FLD(res, i, "emailaddress", field, ok);
		if (!ok)
			break;
		TXT_TO_STR("emailaddress", field, row->emailaddress);

		PQ_GET_FLD(res, i, "joineddate", field, ok);
		if (!ok)
			break;
		TXT_TO_TV("joineddate", field, row->joineddate);

		PQ_GET_FLD(res, i, "passwordhash", field, ok);
		if (!ok)
			break;
		TXT_TO_STR("passwordhash", field, row->passwordhash);

		PQ_GET_FLD(res, i, "secondaryuserid", field, ok);
		if (!ok)
			break;
		TXT_TO_STR("secondaryuserid", field, row->secondaryuserid);

		HISTORYDATEFLDS(res, i, row, ok);
		if (!ok)
			break;

		users_root = add_to_ktree(users_root, item, cmp_users);
		userid_root = add_to_ktree(userid_root, item, cmp_userid);
		k_add_head(users_store, item);
	}
	if (!ok)
		k_add_head(users_list, item);

	K_WUNLOCK(users_list);
	PQclear(res);

	if (ok)
		LOGDEBUG("%s(): built", __func__);

	return true;
}

void users_reload()
{
	PGconn *conn = dbconnect();

	K_WLOCK(users_list);
	users_root = free_ktree(users_root, NULL);
	userid_root = free_ktree(userid_root, NULL);
	k_list_transfer_to_head(users_store, users_list);
	K_WUNLOCK(users_list);

	users_fill(conn);

	PQfinish(conn);
}

// order by userid asc,workername asc,expirydate desc
static double cmp_workers(K_ITEM *a, K_ITEM *b)
{
	double c = (double)(DATA_WORKERS(a)->userid) -
		   (double)(DATA_WORKERS(b)->userid);
	if (c == 0.0) {
		c = strcmp(DATA_WORKERS(a)->workername,
			   DATA_WORKERS(b)->workername);
		if (c == 0.0) {
			c = tvdiff(&(DATA_WORKERS(b)->expirydate),
				   &(DATA_WORKERS(a)->expirydate));
		}
	}
	return c;
}

static K_ITEM *find_workers(int64_t userid, char *workername)
{
	WORKERS workers;
	K_TREE_CTX ctx[1];
	K_ITEM look;

	workers.userid = userid;
	STRNCPY(workers.workername, workername);
	workers.expirydate.tv_sec = default_expiry.tv_sec;
	workers.expirydate.tv_usec = default_expiry.tv_usec;

	look.data = (void *)(&workers);
	return find_in_ktree(workers_root, &look, cmp_workers, ctx);
}

static K_ITEM *workers_add(PGconn *conn, int64_t userid, char *workername,
			   char *difficultydefault, char *idlenotificationenabled,
			   char *idlenotificationtime, tv_t *now, char *by,
			   char *code, char *inet)
{
	ExecStatusType rescode;
	PGresult *res;
	K_ITEM *item, *ret = NULL;
	int n;
	WORKERS *row;
	char *ins;
	char *params[6 + HISTORYDATECOUNT];
	int par;
	int32_t diffdef;
	int32_t nottime;

	LOGDEBUG("%s(): add", __func__);

	K_WLOCK(workers_list);
	item = k_unlink_head(workers_list);
	K_WUNLOCK(workers_list);

	row = DATA_WORKERS(item);

	row->workerid = nextid(conn, "workerid", (int64_t)1, now, by, code, inet);
	if (row->workerid == 0)
		goto unitem;

	row->userid = userid;
	STRNCPY(row->workername, workername);
	if (difficultydefault && *difficultydefault) {
		diffdef = atoi(difficultydefault);
		if (diffdef < DIFFICULTYDEFAULT_MIN)
			diffdef = DIFFICULTYDEFAULT_MIN;
		if (diffdef > DIFFICULTYDEFAULT_MAX)
			diffdef = DIFFICULTYDEFAULT_MAX;
		row->difficultydefault = diffdef;
	} else
		row->difficultydefault = DIFFICULTYDEFAULT_DEF;

	row->idlenotificationenabled[1] = '\0';
	if (idlenotificationenabled && *idlenotificationenabled) {
		if (tolower(*idlenotificationenabled) == IDLENOTIFICATIONENABLED[0])
			row->idlenotificationenabled[0] = IDLENOTIFICATIONENABLED[0];
		else
			row->idlenotificationenabled[0] = IDLENOTIFICATIONDISABLED[0];
	} else
		row->idlenotificationenabled[0] = IDLENOTIFICATIONENABLED_DEF[0];

	if (idlenotificationtime && *idlenotificationtime) {
		nottime = atoi(idlenotificationtime);
		if (nottime < DIFFICULTYDEFAULT_MIN) {
			row->idlenotificationenabled[0] = IDLENOTIFICATIONDISABLED[0];
			nottime = DIFFICULTYDEFAULT_MIN;
		} else if (nottime > IDLENOTIFICATIONTIME_MAX)
			nottime = row->idlenotificationtime;
		row->idlenotificationtime = nottime;
	} else
		row->idlenotificationtime = IDLENOTIFICATIONTIME_DEF;

	HISTORYDATEINIT(row, now, by, code, inet);

	par = 0;
	params[par++] = bigint_to_buf(row->workerid, NULL, 0);
	params[par++] = bigint_to_buf(row->userid, NULL, 0);
	params[par++] = str_to_buf(row->workername, NULL, 0);
	params[par++] = int_to_buf(row->difficultydefault, NULL, 0);
	params[par++] = str_to_buf(row->idlenotificationenabled, NULL, 0);
	params[par++] = int_to_buf(row->idlenotificationtime, NULL, 0);
	HISTORYDATEPARAMS(params, par, row);
	PARCHK(par, params);

	ins = "insert into workers "
		"(workerid,userid,workername,difficultydefault,"
		"idlenotificationenabled,idlenotificationtime"
		HISTORYDATECONTROL ") values (" PQPARAM11 ")";

	res = PQexecParams(conn, ins, par, NULL, (const char **)params, NULL, NULL, 0);
	rescode = PQresultStatus(res);
	if (!PGOK(rescode)) {
		PGLOGERR("Insert", rescode, conn);
		goto unparam;
	}

	ret = item;
unparam:
	PQclear(res);
	for (n = 0; n < par; n++)
		free(params[n]);
unitem:
	K_WLOCK(workers_list);
	if (!ret)
		k_add_head(workers_list, item);
	else {
		workers_root = add_to_ktree(workers_root, item, cmp_workers);
		k_add_head(workers_store, item);
	}
	K_WUNLOCK(workers_list);

	return ret;
}

static bool workers_update(PGconn *conn, K_ITEM *item, char *difficultydefault,
			   char *idlenotificationenabled, char *idlenotificationtime,
			   tv_t *now, char *by, char *code, char *inet)
{
	ExecStatusType rescode;
	PGresult *res;
	int n;
	WORKERS *row;
	char *upd, *ins;
	bool ok = false;
	char *params[6 + HISTORYDATECOUNT];
	int par;
	int32_t diffdef;
	char idlenot;
	int32_t nottime;

	LOGDEBUG("%s(): update", __func__);

	row = DATA_WORKERS(item);

	if (difficultydefault && *difficultydefault) {
		diffdef = atoi(difficultydefault);
		if (diffdef < DIFFICULTYDEFAULT_MIN)
			diffdef = row->difficultydefault;
		if (diffdef > DIFFICULTYDEFAULT_MAX)
			diffdef = row->difficultydefault;
	} else
		diffdef = row->difficultydefault;

	if (idlenotificationenabled && *idlenotificationenabled) {
		if (tolower(*idlenotificationenabled) == IDLENOTIFICATIONENABLED[0])
			idlenot = IDLENOTIFICATIONENABLED[0];
		else
			idlenot = IDLENOTIFICATIONDISABLED[0];
	} else
		idlenot = row->idlenotificationenabled[0];

	if (idlenotificationtime && *idlenotificationtime) {
		nottime = atoi(idlenotificationtime);
		if (nottime < IDLENOTIFICATIONTIME_MIN)
			nottime = row->idlenotificationtime;
		if (nottime > IDLENOTIFICATIONTIME_MAX)
			nottime = row->idlenotificationtime;
	} else
		nottime = row->idlenotificationtime;

	HISTORYDATEINIT(row, now, by, code, inet);

	if (diffdef != row->difficultydefault ||
	    idlenot != row->idlenotificationenabled[0] ||
	    nottime != row->idlenotificationtime) {

		upd = "update workers set expirydate=$1 where workerid=$2 and expirydate=$3";
		par = 0;
		params[par++] = tv_to_buf(now, NULL, 0);
		params[par++] = bigint_to_buf(row->workerid, NULL, 0);
		params[par++] = tv_to_buf((tv_t *)&default_expiry, NULL, 0);
		// Not the full size of params[] so no PARCHK()

		res = PQexec(conn, "Begin");
		rescode = PQresultStatus(res);
		if (!PGOK(rescode)) {
			PGLOGERR("Begin", rescode, conn);
			PQclear(res);
			goto unparam;
		}
		PQclear(res);

		res = PQexecParams(conn, upd, par, NULL, (const char **)params, NULL, NULL, 0);
		rescode = PQresultStatus(res);
		PQclear(res);
		if (!PGOK(rescode)) {
			PGLOGERR("Insert", rescode, conn);
			res = PQexec(conn, "Rollback");
			PQclear(res);
			goto unparam;
		}

		for (n = 0; n < par; n++)
			free(params[n]);

		ins = "insert into workers "
			"(workerid,userid,workername,difficultydefault,"
			"idlenotificationenabled,idlenotificationtime"
			HISTORYDATECONTROL ") values (" PQPARAM11 ")";

		row->difficultydefault = diffdef;
		row->idlenotificationenabled[0] = idlenot;
		row->idlenotificationenabled[1] = '\0';
		row->idlenotificationtime = nottime;

		par = 0;
		params[par++] = bigint_to_buf(row->workerid, NULL, 0);
		params[par++] = bigint_to_buf(row->userid, NULL, 0);
		params[par++] = str_to_buf(row->workername, NULL, 0);
		params[par++] = int_to_buf(row->difficultydefault, NULL, 0);
		params[par++] = str_to_buf(row->idlenotificationenabled, NULL, 0);
		params[par++] = int_to_buf(row->idlenotificationtime, NULL, 0);
		HISTORYDATEPARAMS(params, par, row);
		// This one should be the full size
		PARCHK(par, params);

		res = PQexecParams(conn, ins, par, NULL, (const char **)params, NULL, NULL, 0);
		rescode = PQresultStatus(res);
		PQclear(res);
		if (!PGOK(rescode)) {
			PGLOGERR("Insert", rescode, conn);
			res = PQexec(conn, "Rollback");
			PQclear(res);
			goto unparam;
		}

		res = PQexec(conn, "Commit");
		PQclear(res);
	}

	ok = true;
unparam:
	for (n = 0; n < par; n++)
		free(params[n]);

	return ok;
}

static K_ITEM *new_worker(PGconn *conn, bool update, int64_t userid, char *workername,
			  char *diffdef, char *idlenotificationenabled,
			  char *idlenotificationtime, tv_t *now, char *by,
			  char *code, char *inet)
{
	K_ITEM *item;

	item = find_workers(userid, workername);
	if (item) {
		if (update) {
			workers_update(conn, item, diffdef, idlenotificationenabled,
				       idlenotificationtime, now, by, code, inet);
		}
	} else {
		item = workers_add(conn, userid, workername, diffdef,
				   idlenotificationenabled, idlenotificationtime,
				   now, by, code, inet);
	}
	return item;
}

/* unused
static K_ITEM *new_worker_find_user(PGconn *conn, bool update, char *username,
				    char *workername, char *diffdef,
				    char *idlenotificationenabled,
				    char *idlenotificationtime, tv_t *now,
				    char *by, char *code, char *inet)
{
	K_ITEM *item;

	item = find_users(username);
	if (!item)
		return NULL;

	return new_worker(conn, update, DATA_USERS(item)->userid, workername,
			  diffdef, idlenotificationenabled,
			  idlenotificationtime, now, by, code, inet);
}
*/

static bool workers_fill(PGconn *conn)
{
	ExecStatusType rescode;
	PGresult *res;
	K_ITEM *item;
	int n, i;
	WORKERS *row;
	char *field;
	char *sel;
	int fields = 6;
	bool ok;

	LOGDEBUG("%s(): select", __func__);

	sel = "select "
		"userid,workername,difficultydefault,"
		"idlenotificationenabled,idlenotificationtime"
		HISTORYDATECONTROL
		",workerid from workers";
	res = PQexec(conn, sel);
	rescode = PQresultStatus(res);
	if (!PGOK(rescode)) {
		PGLOGERR("Select", rescode, conn);
		PQclear(res);
		return false;
	}

	n = PQnfields(res);
	if (n != (fields + HISTORYDATECOUNT)) {
		LOGERR("%s(): Invalid field count - should be %d, but is %d",
			__func__, fields + HISTORYDATECOUNT, n);
		PQclear(res);
		return false;
	}

	n = PQntuples(res);
	LOGDEBUG("%s(): tree build count %d", __func__, n);
	ok = true;
	K_WLOCK(workers_list);
	for (i = 0; i < n; i++) {
		item = k_unlink_head(workers_list);
		row = DATA_WORKERS(item);

		PQ_GET_FLD(res, i, "userid", field, ok);
		if (!ok)
			break;
		TXT_TO_BIGINT("userid", field, row->userid);

		PQ_GET_FLD(res, i, "workername", field, ok);
		if (!ok)
			break;
		TXT_TO_STR("workername", field, row->workername);

		PQ_GET_FLD(res, i, "difficultydefault", field, ok);
		if (!ok)
			break;
		TXT_TO_INT("difficultydefault", field, row->difficultydefault);

		PQ_GET_FLD(res, i, "idlenotificationenabled", field, ok);
		if (!ok)
			break;
		TXT_TO_STR("idlenotificationenabled", field, row->idlenotificationenabled);

		PQ_GET_FLD(res, i, "idlenotificationtime", field, ok);
		if (!ok)
			break;
		TXT_TO_INT("idlenotificationtime", field, row->idlenotificationtime);

		HISTORYDATEFLDS(res, i, row, ok);
		if (!ok)
			break;

		PQ_GET_FLD(res, i, "workerid", field, ok);
		if (!ok)
			break;
		TXT_TO_BIGINT("workerid", field, row->workerid);

		workers_root = add_to_ktree(workers_root, item, cmp_workers);
		k_add_head(workers_store, item);
	}
	if (!ok)
		k_add_head(workers_list, item);

	K_WUNLOCK(workers_list);
	PQclear(res);

	if (ok)
		LOGDEBUG("%s(): built", __func__);

	return true;
}

void workers_reload()
{
	PGconn *conn = dbconnect();

	K_WLOCK(workers_list);
	workers_root = free_ktree(workers_root, NULL);
	k_list_transfer_to_head(workers_store, workers_list);
	K_WUNLOCK(workers_list);

	workers_fill(conn);

	PQfinish(conn);
}

// order by userid asc,paydate asc,payaddress asc,expirydate desc
static double cmp_payments(K_ITEM *a, K_ITEM *b)
{
	double c = (double)(DATA_PAYMENTS(a)->userid) -
		   (double)(DATA_PAYMENTS(b)->userid);
	if (c == 0.0) {
		c = tvdiff(&(DATA_PAYMENTS(a)->paydate),
			   &(DATA_PAYMENTS(b)->paydate));
		if (c == 0.0) {
			c = strcmp(DATA_PAYMENTS(a)->payaddress,
				   DATA_PAYMENTS(b)->payaddress);
			if (c == 0.0) {
				c = tvdiff(&(DATA_PAYMENTS(b)->expirydate),
					   &(DATA_PAYMENTS(a)->expirydate));
			}
		}
	}
	return c;
}

static bool payments_fill(PGconn *conn)
{
	ExecStatusType rescode;
	PGresult *res;
	K_ITEM *item;
	int n, i;
	PAYMENTS *row;
	char *params[1];
	int par;
	char *field;
	char *sel;
	int fields = 8;
	bool ok;

	LOGDEBUG("%s(): select", __func__);

	// TODO: handle selecting a subset, eg 20 per web page
	sel = "select "
		"userid,paydate,payaddress,originaltxn,amount,committxn,commitblockhash"
		HISTORYDATECONTROL
		",paymentid from payments where expirydate=$1";
	par = 0;
	params[par++] = tv_to_buf((tv_t *)(&default_expiry), NULL, 0);
	PARCHK(par, params);
	res = PQexecParams(conn, sel, par, NULL, (const char **)params, NULL, NULL, 0);
	rescode = PQresultStatus(res);
	if (!PGOK(rescode)) {
		PGLOGERR("Select", rescode, conn);
		PQclear(res);
		return false;
	}

	n = PQnfields(res);
	if (n != (fields + HISTORYDATECOUNT)) {
		LOGERR("%s(): Invalid field count - should be %d, but is %d",
			__func__, fields + HISTORYDATECOUNT, n);
		PQclear(res);
		return false;
	}

	n = PQntuples(res);
	LOGDEBUG("%s(): tree build count %d", __func__, n);
	ok = true;
	K_WLOCK(payments_list);
	for (i = 0; i < n; i++) {
		item = k_unlink_head(payments_list);
		row = DATA_PAYMENTS(item);

		PQ_GET_FLD(res, i, "userid", field, ok);
		if (!ok)
			break;
		TXT_TO_BIGINT("userid", field, row->userid);

		PQ_GET_FLD(res, i, "paydate", field, ok);
		if (!ok)
			break;
		TXT_TO_TV("paydate", field, row->paydate);

		PQ_GET_FLD(res, i, "payaddress", field, ok);
		if (!ok)
			break;
		TXT_TO_STR("payaddress", field, row->payaddress);

		PQ_GET_FLD(res, i, "originaltxn", field, ok);
		if (!ok)
			break;
		TXT_TO_STR("originaltxn", field, row->originaltxn);

		PQ_GET_FLD(res, i, "amount", field, ok);
		if (!ok)
			break;
		TXT_TO_BIGINT("amount", field, row->amount);

		PQ_GET_FLD(res, i, "committxn", field, ok);
		if (!ok)
			break;
		TXT_TO_STR("committxn", field, row->committxn);

		PQ_GET_FLD(res, i, "commitblockhash", field, ok);
		if (!ok)
			break;
		TXT_TO_STR("commitblockhash", field, row->commitblockhash);

		HISTORYDATEFLDS(res, i, row, ok);
		if (!ok)
			break;

		PQ_GET_FLD(res, i, "paymentid", field, ok);
		if (!ok)
			break;
		TXT_TO_BIGINT("paymentid", field, row->paymentid);

		payments_root = add_to_ktree(payments_root, item, cmp_payments);
		k_add_head(payments_store, item);
	}
	if (!ok)
		k_add_head(payments_list, item);

	K_WUNLOCK(payments_list);
	PQclear(res);

	if (ok)
		LOGDEBUG("%s(): built", __func__);

	return true;
}

void payments_reload()
{
	PGconn *conn = dbconnect();

	K_WLOCK(payments_list);
	payments_root = free_ktree(payments_root, NULL);
	k_list_transfer_to_head(payments_store, payments_list);
	K_WUNLOCK(payments_list);

	payments_fill(conn);

	PQfinish(conn);
}

// order by workinfoid asc, expirydate asc
static double cmp_workinfo(K_ITEM *a, K_ITEM *b)
{
	double c = (double)(DATA_WORKINFO(a)->workinfoid) -
		   (double)(DATA_WORKINFO(b)->workinfoid);
	if (c == 0) {
		c = tvdiff(&(DATA_WORKINFO(b)->expirydate),
			   &(DATA_WORKINFO(a)->expirydate));
	}
	return c;
}

static K_ITEM *find_workinfo(int64_t workinfoid)
{
	WORKINFO workinfo;
	K_TREE_CTX ctx[1];
	K_ITEM look;

	workinfo.workinfoid = workinfoid;

	look.data = (void *)(&workinfo);
	return find_in_ktree(workinfo_root, &look, cmp_workinfo, ctx);
}

static int64_t workinfo_add(PGconn *conn, char *workinfoidstr, char *poolinstance,
				char *transactiontree, char *merklehash, char *prevhash,
				char *coinbase1, char *coinbase2, char *version,
				char *bits, char *ntime, char *reward,
				tv_t *now, char *by, char *code, char *inet)
{
	ExecStatusType rescode;
	PGresult *res;
	K_ITEM *item;
	int n;
	int64_t workinfoid = -1;
	WORKINFO *row;
	char *ins;
	char *params[11 + HISTORYDATECOUNT];
	int par;

	LOGDEBUG("%s(): add", __func__);

	K_WLOCK(workinfo_list);
	item = k_unlink_head(workinfo_list);
	K_WUNLOCK(workinfo_list);

	row = DATA_WORKINFO(item);

	TXT_TO_BIGINT("workinfoid", workinfoidstr, row->workinfoid);
	STRNCPY(row->poolinstance, poolinstance);
	row->transactiontree = strdup(transactiontree);
	row->merklehash = strdup(merklehash);
	STRNCPY(row->prevhash, prevhash);
	STRNCPY(row->coinbase1, coinbase1);
	STRNCPY(row->coinbase2, coinbase2);
	STRNCPY(row->version, version);
	STRNCPY(row->bits, bits);
	STRNCPY(row->ntime, ntime);
	TXT_TO_BIGINT("reward", reward, row->reward);

	HISTORYDATEINIT(row, now, by, code, inet);
	HISTORYDATETRANSFER(row);

	par = 0;
	params[par++] = bigint_to_buf(row->workinfoid, NULL, 0);
	params[par++] = str_to_buf(row->poolinstance, NULL, 0);
	params[par++] = str_to_buf(row->transactiontree, NULL, 0);
	params[par++] = str_to_buf(row->merklehash, NULL, 0);
	params[par++] = str_to_buf(row->prevhash, NULL, 0);
	params[par++] = str_to_buf(row->coinbase1, NULL, 0);
	params[par++] = str_to_buf(row->coinbase2, NULL, 0);
	params[par++] = str_to_buf(row->version, NULL, 0);
	params[par++] = str_to_buf(row->bits, NULL, 0);
	params[par++] = str_to_buf(row->ntime, NULL, 0);
	params[par++] = bigint_to_buf(row->reward, NULL, 0);
	HISTORYDATEPARAMS(params, par, row);
	PARCHK(par, params);

	ins = "insert into workinfo "
		"(workinfoid,poolinstance,transactiontree,merklehash,"
		"prevhash,coinbase1,coinbase2,version,bits,ntime,reward"
		HISTORYDATECONTROL ") values (" PQPARAM16 ")";

	res = PQexecParams(conn, ins, par, NULL, (const char **)params, NULL, NULL, 0);
	rescode = PQresultStatus(res);
	if (!PGOK(rescode)) {
		PGLOGERR("Insert", rescode, conn);
		goto unparam;
	}

	workinfoid = row->workinfoid;

unparam:
	PQclear(res);
	for (n = 0; n < par; n++)
		free(params[n]);

	K_WLOCK(workinfo_list);
	if (workinfoid == -1) {
		free(row->transactiontree);
		free(row->merklehash);
		k_add_head(workinfo_list, item);
	} else {
		workinfo_root = add_to_ktree(workinfo_root, item, cmp_workinfo);
		k_add_head(workinfo_store, item);
	}
	K_WUNLOCK(workinfo_list);

	return workinfoid;
}

static bool workinfo_fill(PGconn *conn)
{
	ExecStatusType rescode;
	PGresult *res;
	K_ITEM *item;
	int n, i;
	WORKINFO *row;
	char *params[1];
	int par;
	char *field;
	char *sel;
	int fields = 11;
	bool ok;

	LOGDEBUG("%s(): select", __func__);

	// TODO: select the data based on sharesummary since old data isn't needed
	//  however, the ageing rules for workinfo will decide that also
	//  keep the last block + current?
	sel = "select "
		"workinfoid,poolinstance,transactiontree,merklehash,prevhash,"
		"coinbase1,coinbase2,version,bits,ntime,reward"
		HISTORYDATECONTROL
		" from workinfo where expirydate=$1";
	par = 0;
	params[par++] = tv_to_buf((tv_t *)(&default_expiry), NULL, 0);
	PARCHK(par, params);
	res = PQexecParams(conn, sel, par, NULL, (const char **)params, NULL, NULL, 0);
	rescode = PQresultStatus(res);
	if (!PGOK(rescode)) {
		PGLOGERR("Select", rescode, conn);
		PQclear(res);
		return false;
	}

	n = PQnfields(res);
	if (n != (fields + HISTORYDATECOUNT)) {
		LOGERR("%s(): Invalid field count - should be %d, but is %d",
			__func__, fields + HISTORYDATECOUNT, n);
		PQclear(res);
		return false;
	}

	n = PQntuples(res);
	LOGDEBUG("%s(): tree build count %d", __func__, n);
	ok = true;
	K_WLOCK(workinfo_list);
	for (i = 0; i < n; i++) {
		item = k_unlink_head(workinfo_list);
		row = DATA_WORKINFO(item);

		PQ_GET_FLD(res, i, "workinfoid", field, ok);
		if (!ok)
			break;
		TXT_TO_BIGINT("workinfoid", field, row->workinfoid);

		PQ_GET_FLD(res, i, "poolinstance", field, ok);
		if (!ok)
			break;
		TXT_TO_STR("poolinstance", field, row->poolinstance);

		PQ_GET_FLD(res, i, "transactiontree", field, ok);
		if (!ok)
			break;
		TXT_TO_BLOB("transactiontree", field, row->transactiontree);

		PQ_GET_FLD(res, i, "merklehash", field, ok);
		if (!ok)
			break;
		TXT_TO_BLOB("merklehash", field, row->merklehash);

		PQ_GET_FLD(res, i, "prevhash", field, ok);
		if (!ok)
			break;
		TXT_TO_STR("prevhash", field, row->prevhash);

		PQ_GET_FLD(res, i, "coinbase1", field, ok);
		if (!ok)
			break;
		TXT_TO_STR("coinbase1", field, row->coinbase1);

		PQ_GET_FLD(res, i, "coinbase2", field, ok);
		if (!ok)
			break;
		TXT_TO_STR("coinbase2", field, row->coinbase2);

		PQ_GET_FLD(res, i, "version", field, ok);
		if (!ok)
			break;
		TXT_TO_STR("version", field, row->version);

		PQ_GET_FLD(res, i, "bits", field, ok);
		if (!ok)
			break;
		TXT_TO_STR("bits", field, row->bits);

		PQ_GET_FLD(res, i, "ntime", field, ok);
		if (!ok)
			break;
		TXT_TO_STR("ntime", field, row->ntime);

		PQ_GET_FLD(res, i, "reward", field, ok);
		if (!ok)
			break;
		TXT_TO_BIGINT("reward", field, row->reward);

		HISTORYDATEFLDS(res, i, row, ok);
		if (!ok)
			break;

		workinfo_root = add_to_ktree(workinfo_root, item, cmp_workinfo);
		k_add_head(workinfo_store, item);
	}
	if (!ok)
		k_add_head(workinfo_list, item);

	K_WUNLOCK(workinfo_list);
	PQclear(res);

	if (ok)
		LOGDEBUG("%s(): built", __func__);

	return true;
}

void workinfo_reload()
{
	// TODO: ??? a bad idea?
/*
	PGconn *conn = dbconnect();

	K_WLOCK(workinfo_list);
	workinfo_root = free_ktree(workinfo_root, ???); free transactiontree and merklehash
	k_list_transfer_to_head(workinfo_store, workinfo_list);
	K_WUNLOCK(workinfo_list);

	workinfo_fill(conn);

	PQfinish(conn);
*/
}

// order by workinfoid asc, userid asc, createdate asc, nonce asc, expirydate desc
static double cmp_shares(K_ITEM *a, K_ITEM *b)
{
	double c = (double)(DATA_SHARES(a)->workinfoid) -
		   (double)(DATA_SHARES(b)->workinfoid);
	if (c == 0) {
		c = (double)(DATA_SHARES(b)->userid) -
		    (double)(DATA_SHARES(a)->userid);
		if (c == 0) {
			c = tvdiff(&(DATA_SHARES(b)->createdate),
				   &(DATA_SHARES(a)->createdate));
			if (c == 0) {
				c = strcmp(DATA_SHARES(a)->nonce,
					   DATA_SHARES(b)->nonce);
				if (c == 0) {
					c = tvdiff(&(DATA_SHARES(b)->expirydate),
						   &(DATA_SHARES(a)->expirydate));
				}
			}
		}
	}
	return c;
}

// Memory (and log file) only
static bool shares_add(char *workinfoid, char *username, char *workername, char *clientid,
			char *enonce1, char *nonce2, char *nonce, char *diff, char *sdiff,
			char *secondaryuserid, tv_t *now, char *by, char *code, char *inet)
{
	K_ITEM *s_item, *u_item, *w_item;
	SHARES *shares;
	bool ok = false;

	LOGDEBUG("%s(): add", __func__);

	K_WLOCK(shares_list);
	s_item = k_unlink_head(shares_list);
	K_WUNLOCK(shares_list);

	shares = DATA_SHARES(s_item);

	// TODO: allow BTC address later?
	u_item = find_users(username);
	if (!u_item)
		goto unitem;

	shares->userid = DATA_USERS(u_item)->userid;

	TXT_TO_BIGINT("workinfoid", workinfoid, shares->workinfoid);
	STRNCPY(shares->workername, workername);
	TXT_TO_INT("clientid", clientid, shares->clientid);
	STRNCPY(shares->enonce1, enonce1);
	STRNCPY(shares->nonce2, nonce2);
	STRNCPY(shares->nonce, nonce);
	TXT_TO_DOUBLE("diff", diff, shares->diff);
	TXT_TO_DOUBLE("sdiff", sdiff, shares->sdiff);
	STRNCPY(shares->secondaryuserid, secondaryuserid);

	HISTORYDATEINIT(shares, now, by, code, inet);
	HISTORYDATETRANSFER(shares);

	w_item = find_workinfo(shares->workinfoid);
	if (!w_item)
		goto unitem;

	w_item = find_workers(shares->userid, shares->workername);
	if (!w_item)
		goto unitem;

// TODO: update stats - log file load will have to do all these checks also

	ok = true;
unitem:
	K_WLOCK(shares_list);
	if (!ok)
		k_add_head(shares_list, s_item);
	else {
		shares_root = add_to_ktree(shares_root, s_item, cmp_shares);
		k_add_head(shares_store, s_item);
	}
	K_WUNLOCK(shares_list);

	return ok;
}

static bool shares_fill()
{
	// TODO: reload shares from workinfo from log file
	// and verify workinfo while doing that

	return true;
}

// order by workinfoid asc, userid asc, createdate asc, nonce asc, expirydate desc
static double cmp_shareerrors(K_ITEM *a, K_ITEM *b)
{
	double c = (double)(DATA_SHAREERRORS(a)->workinfoid) -
		   (double)(DATA_SHAREERRORS(b)->workinfoid);
	if (c == 0) {
		c = (double)(DATA_SHAREERRORS(b)->userid) -
		    (double)(DATA_SHAREERRORS(a)->userid);
		if (c == 0) {
			c = tvdiff(&(DATA_SHAREERRORS(b)->createdate),
				   &(DATA_SHAREERRORS(a)->createdate));
			if (c == 0) {
				c = tvdiff(&(DATA_SHAREERRORS(b)->expirydate),
					   &(DATA_SHAREERRORS(a)->expirydate));
			}
		}
	}
	return c;
}

// Memory (and log file) only
static bool shareerrors_add(char *workinfoid, char *username, char *workername,
			char *clientid, char *errn, char *error, char *secondaryuserid,
			tv_t *now, char *by, char *code, char *inet)
{
	K_ITEM *s_item, *u_item, *w_item;
	SHAREERRORS *shareerrors;
	bool ok = false;

	LOGDEBUG("%s(): add", __func__);

	K_WLOCK(shareerrors_list);
	s_item = k_unlink_head(shareerrors_list);
	K_WUNLOCK(shareerrors_list);

	shareerrors = DATA_SHAREERRORS(s_item);

	// TODO: allow BTC address later?
	u_item = find_users(username);
	if (!u_item)
		goto unitem;

	shareerrors->userid = DATA_USERS(u_item)->userid;

	TXT_TO_BIGINT("workinfoid", workinfoid, shareerrors->workinfoid);
	STRNCPY(shareerrors->workername, workername);
	TXT_TO_INT("clientid", clientid, shareerrors->clientid);
	TXT_TO_INT("errno", errn, shareerrors->errn);
	STRNCPY(shareerrors->error, error);
	STRNCPY(shareerrors->secondaryuserid, secondaryuserid);

	HISTORYDATEINIT(shareerrors, now, by, code, inet);
	HISTORYDATETRANSFER(shareerrors);

	w_item = find_workinfo(shareerrors->workinfoid);
	if (!w_item)
		goto unitem;

	w_item = find_workers(shareerrors->userid, shareerrors->workername);
	if (!w_item)
		goto unitem;

// TODO: update stats - log file load will have to do all these checks also

	ok = true;
unitem:
	K_WLOCK(shareerrors_list);
	if (!ok)
		k_add_head(shareerrors_list, s_item);
	else {
		shareerrors_root = add_to_ktree(shareerrors_root, s_item, cmp_shareerrors);
		k_add_head(shareerrors_store, s_item);
	}
	K_WUNLOCK(shareerrors_list);

	return ok;
}

static bool shareerrors_fill()
{
	// TODO: reload shareerrors from workinfo from log file
	// and verify workinfo while doing that

	return true;
}

static double cmp_auths(K_ITEM *a, K_ITEM *b)
{
	double c = (double)(DATA_AUTHS(a)->authid) -
		   (double)(DATA_AUTHS(b)->authid);
	if (c == 0) {
		c = (double)(DATA_AUTHS(b)->userid) -
		    (double)(DATA_AUTHS(a)->userid);
		if (c == 0) {
			c = tvdiff(&(DATA_AUTHS(b)->createdate),
				   &(DATA_AUTHS(a)->createdate));
			if (c == 0) {
				c = tvdiff(&(DATA_AUTHS(b)->expirydate),
					   &(DATA_AUTHS(a)->expirydate));
			}
		}
	}
	return c;
}

static char *auths_add(PGconn *conn, char *username, char *workername,
				char *clientid, char *enonce1, char *useragent,
				tv_t *now, char *by, char *code, char *inet)
{
	ExecStatusType rescode;
	PGresult *res;
	K_ITEM *a_item, *u_item;
	int n;
	AUTHS *row;
	char *ins;
	char *secuserid = NULL;
	char *params[6 + HISTORYDATECOUNT];
	int par;

	LOGDEBUG("%s(): add", __func__);

	K_WLOCK(auths_list);
	a_item = k_unlink_head(auths_list);
	K_WUNLOCK(auths_list);

	row = DATA_AUTHS(a_item);

	u_item = find_users(username);
	if (!u_item)
		goto unitem;

	row->userid = DATA_USERS(u_item)->userid;
	new_worker(conn, false, row->userid, workername, DIFFICULTYDEFAULT_DEF_STR,
		   IDLENOTIFICATIONENABLED_DEF, IDLENOTIFICATIONTIME_DEF_STR, now,
		   by, code, inet);
	STRNCPY(row->workername, workername);
	TXT_TO_INT("clientid", clientid, row->clientid);
	STRNCPY(row->enonce1, enonce1);
	STRNCPY(row->useragent, useragent);

	HISTORYDATEINIT(row, now, by, code, inet);
	HISTORYDATETRANSFER(row);

	row->authid = nextid(conn, "authid", (int64_t)1, now, by, code, inet);
	if (row->authid == 0)
		goto unitem;

	par = 0;
	params[par++] = bigint_to_buf(row->authid, NULL, 0);
	params[par++] = bigint_to_buf(row->userid, NULL, 0);
	params[par++] = str_to_buf(row->workername, NULL, 0);
	params[par++] = int_to_buf(row->clientid, NULL, 0);
	params[par++] = str_to_buf(row->enonce1, NULL, 0);
	params[par++] = str_to_buf(row->useragent, NULL, 0);
	HISTORYDATEPARAMS(params, par, row);
	PARCHK(par, params);

	ins = "insert into auths "
		"(authid,userid,workername,clientid,enonce1,useragent"
		HISTORYDATECONTROL ") values (" PQPARAM11 ")";

	res = PQexecParams(conn, ins, par, NULL, (const char **)params, NULL, NULL, 0);
	rescode = PQresultStatus(res);
	if (!PGOK(rescode)) {
		PGLOGERR("Insert", rescode, conn);
		goto unparam;
	}

	secuserid = DATA_USERS(u_item)->secondaryuserid;

unparam:
	PQclear(res);
	for (n = 0; n < par; n++)
		free(params[n]);
unitem:
	K_WLOCK(auths_list);
	if (!secuserid)
		k_add_head(auths_list, a_item);
	else {
		auths_root = add_to_ktree(auths_root, a_item, cmp_auths);
		k_add_head(auths_store, a_item);
	}
	K_WUNLOCK(auths_list);

	return secuserid;
}

static bool auths_fill(PGconn *conn)
{
	ExecStatusType rescode;
	PGresult *res;
	K_ITEM *item;
	int n, i;
	AUTHS *row;
	char *params[1];
	int par;
	char *field;
	char *sel;
	int fields = 6;
	bool ok;

	LOGDEBUG("%s(): select", __func__);

	// TODO: keep last x - since a user may login and mine for 100 days
	sel = "select "
		"authid,userid,workername,clientid,enonce1,useragent"
		HISTORYDATECONTROL
		" from auths where expirydate=$1";
	par = 0;
	params[par++] = tv_to_buf((tv_t *)(&default_expiry), NULL, 0);
	PARCHK(par, params);
	res = PQexecParams(conn, sel, par, NULL, (const char **)params, NULL, NULL, 0);
	rescode = PQresultStatus(res);
	if (!PGOK(rescode)) {
		PGLOGERR("Select", rescode, conn);
		PQclear(res);
		return false;
	}

	n = PQnfields(res);
	if (n != (fields + HISTORYDATECOUNT)) {
		LOGERR("%s(): Invalid field count - should be %d, but is %d",
			__func__, fields + HISTORYDATECOUNT, n);
		PQclear(res);
		return false;
	}

	n = PQntuples(res);
	LOGDEBUG("%s(): tree build count %d", __func__, n);
	ok = true;
	K_WLOCK(auths_list);
	for (i = 0; i < n; i++) {
		item = k_unlink_head(auths_list);
		row = DATA_AUTHS(item);

		PQ_GET_FLD(res, i, "authid", field, ok);
		if (!ok)
			break;
		TXT_TO_BIGINT("authid", field, row->authid);

		PQ_GET_FLD(res, i, "userid", field, ok);
		if (!ok)
			break;
		TXT_TO_BIGINT("userid", field, row->userid);

		PQ_GET_FLD(res, i, "workername", field, ok);
		if (!ok)
			break;
		TXT_TO_STR("workername", field, row->workername);

		PQ_GET_FLD(res, i, "clientid", field, ok);
		if (!ok)
			break;
		TXT_TO_INT("clientid", field, row->clientid);

		PQ_GET_FLD(res, i, "enonce1", field, ok);
		if (!ok)
			break;
		TXT_TO_BLOB("enonce1", field, row->enonce1);

		PQ_GET_FLD(res, i, "useragent", field, ok);
		if (!ok)
			break;
		TXT_TO_STR("useragent", field, row->useragent);

		HISTORYDATEFLDS(res, i, row, ok);
		if (!ok)
			break;

		auths_root = add_to_ktree(auths_root, item, cmp_auths);
		k_add_head(auths_store, item);
	}
	if (!ok)
		k_add_head(auths_list, item);

	K_WUNLOCK(auths_list);
	PQclear(res);

	if (ok)
		LOGDEBUG("%s(): built", __func__);

	return true;
}

void auths_reload()
{
	PGconn *conn = dbconnect();

	K_WLOCK(auths_list);
	auths_root = free_ktree(auths_root, NULL);
	k_list_transfer_to_head(auths_store, auths_list);
	K_WUNLOCK(auths_list);

	auths_fill(conn);

	PQfinish(conn);
}

// order by poolinstance asc, createdate asc
static double cmp_poolstats(K_ITEM *a, K_ITEM *b)
{
	double c = (double)strcmp(DATA_POOLSTATS(a)->poolinstance,
				  DATA_POOLSTATS(b)->poolinstance);
	if (c == 0) {
		c = tvdiff(&(DATA_POOLSTATS(a)->createdate),
			   &(DATA_POOLSTATS(b)->createdate));
	}
	return c;
}

static bool poolstats_add(PGconn *conn, bool store, char *poolinstance, char *users,
				char *workers, char *hashrate, char *hashrate5m,
				char *hashrate1hr, char *hashrate24hr,
				tv_t *now, char *by, char *code, char *inet)
{
	ExecStatusType rescode;
	PGresult *res;
	K_ITEM *p_item;
	int n;
	POOLSTATS *row;
	char *ins;
	char *params[7 + SIMPLEDATECOUNT];
	int par;
	bool ok = false;

	LOGDEBUG("%s(): add", __func__);

	K_WLOCK(poolstats_list);
	p_item = k_unlink_head(poolstats_list);
	K_WUNLOCK(poolstats_list);

	row = DATA_POOLSTATS(p_item);

	STRNCPY(row->poolinstance, poolinstance);
	TXT_TO_INT("users", users, row->users);
	TXT_TO_INT("workers", workers, row->workers);
	TXT_TO_DOUBLE("hashrate", hashrate, row->hashrate);
	TXT_TO_DOUBLE("hashrate5m", hashrate5m, row->hashrate5m);
	TXT_TO_DOUBLE("hashrate1hr", hashrate1hr, row->hashrate1hr);
	TXT_TO_DOUBLE("hashrate24hr", hashrate24hr, row->hashrate24hr);

	SIMPLEDATEINIT(row, now, by, code, inet);
	SIMPLEDATETRANSFER(row);

	par = 0;
	if (store) {
		params[par++] = str_to_buf(row->poolinstance, NULL, 0);
		params[par++] = int_to_buf(row->users, NULL, 0);
		params[par++] = int_to_buf(row->workers, NULL, 0);
		params[par++] = bigint_to_buf(row->hashrate, NULL, 0);
		params[par++] = bigint_to_buf(row->hashrate5m, NULL, 0);
		params[par++] = bigint_to_buf(row->hashrate1hr, NULL, 0);
		params[par++] = bigint_to_buf(row->hashrate24hr, NULL, 0);
		SIMPLEDATEPARAMS(params, par, row);
		PARCHK(par, params);

		ins = "insert into poolstats "
			"(poolinstance,users,workers,hashrate,hashrate5m,hashrate1hr,hashrate24hr"
			SIMPLEDATECONTROL ") values (" PQPARAM11 ")";

		res = PQexecParams(conn, ins, par, NULL, (const char **)params, NULL, NULL, 0);
		rescode = PQresultStatus(res);
		if (!PGOK(rescode)) {
			PGLOGERR("Insert", rescode, conn);
			goto unparam;
		}
	}

	ok = true;
unparam:
	if (store) {
		PQclear(res);
		for (n = 0; n < par; n++)
			free(params[n]);
	}

	K_WLOCK(poolstats_list);
	if (!ok)
		k_add_head(poolstats_list, p_item);
	else {
		poolstats_root = add_to_ktree(poolstats_root, p_item, cmp_poolstats);
		k_add_head(poolstats_store, p_item);
	}
	K_WUNLOCK(poolstats_list);

	return ok;
}

// TODO: data selection - only require ?
static bool poolstats_fill(PGconn *conn)
{
	ExecStatusType rescode;
	PGresult *res;
	K_ITEM *item;
	int n, i;
	POOLSTATS *row;
	char *field;
	char *sel;
	int fields = 7;
	bool ok;

	LOGDEBUG("%s(): select", __func__);

	sel = "select "
		"poolinstance,users,workers,hashrate,hashrate5m,hashrate1hr,hashrate24hr"
		SIMPLEDATECONTROL
		" from poolstats";
	res = PQexec(conn, sel);
	rescode = PQresultStatus(res);
	if (!PGOK(rescode)) {
		PGLOGERR("Select", rescode, conn);
		PQclear(res);
		return false;
	}

	n = PQnfields(res);
	if (n != (fields + SIMPLEDATECOUNT)) {
		LOGERR("%s(): Invalid field count - should be %d, but is %d",
			__func__, fields + SIMPLEDATECOUNT, n);
		PQclear(res);
		return false;
	}

	n = PQntuples(res);
	LOGDEBUG("%s(): tree build count %d", __func__, n);
	ok = true;
	K_WLOCK(poolstats_list);
	for (i = 0; i < n; i++) {
		item = k_unlink_head(poolstats_list);
		row = DATA_POOLSTATS(item);

		PQ_GET_FLD(res, i, "poolinstance", field, ok);
		if (!ok)
			break;
		TXT_TO_STR("poolinstance", field, row->poolinstance);

		PQ_GET_FLD(res, i, "users", field, ok);
		if (!ok)
			break;
		TXT_TO_INT("users", field, row->users);

		PQ_GET_FLD(res, i, "workers", field, ok);
		if (!ok)
			break;
		TXT_TO_INT("workers", field, row->workers);

		PQ_GET_FLD(res, i, "hashrate", field, ok);
		if (!ok)
			break;
		TXT_TO_DOUBLE("hashrate", field, row->hashrate);

		PQ_GET_FLD(res, i, "hashrate5m", field, ok);
		if (!ok)
			break;
		TXT_TO_DOUBLE("hashrate5m", field, row->hashrate5m);

		PQ_GET_FLD(res, i, "hashrate1hr", field, ok);
		if (!ok)
			break;
		TXT_TO_DOUBLE("hashrate1hr", field, row->hashrate1hr);

		PQ_GET_FLD(res, i, "hashrate24hr", field, ok);
		if (!ok)
			break;
		TXT_TO_DOUBLE("hashrate24hr", field, row->hashrate24hr);

		poolstats_root = add_to_ktree(poolstats_root, item, cmp_poolstats);
		k_add_head(poolstats_store, item);
	}
	if (!ok)
		k_add_head(poolstats_list, item);

	K_WUNLOCK(poolstats_list);
	PQclear(res);

	if (ok)
		LOGDEBUG("%s(): built", __func__);

	return true;
}

void poolstats_reload()
{
	PGconn *conn = dbconnect();

	K_WLOCK(poolstats_list);
	poolstats_root = free_ktree(poolstats_root, NULL);
	k_list_transfer_to_head(poolstats_store, poolstats_list);
	K_WUNLOCK(poolstats_list);

	poolstats_fill(conn);

	PQfinish(conn);
}

static void getdata()
{
	PGconn *conn = dbconnect();

	users_fill(conn);
	workers_fill(conn);
	payments_fill(conn);
	workinfo_fill(conn);
	shares_fill();
	shareerrors_fill();
	auths_fill(conn);
	poolstats_fill(conn);

	PQfinish(conn);
}

static PGconn *dbquit(PGconn *conn)
{
	if (conn != NULL)
		PQfinish(conn);
	return NULL;
}

/* Open the file in path, check if there is a pid in there that still exists
 * and if not, write the pid into that file. */
static bool write_pid(ckpool_t *ckp, const char *path, pid_t pid)
{
	struct stat statbuf;
	FILE *fp;
	int ret;

	if (!stat(path, &statbuf)) {
		int oldpid;

		LOGWARNING("File %s exists", path);
		fp = fopen(path, "r");
		if (!fp) {
			LOGEMERG("Failed to open file %s", path);
			return false;
		}
		ret = fscanf(fp, "%d", &oldpid);
		fclose(fp);
		if (ret == 1 && !(kill(oldpid, 0))) {
			if (!ckp->killold) {
				LOGEMERG("Process %s pid %d still exists, start ckpool with -k if you wish to kill it",
					 path, oldpid);
				return false;
			}
			if (kill(oldpid, 9)) {
				LOGEMERG("Unable to kill old process %s pid %d", path, oldpid);
				return false;
			}
			LOGWARNING("Killing off old process %s pid %d", path, oldpid);
		}
	}
	fp = fopen(path, "w");
	if (!fp) {
		LOGERR("Failed to open file %s", path);
		return false;
	}
	fprintf(fp, "%d", pid);
	fclose(fp);

	return true;
}

static void create_process_unixsock(proc_instance_t *pi)
{
	unixsock_t *us = &pi->us;

	us->path = strdup(pi->ckp->socket_dir);
	realloc_strcat(&us->path, pi->sockname);
	LOGDEBUG("Opening %s", us->path);
	us->sockd = open_unix_server(us->path);
	if (unlikely(us->sockd < 0))
		quit(1, "Failed to open %s socket", pi->sockname);
}

static void write_namepid(proc_instance_t *pi)
{
	char s[256];

	pi->pid = getpid();
	sprintf(s, "%s%s.pid", pi->ckp->socket_dir, pi->processname);
	if (!write_pid(pi->ckp, s, pi->pid))
		quit(1, "Failed to write %s pid %d", pi->processname, pi->pid);
}

static void rm_namepid(proc_instance_t *pi)
{
	char s[256];

	sprintf(s, "%s%s.pid", pi->ckp->socket_dir, pi->processname);
	unlink(s);

}

static void clean_up(ckpool_t *ckp)
{
	rm_namepid(&ckp->main);
	dealloc(ckp->socket_dir);
	fclose(ckp->logfp);
}

static void setup_data()
{
	transfer_list = k_new_list("Transfer", sizeof(TRANSFER), ALLOC_TRANSFER, LIMIT_TRANSFER, true);
	transfer_store = k_new_store(transfer_list);
	transfer_root = new_ktree();

	users_list = k_new_list("Users", sizeof(USERS), ALLOC_USERS, LIMIT_USERS, true);
	users_store = k_new_store(users_list);
	users_root = new_ktree();
	userid_root = new_ktree();

	workers_list = k_new_list("Workers", sizeof(WORKERS), ALLOC_WORKERS, LIMIT_WORKERS, true);
	workers_store = k_new_store(workers_list);
	workers_root = new_ktree();

	payments_list = k_new_list("Payments", sizeof(PAYMENTS), ALLOC_PAYMENTS, LIMIT_PAYMENTS, true);
	payments_store = k_new_store(payments_list);
	payments_root = new_ktree();

	idcontrol_list = k_new_list("IDControl", sizeof(IDCONTROL), ALLOC_IDCONTROL, LIMIT_IDCONTROL, true);
	idcontrol_store = k_new_store(idcontrol_list);

	workinfo_list = k_new_list("WorkInfo", sizeof(WORKINFO), ALLOC_WORKINFO, LIMIT_WORKINFO, true);
	workinfo_store = k_new_store(workinfo_list);
	workinfo_root = new_ktree();

	shares_list = k_new_list("Shares", sizeof(SHARES), ALLOC_SHARES, LIMIT_SHARES, true);
	shares_store = k_new_store(shares_list);
	shares_root = new_ktree();

	shareerrors_list = k_new_list("ShareErrors", sizeof(SHAREERRORS), ALLOC_SHAREERRORS, LIMIT_SHAREERRORS, true);
	shareerrors_store = k_new_store(shareerrors_list);
	shareerrors_root = new_ktree();

	auths_list = k_new_list("Auths", sizeof(AUTHS), ALLOC_AUTHS, LIMIT_AUTHS, true);
	auths_store = k_new_store(auths_list);
	auths_root = new_ktree();

	poolstats_list = k_new_list("PoolStats", sizeof(POOLSTATS), ALLOC_POOLSTATS, LIMIT_POOLSTATS, true);
	poolstats_store = k_new_store(poolstats_list);
	poolstats_root = new_ktree();

	getdata();
}

static char *cmd_adduser(char *id, tv_t *now, char *by, char *code, char *inet)
{
	char reply[1024] = "";
	size_t siz = sizeof(reply);

	K_ITEM *i_username, *i_emailaddress, *i_passwordhash;
	PGconn *conn;
	bool ok;

	i_username = require_name("username", 3, (char *)userpatt, reply, siz);
	if (!i_username)
		return strdup(reply);

	i_emailaddress = require_name("emailaddress", 7, (char *)mailpatt, reply, siz);
	if (!i_emailaddress)
		return strdup(reply);

	i_passwordhash = require_name("passwordhash", 64, (char *)hashpatt, reply, siz);
	if (!i_passwordhash)
		return strdup(reply);

	conn = dbconnect();
	ok = users_add(conn, DATA_TRANSFER(i_username)->data,
				DATA_TRANSFER(i_emailaddress)->data,
				DATA_TRANSFER(i_passwordhash)->data,
				now, by, code, inet);
	PQfinish(conn);

	if (!ok) {
		STRNCPY(reply, "failed.DBE");
		return strdup(reply);
	}

	LOGDEBUG("%s.added.%s", id, DATA_TRANSFER(i_username)->data);
	snprintf(reply, siz, "added.%s", DATA_TRANSFER(i_username)->data);

	return strdup(reply);
}

static char *cmd_chkpass(char *id, __maybe_unused tv_t *now, __maybe_unused char *by,
				__maybe_unused char *code, __maybe_unused char *inet)
{
	K_ITEM *i_username, *i_passwordhash, *u_item;
	char reply[1024] = "";
	size_t siz = sizeof(reply);
	bool ok;

	i_username = require_name("username", 3, (char *)userpatt, reply, siz);
	if (!i_username)
		return strdup(reply);

	i_passwordhash = require_name("passwordhash", 64, (char *)hashpatt, reply, siz);
	if (!i_passwordhash)
		return strdup(reply);

	u_item = find_users(DATA_TRANSFER(i_username)->data);

	if (!u_item)
		ok = false;
	else {
		if (strcasecmp(DATA_TRANSFER(i_passwordhash)->data, DATA_USERS(u_item)->passwordhash) == 0)
			ok = true;
		else
			ok = false;
	}

	if (!ok)
		return strdup("bad");

	LOGDEBUG("%s.login.%s", id, DATA_TRANSFER(i_username)->data);
	return strdup("ok");
}

static char *cmd_poolstats(__maybe_unused char *id, tv_t *now, char *by, char *code, char *inet)
{
	char reply[1024] = "";
	size_t siz = sizeof(reply);
	K_TREE_CTX ctx[1];
	PGconn *conn;
	bool store;

	// log to logfile

	K_ITEM *i_poolinstance, *i_users, *i_workers, *i_hashrate, *i_hashrate5m;
	K_ITEM *i_hashrate1hr, *i_hashrate24hr, *i_createdate, look, *ps;
	tv_t createdate;
	POOLSTATS row;
	bool ok = false;

	i_poolinstance = require_name("poolinstance", 1, NULL, reply, siz);
	if (!i_poolinstance)
		return strdup(reply);

	i_users = require_name("users", 1, NULL, reply, siz);
	if (!i_users)
		return strdup(reply);

	i_workers = require_name("workers", 1, NULL, reply, siz);
	if (!i_workers)
		return strdup(reply);

	i_hashrate = require_name("hashrate", 1, NULL, reply, siz);
	if (!i_hashrate)
		return strdup(reply);

	i_hashrate5m = require_name("hashrate5m", 1, NULL, reply, siz);
	if (!i_hashrate5m)
		return strdup(reply);

	i_hashrate1hr = require_name("hashrate1hr", 1, NULL, reply, siz);
	if (!i_hashrate1hr)
		return strdup(reply);

	i_hashrate24hr = require_name("hashrate24hr", 1, NULL, reply, siz);
	if (!i_hashrate24hr)
		return strdup(reply);

	STRNCPY(row.poolinstance, DATA_TRANSFER(i_poolinstance)->data);
	row.createdate.tv_sec = date_eot.tv_sec;
	row.createdate.tv_usec = date_eot.tv_usec;
	look.data = (void *)(&row);
	ps = find_before_in_ktree(poolstats_root, &look, cmp_poolstats, ctx);
	if (!ps)
		store = true;
	else {
		i_createdate = require_name("createdate", 1, NULL, reply, siz);
		if (!i_createdate)
			return strdup(reply);
		TXT_TO_TV("createdate", DATA_TRANSFER(i_createdate)->data, createdate);
		if (tvdiff(&createdate, &(row.createdate)) > STATS_PER)
			store = true;
		else
			store = false;
	}

	conn = dbconnect();
	ok = poolstats_add(conn, store, DATA_TRANSFER(i_poolinstance)->data,
					DATA_TRANSFER(i_users)->data,
					DATA_TRANSFER(i_workers)->data,
					DATA_TRANSFER(i_hashrate)->data,
					DATA_TRANSFER(i_hashrate5m)->data,
					DATA_TRANSFER(i_hashrate1hr)->data,
					DATA_TRANSFER(i_hashrate24hr)->data,
					now, by, code, inet);
	PQfinish(conn);

	if (!ok) {
		STRNCPY(reply, "bad.DBE");
		return strdup(reply);
	}

	LOGDEBUG("%s.added.ok", id);
	snprintf(reply, siz, "added.ok");
	return strdup(reply);
}

static char *cmd_newid(__maybe_unused char *id, tv_t *now, char *by, char *code, char *inet)
{
	char reply[1024] = "";
	size_t siz = sizeof(reply);
	K_ITEM *i_idname, *i_idvalue, *look;
	IDCONTROL *row;
	char *params[2 + MODIFYDATECOUNT];
	int par;
	bool ok = false;
	ExecStatusType rescode;
	PGresult *res;
	PGconn *conn;
	char *ins;
	int n;

	LOGDEBUG("%s(): add", __func__);

	i_idname = require_name("idname", 3, (char *)idpatt, reply, siz);
	if (!i_idname)
		return strdup(reply);

	i_idvalue = require_name("idvalue", 1, (char *)intpatt, reply, siz);
	if (!i_idvalue)
		return strdup(reply);

	K_WLOCK(idcontrol_list);
	look = k_unlink_head(idcontrol_list);
	K_WUNLOCK(idcontrol_list);

	row = DATA_IDCONTROL(look);

	STRNCPY(row->idname, DATA_TRANSFER(i_idname)->data);
	TXT_TO_BIGINT("idvalue", DATA_TRANSFER(i_idvalue)->data, row->lastid);
	MODIFYDATEINIT(row, now, by, code, inet);

	par = 0;
	params[par++] = str_to_buf(row->idname, NULL, 0);
	params[par++] = bigint_to_buf(row->lastid, NULL, 0);
	MODIFYDATEPARAMS(params, par, row);
	PARCHK(par, params);

	ins = "insert into idcontrol "
		"(idname,lastid" MODIFYDATECONTROL ") values (" PQPARAM10 ")";

	conn = dbconnect();

	res = PQexecParams(conn, ins, par, NULL, (const char **)params, NULL, NULL, 0);
	rescode = PQresultStatus(res);
	if (!PGOK(rescode)) {
		PGLOGERR("Insert", rescode, conn);
		goto foil;
	}

	ok = true;
foil:
	PQclear(res);
	PQfinish(conn);
	for (n = 0; n < par; n++)
		free(params[n]);

	K_WLOCK(idcontrol_list);
	k_add_head(idcontrol_list, look);
	K_WUNLOCK(idcontrol_list);

	if (!ok) {
		snprintf(reply, siz, "failed.DBE");
		return strdup(reply);
	}

	LOGDEBUG("added.%s", DATA_TRANSFER(i_idname)->data);
	snprintf(reply, siz, "added.%s", DATA_TRANSFER(i_idname)->data);
	return strdup(reply);
}

static char *cmd_payments(char *id, __maybe_unused tv_t *now, __maybe_unused char *by,
				__maybe_unused char *code, __maybe_unused char *inet)
{
	K_ITEM *i_username, *look, *u_item, *p_item;
	K_TREE_CTX ctx[1];
	PAYMENTS *row;
	char reply[1024] = "";
	char tmp[1024];
	size_t siz = sizeof(reply);
	char *buf;
	size_t len, off;
	int rows;

	i_username = require_name("username", 3, (char *)userpatt, reply, siz);
	if (!i_username)
		return strdup(reply);

	u_item = find_users(DATA_TRANSFER(i_username)->data);
	if (!u_item)
		return strdup("bad");

	K_WLOCK(payments_list);
	look = k_unlink_head(payments_list);
	K_WUNLOCK(payments_list);
	row = DATA_PAYMENTS(look);
	row->userid = DATA_USERS(u_item)->userid;
	row->paydate.tv_sec = 0;
	row->paydate.tv_usec = 0;
	p_item = find_after_in_ktree(payments_root, look, cmp_payments, ctx);
	len = 1024;
	buf = malloc(len);
	if (!buf)
		quithere(1, "malloc buf (%d) OOM", (int)len);
	strcpy(buf, "ok.");
	off = strlen(buf);
	rows = 0;
	while (p_item && DATA_PAYMENTS(p_item)->userid == DATA_USERS(u_item)->userid) {
		tv_to_buf(&(DATA_PAYMENTS(p_item)->paydate), reply, sizeof(reply));
		snprintf(tmp, sizeof(tmp), "paydate%d=%s%c", rows, reply, FLDSEP);
		APPEND_REALLOC(buf, off, len, tmp);

		str_to_buf(DATA_PAYMENTS(p_item)->payaddress, reply, sizeof(reply));
		snprintf(tmp, sizeof(tmp), "payaddress%d=%s%c", rows, reply, FLDSEP);
		APPEND_REALLOC(buf, off, len, tmp);

		bigint_to_buf(DATA_PAYMENTS(p_item)->amount, reply, sizeof(reply));
		snprintf(tmp, sizeof(tmp), "amount%d=%s%c", rows, reply, FLDSEP);
		APPEND_REALLOC(buf, off, len, tmp);

		rows++;
		p_item = next_in_ktree(ctx);
	}
	snprintf(tmp, sizeof(tmp), "rows=%d", rows);
	APPEND_REALLOC(buf, off, len, tmp);

	K_WLOCK(payments_list);
	k_add_head(payments_list, look);
	K_WUNLOCK(payments_list);

	LOGDEBUG("%s.payments.%s", id, DATA_TRANSFER(i_username)->data);
	return buf;
}

static char *cmd_sharelog(__maybe_unused char *id, tv_t *now, char *by, char *code, char *inet)
{
	char reply[1024] = "";
	size_t siz = sizeof(reply);
	K_ITEM *i_method;
	PGconn *conn;

	// log to logfile with processing success/failure code

	i_method = require_name("method", 1, NULL, reply, siz);
	if (!i_method)
		return strdup(reply);

	if (strcasecmp(DATA_TRANSFER(i_method)->data, METHOD_WORKINFO) == 0) {
		K_ITEM *i_workinfoid, *i_poolinstance, *i_transactiontree, *i_merklehash;
		K_ITEM *i_prevhash, *i_coinbase1, *i_coinbase2, *i_version, *i_bits;
		K_ITEM *i_ntime, *i_reward;
		int64_t workinfoid;

		i_workinfoid = require_name("workinfoid", 1, NULL, reply, siz);
		if (!i_workinfoid)
			return strdup(reply);

		i_poolinstance = require_name("poolinstance", 1, NULL, reply, siz);
		if (!i_poolinstance)
			return strdup(reply);

		i_transactiontree = require_name("transactiontree", 1, NULL, reply, siz);
		if (!i_transactiontree)
			return strdup(reply);

		i_merklehash = require_name("merklehash", 1, NULL, reply, siz);
		if (!i_merklehash)
			return strdup(reply);

		i_prevhash = require_name("prevhash", 1, NULL, reply, siz);
		if (!i_prevhash)
			return strdup(reply);

		i_coinbase1 = require_name("coinbase1", 1, NULL, reply, siz);
		if (!i_coinbase1)
			return strdup(reply);

		i_coinbase2 = require_name("coinbase2", 1, NULL, reply, siz);
		if (!i_coinbase2)
			return strdup(reply);

		i_version = require_name("version", 1, NULL, reply, siz);
		if (!i_version)
			return strdup(reply);

		i_bits = require_name("bits", 1, NULL, reply, siz);
		if (!i_bits)
			return strdup(reply);

		i_ntime = require_name("ntime", 1, NULL, reply, siz);
		if (!i_ntime)
			return strdup(reply);

		i_reward = require_name("reward", 1, NULL, reply, siz);
		if (!i_reward)
			return strdup(reply);

		conn = dbconnect();
		workinfoid = workinfo_add(conn, DATA_TRANSFER(i_workinfoid)->data,
						DATA_TRANSFER(i_poolinstance)->data,
						DATA_TRANSFER(i_transactiontree)->data,
						DATA_TRANSFER(i_merklehash)->data,
						DATA_TRANSFER(i_prevhash)->data,
						DATA_TRANSFER(i_coinbase1)->data,
						DATA_TRANSFER(i_coinbase2)->data,
						DATA_TRANSFER(i_version)->data,
						DATA_TRANSFER(i_bits)->data,
						DATA_TRANSFER(i_ntime)->data,
						DATA_TRANSFER(i_reward)->data,
						now, by, code, inet);
		PQfinish(conn);

		if (workinfoid == -1) {
			STRNCPY(reply, "bad.DBE");
			return strdup(reply);
		}

		LOGDEBUG("added.%s.%"PRId64, DATA_TRANSFER(i_method)->data, workinfoid);
		snprintf(reply, siz, "added.%"PRId64, workinfoid);
		return strdup(reply);
	} else if (strcasecmp(DATA_TRANSFER(i_method)->data, METHOD_SHARES) == 0) {
		K_ITEM *i_workinfoid, *i_username, *i_workername, *i_clientid, *i_enonce1;
		K_ITEM *i_nonce2, *i_nonce, *i_diff, *i_sdiff, *i_secondaryuserid;
		bool ok;

		i_workinfoid = require_name("workinfoid", 1, NULL, reply, siz);
		if (!i_workinfoid)
			return strdup(reply);

		i_username = require_name("username", 1, NULL, reply, siz);
		if (!i_username)
			return strdup(reply);

		i_workername = require_name("workername", 1, NULL, reply, siz);
		if (!i_workername)
			return strdup(reply);

		i_clientid = require_name("clientid", 1, NULL, reply, siz);
		if (!i_clientid)
			return strdup(reply);

		i_enonce1 = require_name("enonce1", 1, NULL, reply, siz);
		if (!i_enonce1)
			return strdup(reply);

		i_nonce2 = require_name("nonce2", 1, NULL, reply, siz);
		if (!i_nonce2)
			return strdup(reply);

		i_nonce = require_name("nonce", 1, NULL, reply, siz);
		if (!i_nonce)
			return strdup(reply);

		i_diff = require_name("diff", 1, NULL, reply, siz);
		if (!i_diff)
			return strdup(reply);

		i_sdiff = require_name("sdiff", 1, NULL, reply, siz);
		if (!i_sdiff)
			return strdup(reply);

		i_secondaryuserid = require_name("secondaryuserid", 1, NULL, reply, siz);
		if (!i_secondaryuserid)
			return strdup(reply);

		ok = shares_add(DATA_TRANSFER(i_workinfoid)->data,
				DATA_TRANSFER(i_username)->data,
				DATA_TRANSFER(i_workername)->data,
				DATA_TRANSFER(i_clientid)->data,
				DATA_TRANSFER(i_enonce1)->data,
				DATA_TRANSFER(i_nonce2)->data,
				DATA_TRANSFER(i_nonce)->data,
				DATA_TRANSFER(i_diff)->data,
				DATA_TRANSFER(i_sdiff)->data,
				DATA_TRANSFER(i_secondaryuserid)->data,
				now, by, code, inet);
		if (!ok) {
			STRNCPY(reply, "bad.DATA");
			return strdup(reply);
		}

		LOGDEBUG("added.%s.%s", DATA_TRANSFER(i_method)->data,
					DATA_TRANSFER(i_nonce)->data);
		snprintf(reply, siz, "added.%s", DATA_TRANSFER(i_nonce)->data);
		return strdup(reply);
	} else if (strcasecmp(DATA_TRANSFER(i_method)->data, METHOD_SHAREERRORS) == 0) {
		K_ITEM *i_workinfoid, *i_username, *i_workername, *i_clientid, *i_errn;
		K_ITEM *i_error, *i_secondaryuserid;
		bool ok;

		i_workinfoid = require_name("workinfoid", 1, NULL, reply, siz);
		if (!i_workinfoid)
			return strdup(reply);

		i_username = require_name("username", 1, NULL, reply, siz);
		if (!i_username)
			return strdup(reply);

		i_workername = require_name("workername", 1, NULL, reply, siz);
		if (!i_workername)
			return strdup(reply);

		i_clientid = require_name("clientid", 1, NULL, reply, siz);
		if (!i_clientid)
			return strdup(reply);

		i_errn = require_name("errno", 1, NULL, reply, siz);
		if (!i_errn)
			return strdup(reply);

		i_error = require_name("error", 1, NULL, reply, siz);
		if (!i_error)
			return strdup(reply);

		i_secondaryuserid = require_name("secondaryuserid", 1, NULL, reply, siz);
		if (!i_secondaryuserid)
			return strdup(reply);

		ok = shareerrors_add(DATA_TRANSFER(i_workinfoid)->data,
					DATA_TRANSFER(i_username)->data,
					DATA_TRANSFER(i_workername)->data,
					DATA_TRANSFER(i_clientid)->data,
					DATA_TRANSFER(i_errn)->data,
					DATA_TRANSFER(i_error)->data,
					DATA_TRANSFER(i_secondaryuserid)->data,
					now, by, code, inet);
		if (!ok) {
			STRNCPY(reply, "bad.DATA");
			return strdup(reply);
		}

		LOGDEBUG("added.%s.%s", DATA_TRANSFER(i_method)->data,
					DATA_TRANSFER(i_username)->data);
		snprintf(reply, siz, "added.%s", DATA_TRANSFER(i_username)->data);
		return strdup(reply);
	}

	STRNCPY(reply, "bad.method");
	return strdup(reply);
}

static char *cmd_auth(__maybe_unused char *id, tv_t *now, char *by, char *code, char *inet)
{
	char reply[1024] = "";
	size_t siz = sizeof(reply);
	K_ITEM *i_method;
	PGconn *conn;

	i_method = require_name("method", 1, NULL, reply, siz);
	if (!i_method)
		return strdup(reply);

	if (strcasecmp(DATA_TRANSFER(i_method)->data, METHOD_AUTH) == 0) {
		K_ITEM *i_username, *i_workername, *i_clientid, *i_enonce1, *i_useragent;
		char *secuserid;

		i_username = require_name("username", 1, NULL, reply, siz);
		if (!i_username)
			return strdup(reply);

		i_workername = require_name("workername", 1, NULL, reply, siz);
		if (!i_workername)
			return strdup(reply);

		i_clientid = require_name("clientid", 1, NULL, reply, siz);
		if (!i_clientid)
			return strdup(reply);

		i_enonce1 = require_name("enonce1", 1, NULL, reply, siz);
		if (!i_enonce1)
			return strdup(reply);

		i_useragent = require_name("useragent", 1, NULL, reply, siz);
		if (!i_useragent)
			return strdup(reply);

		conn = dbconnect();
		secuserid = auths_add(conn, DATA_TRANSFER(i_username)->data,
					    DATA_TRANSFER(i_workername)->data,
					    DATA_TRANSFER(i_clientid)->data,
					    DATA_TRANSFER(i_enonce1)->data,
					    DATA_TRANSFER(i_useragent)->data,
					    now, by, code, inet);
		PQfinish(conn);

		if (!secuserid) {
			STRNCPY(reply, "bad.DBE");
			return strdup(reply);
		}

		LOGDEBUG("added.%s.%s", DATA_TRANSFER(i_method)->data, secuserid);
		snprintf(reply, siz, "added.%s", secuserid);
		return strdup(reply);
	}

	STRNCPY(reply, "bad.method");
	return strdup(reply);
}

enum cmd_values {
	CMD_UNSET,
	CMD_REPLY, // Means something was wrong - send back reply
	CMD_SHUTDOWN,
	CMD_PING,
	CMD_LOGSHARE,
	CMD_AUTH,
	CMD_ADDUSER,
	CMD_CHKPASS,
	CMD_POOLSTAT,
	CMD_NEWID,
	CMD_PAYMENTS,
	CMD_END
};

#define ACCESS_POOL	"p"
#define ACCESS_SYSTEM	"s"
#define ACCESS_WEB	"w"
#define ACCESS_PROXY	"x"

static struct CMDS {
	enum cmd_values cmd_val;
	char *cmd_str;
	char *(*func)(char *, tv_t *, char *, char *, char *);
	char *access;
} cmds[] = {
	{ CMD_SHUTDOWN,	"shutdown",	NULL,		ACCESS_SYSTEM },
	{ CMD_PING,	"ping",		NULL,		ACCESS_SYSTEM ACCESS_WEB },
	// Workinfo, Shares and Shareerrors
	{ CMD_LOGSHARE,	"sharelog",	cmd_sharelog,	ACCESS_POOL },
	{ CMD_AUTH,	"authorise",	cmd_auth,	ACCESS_POOL },
	{ CMD_ADDUSER,	"adduser",	cmd_adduser,	ACCESS_WEB },
	{ CMD_CHKPASS,	"chkpass",	cmd_chkpass,	ACCESS_WEB },
	{ CMD_POOLSTAT,	"poolstats",	cmd_poolstats,	ACCESS_WEB },
	{ CMD_NEWID,	"newid",	cmd_newid,	ACCESS_SYSTEM },
	{ CMD_PAYMENTS,	"payments",	cmd_payments,	ACCESS_WEB },
	{ CMD_END,	NULL,		NULL,		NULL }
};

// TODO: size limits?
static enum cmd_values breakdown(char *buf, int *which_cmds, char *id)
{
	K_TREE_CTX ctx[1];
	K_ITEM *item;
	char *copy, *cmd, *data, *next, *eq;

	*which_cmds = CMD_UNSET;
	copy = strdup(buf);
	cmd = strchr(copy, '.');
	if (!cmd || !*cmd) {
		STRNCPYSIZ(id, copy, ID_SIZ);
		LOGINFO("Listener received invalid message: '%s'", buf);
		free(copy);
		return CMD_REPLY;
	}

	*(cmd++) = '\0';
	STRNCPYSIZ(id, copy, ID_SIZ);
	data = strchr(cmd, '.');
	if (data)
		*(data++) = '\0';

	for (*which_cmds = 0; cmds[*which_cmds].cmd_val != CMD_END; (*which_cmds)++) {
		if (strcasecmp(cmd, cmds[*which_cmds].cmd_str) == 0)
			break;
	}

	if (cmds[*which_cmds].cmd_val == CMD_END) {
		LOGINFO("Listener received unknown command: '%s'", buf);
		free(copy);
		return CMD_REPLY;
	}

	next = data;
	if (strncmp(next, JSON_TRANSFER, JSON_TRANSFER_LEN) == 0) {
		json_t *json_data;
		json_error_t err_val;
		void *json_iter;
		const char *json_key, *json_str;
		json_t *json_value;
		size_t siz;

		next += JSON_TRANSFER_LEN;
		json_data = json_loads(next, JSON_DISABLE_EOF_CHECK, &err_val);
		if (!json_data) {
			LOGINFO("Json decode error from command: '%s'", cmd);
			free(copy);
			return CMD_REPLY;
		}
		json_iter = json_object_iter(json_data);
		K_WLOCK(transfer_list);
		while (json_iter) {
			json_key = json_object_iter_key(json_iter);
			json_value = json_object_iter_value(json_iter);
			if (json_is_string(json_value) ||
			    json_is_integer(json_value) ||
			    json_is_real(json_value) ||
			    json_is_array(json_value)) {
				item = k_unlink_head(transfer_list);
				STRNCPY(DATA_TRANSFER(item)->name, json_key);

				if (json_is_string(json_value)) {
					json_str = json_string_value(json_value);
					siz = strlen(json_str);
					if (siz >= sizeof(DATA_TRANSFER(item)->value))
						DATA_TRANSFER(item)->data = strdup(json_str);
					else {
						STRNCPY(DATA_TRANSFER(item)->value, json_str);
						DATA_TRANSFER(item)->data = DATA_TRANSFER(item)->value;
					}
				} else if (json_is_integer(json_value)) {
					snprintf(DATA_TRANSFER(item)->value,
						 sizeof(DATA_TRANSFER(item)->value),
						 "%"PRId64,
						 (int64_t)json_integer_value(json_value));
					DATA_TRANSFER(item)->data = DATA_TRANSFER(item)->value;
				} else if (json_is_real(json_value)) {
					snprintf(DATA_TRANSFER(item)->value,
						 sizeof(DATA_TRANSFER(item)->value),
						 "%f", json_real_value(json_value));
					DATA_TRANSFER(item)->data = DATA_TRANSFER(item)->value;
				} else {
					/* Array - only one level array of strings for now (merkletree)
					 * ignore other data */
					size_t i, len, off, count = json_array_size(json_value);
					json_t *json_element;
					bool first = true;

					len = 1024;
					DATA_TRANSFER(item)->data = malloc(len);
					if (!(DATA_TRANSFER(item)->data))
						quithere(1, "malloc data (%d) OOM", (int)len);
					off = 0;
					for (i = 0; i < count; i++) {
						json_element = json_array_get(json_value, i);
						if (json_is_string(json_element)) {
							json_str = json_string_value(json_element);
							siz = strlen(json_str);
							if (first)
								first = false;
							else {
								APPEND_REALLOC(DATA_TRANSFER(item)->data,
										off, len, " ");
							}
							APPEND_REALLOC(DATA_TRANSFER(item)->data,
									off, len, json_str);
						}
					}
				}

				if (find_in_ktree(transfer_root, item, cmp_transfer, ctx)) {
					if (DATA_TRANSFER(item)->data != DATA_TRANSFER(item)->value)
						free(DATA_TRANSFER(item)->data);
					k_add_head(transfer_list, item);
				} else {
					transfer_root = add_to_ktree(transfer_root, item, cmp_transfer);
					k_add_head(transfer_store, item);
				}
			}
			json_iter = json_object_iter_next(json_data, json_iter);
		}
		K_WUNLOCK(transfer_list);
		json_decref(json_data);
	} else {
		K_WLOCK(transfer_list);
		while (next && *next) {
			data = next;
			next = strchr(data, 0x02);
			if (next)
				*(next++) = '\0';

			eq = strchr(data, '=');
			if (!eq)
				eq = "";
			else
				*(eq++) = '\0';

			item = k_unlink_head(transfer_list);
			STRNCPY(DATA_TRANSFER(item)->name, data);
			STRNCPY(DATA_TRANSFER(item)->value, eq);
			DATA_TRANSFER(item)->data = DATA_TRANSFER(item)->value;

			if (find_in_ktree(transfer_root, item, cmp_transfer, ctx)) {
				if (DATA_TRANSFER(item)->data != DATA_TRANSFER(item)->value)
					free(DATA_TRANSFER(item)->data);
				k_add_head(transfer_list, item);
			} else {
				transfer_root = add_to_ktree(transfer_root, item, cmp_transfer);
				k_add_head(transfer_store, item);
			}
		}
		K_WUNLOCK(transfer_list);
	}

	free(copy);
	return cmds[*which_cmds].cmd_val;
}

// TODO: equivalent of api_allow
static void *listener(void *arg)
{
	proc_instance_t *pi = (proc_instance_t *)arg;
	unixsock_t *us = &pi->us;
	char *end, *ans, *rep, *buf = NULL;
	char id[ID_SIZ+1], reply[1024+1];
	enum cmd_values cmd;
	int sockd, which_cmds;
	K_ITEM *item;
	size_t siz;
	tv_t now;

	rename_proc(pi->sockname);

	setup_data();

	while (true) {
		dealloc(buf);
		sockd = accept(us->sockd, NULL, NULL);
		if (sockd < 0) {
			LOGERR("Failed to accept on socket in listener");
			break;
		}

		cmd = CMD_UNSET;

		buf = recv_unix_msg(sockd);
		// Once we've read the message
		setnow(&now);
		if (buf) {
			end = buf + strlen(buf) - 1;
			// strip trailing \n and \r
			while (end >= buf && (*end == '\n' || *end == '\r'))
				*(end--) = '\0';
		}
		if (!buf || !*buf) {
			// An empty message wont get a reply
			if (!buf)
				LOGWARNING("Failed to get message in listener");
			else
				LOGWARNING("Empty message in listener");
		} else {
			cmd = breakdown(buf, &which_cmds, id);
			switch (cmd) {
				case CMD_REPLY:
					snprintf(reply, sizeof(reply), "%s.%ld.?", id, now.tv_sec);
					send_unix_msg(sockd, reply);
					break;
				case CMD_SHUTDOWN:
					LOGWARNING("Listener received shutdown message, terminating ckdb");
					snprintf(reply, sizeof(reply), "%s.%d.exiting", id, (int)now.tv_sec);
					send_unix_msg(sockd, reply);
					break;
				case CMD_PING:
					LOGDEBUG("Listener received ping request");
					snprintf(reply, sizeof(reply), "%s.%ld.pong", id, now.tv_sec);
					send_unix_msg(sockd, reply);
					break;
				default:
					// TODO: optionally get by/code/inet from transfer here instead?
					ans = cmds[which_cmds].func(id, &now, (char *)"code",
								    (char *)__func__,
								    (char *)"127.0.0.1");

					siz = strlen(ans) + strlen(id) + 32;
					rep = malloc(siz);
					snprintf(rep, siz, "%s.%ld.%s", id, now.tv_sec, ans);
					free(ans);
					ans = NULL;
					send_unix_msg(sockd, rep);
					free(rep);
					rep = NULL;
					break;
			}
		}
		close(sockd);

		if (cmd == CMD_SHUTDOWN)
			break;

		K_WLOCK(transfer_list);
		transfer_root = free_ktree(transfer_root, NULL);
		item = transfer_store->head;
		while (item) {
			if (DATA_TRANSFER(item)->data != DATA_TRANSFER(item)->value)
				free(DATA_TRANSFER(item)->data);
			item = item->next;
		}
		k_list_transfer_to_head(transfer_store, transfer_list);
		K_WUNLOCK(transfer_list);
	}

	dealloc(buf);
	close_unix_socket(us->sockd, us->path);
	return NULL;
}

int main(int argc, char **argv)
{
	struct sigaction handler;
	char buf[512];
	ckpool_t ckp;
	int c, ret;
	char *kill;

	memset(&ckp, 0, sizeof(ckp));
	ckp.loglevel = LOG_NOTICE;

	while ((c = getopt(argc, argv, "c:kl:n:p:s:u:")) != -1) {
		switch(c) {
			case 'c':
				ckp.config = optarg;
				break;
			case 'k':
				ckp.killold = true;
				break;
			case 'n':
				ckp.name = strdup(optarg);
				break;
			case 'l':
				ckp.loglevel = atoi(optarg);
				if (ckp.loglevel < LOG_EMERG || ckp.loglevel > LOG_DEBUG) {
					quit(1, "Invalid loglevel (range %d - %d): %d",
					     LOG_EMERG, LOG_DEBUG, ckp.loglevel);
				}
				break;
			case 's':
				ckp.socket_dir = strdup(optarg);
				break;
			case 'u':
				db_user = strdup(optarg);
				kill = optarg;
				while (*kill)
					*(kill++) = ' ';
				break;
			case 'p':
				db_pass = strdup(optarg);
				kill = optarg;
				if (*kill)
					*(kill++) = ' ';
				while (*kill)
					*(kill++) = '\0';
				break;
		}
	}
//	if (!db_pass)
//		zzz
	if (!db_user)
		db_user = "postgres";
	if (!ckp.name)
		ckp.name = "ckdb";
	snprintf(buf, 15, "%s", ckp.name);
	prctl(PR_SET_NAME, buf, 0, 0, 0);
	memset(buf, 0, 15);

	if (!ckp.config) {
		ckp.config = strdup(ckp.name);
		realloc_strcat(&ckp.config, ".conf");
	}

	if (!ckp.socket_dir) {
//		ckp.socket_dir = strdup("/tmp/");
		ckp.socket_dir = strdup("/opt/");
		realloc_strcat(&ckp.socket_dir, ckp.name);
	}
	trail_slash(&ckp.socket_dir);

	/* Ignore sigpipe */
	signal(SIGPIPE, SIG_IGN);

	ret = mkdir(ckp.socket_dir, 0700);
	if (ret && errno != EEXIST)
		quit(1, "Failed to make directory %s", ckp.socket_dir);

//	parse_config(&ckp);

	if (!ckp.logdir)
		ckp.logdir = strdup("logs");

	/* Create the log directory */
	trail_slash(&ckp.logdir);
	ret = mkdir(ckp.logdir, 0700);
	if (ret && errno != EEXIST)
		quit(1, "Failed to make log directory %s", ckp.logdir);

	/* Create the logfile */
	sprintf(buf, "%s%s.log", ckp.logdir, ckp.name);
	ckp.logfp = fopen(buf, "a");
	if (!ckp.logfp)
		quit(1, "Failed to open log file %s", buf);
	ckp.logfd = fileno(ckp.logfp);

	ckp.main.ckp = &ckp;
	ckp.main.processname = strdup("main");
	ckp.main.sockname = strdup("listener");
	write_namepid(&ckp.main);
	create_process_unixsock(&ckp.main);

	srand((unsigned int)time(NULL));
	create_pthread(&ckp.pth_listener, listener, &ckp.main);

	handler.sa_flags = 0;
	sigemptyset(&handler.sa_mask);
	sigaction(SIGTERM, &handler, NULL);
	sigaction(SIGINT, &handler, NULL);

	/* Shutdown from here if the listener is sent a shutdown message */
	join_pthread(ckp.pth_listener);

	clean_up(&ckp);

	return 0;
}