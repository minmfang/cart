/* Copyright (C) 2016-2018 Intel Corporation
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted for any purpose (including commercial purposes)
 * provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions, and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions, and the following disclaimer in the
 *    documentation and/or materials provided with the distribution.
 *
 * 3. In addition, redistributions of modified forms of the source or binary
 *    code must carry prominent notices stating that the original code was
 *    changed and the date of the change.
 *
 *  4. All publications or advertising materials mentioning features or use of
 *     this software are asked, but not required, to acknowledge that it was
 *     developed by Intel Corporation and credit the contributors.
 *
 * 5. Neither the name of Intel Corporation, nor the name of any Contributor
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
/**
 * This file is part of gurt, it implements the debug subsystem based on clog.
 */

#include <stdlib.h>
#include <stdio.h>

#include <gurt/common.h>

static pthread_mutex_t d_log_lock = PTHREAD_MUTEX_INITIALIZER;
static int d_log_refcount;

int d_misc_logfac;
int d_mem_logfac;

/*
 * Debug bits for common logic paths, can only have up to 16 different bits.
 */
uint64_t DB_ANY; /** generic messages, no classification */
/** function trace, tree/hash/lru operations, a very expensive one */
uint64_t DB_TRACE;
uint64_t DB_MEM; /**< memory operation */
uint64_t DB_NET; /**< network operation */
uint64_t DB_IO;	/**< object I/O */
uint64_t DB_TEST; /**< test programs */
/**
 * all of masks - can be used to set all streams, or used to log specific
 * messages by default.
 */
uint64_t DB_ALL; /** < = DLOG_DBG */
/** Configurable debug bits (project-specific) */
static uint64_t DB_OPT1;
static uint64_t DB_OPT2;
static uint64_t DB_OPT3;
static uint64_t DB_OPT4;
static uint64_t DB_OPT5;
static uint64_t DB_OPT6;
static uint64_t DB_OPT7;
static uint64_t DB_OPT8;
static uint64_t DB_OPT9;
static uint64_t DB_OPT10;

#define DBG_ENV_MAX_LEN	(32)

#define DBG_DICT_ENTRY(bit, name)					\
	{ .db_bit = bit, .db_name = name, .db_name_size = sizeof(name),	\
	  .db_lname = NULL, .db_lname_size = 0 }

struct d_debug_bit d_dbg_bit_dict[] = {
	/* load common debug bits into dict */
	DBG_DICT_ENTRY(&DB_ANY, "any"),
	DBG_DICT_ENTRY(&DB_ALL, "all"),
	DBG_DICT_ENTRY(&DB_MEM, "mem"),
	DBG_DICT_ENTRY(&DB_NET, "net"),
	DBG_DICT_ENTRY(&DB_IO, "io"),
	DBG_DICT_ENTRY(&DB_TRACE, "trace"),
	DBG_DICT_ENTRY(&DB_TEST, "test"),
	/* set by d_log_dbg_bit_alloc() */
	DBG_DICT_ENTRY(&DB_OPT1, NULL),
	DBG_DICT_ENTRY(&DB_OPT2, NULL),
	DBG_DICT_ENTRY(&DB_OPT3, NULL),
	DBG_DICT_ENTRY(&DB_OPT4, NULL),
	DBG_DICT_ENTRY(&DB_OPT5, NULL),
	DBG_DICT_ENTRY(&DB_OPT6, NULL),
	DBG_DICT_ENTRY(&DB_OPT7, NULL),
	DBG_DICT_ENTRY(&DB_OPT8, NULL),
	DBG_DICT_ENTRY(&DB_OPT9, NULL),
	DBG_DICT_ENTRY(&DB_OPT10, NULL),

};

#define NUM_DBG_BIT_ENTRIES	ARRAY_SIZE(d_dbg_bit_dict)

#define DBG_GRP_DICT_ENTRY()					\
	{ .dg_mask = 0, .dg_name = NULL, .dg_name_size = 0 }

struct d_debug_grp d_dbg_grp_dict[] = {
	DBG_GRP_DICT_ENTRY(),
	DBG_GRP_DICT_ENTRY(),
	DBG_GRP_DICT_ENTRY(),
	DBG_GRP_DICT_ENTRY(),
	DBG_GRP_DICT_ENTRY(),
	DBG_GRP_DICT_ENTRY(),
	DBG_GRP_DICT_ENTRY(),
	DBG_GRP_DICT_ENTRY(),
	DBG_GRP_DICT_ENTRY(),
	DBG_GRP_DICT_ENTRY(),
};

#define NUM_DBG_GRP_ENTRIES	ARRAY_SIZE(d_dbg_grp_dict)

#define PRI_DICT_ENTRY(prio, name)	\
	{ .dd_prio = prio, .dd_name = name, .dd_name_size = sizeof(name) }

static struct d_debug_priority d_dbg_prio_dict[] = {
	PRI_DICT_ENTRY(DLOG_INFO, "info"),
	PRI_DICT_ENTRY(DLOG_NOTE, "note"),
	PRI_DICT_ENTRY(DLOG_WARN, "warn"),
	PRI_DICT_ENTRY(DLOG_ERR, "err"),
	PRI_DICT_ENTRY(DLOG_CRIT, "crit"),
	PRI_DICT_ENTRY(DLOG_EMERG, "fatal"),
};

#define NUM_DBG_PRIO_ENTRIES	ARRAY_SIZE(d_dbg_prio_dict)

struct d_debug_data d_dbglog_data = {
	/* count of alloc'd debug bits */
	.dbg_bit_cnt		= 0,
	/* count of alloc'd debug groups */
	.dbg_grp_cnt		= 0,
	/* 0 means we should use the mask provided by facilities */
	.dd_mask		= 0,
	/* optional priority output to stderr */
	.dd_prio_err		= 0,
};

#define BIT_CNT_TO_BIT_MASK(cnt)	(1 << (DLOG_DPRISHIFT + cnt))

/**
 * Allocate optional debug bit, register name and return available bit
 *
 * \param[in]	name	debug mask short name
 * \param[in]	lname	debug mask long name
 * \param[out]	dbgbit	alloc'd debug bit
 *
 * \return		0 on success, -1 on error
 */
int
d_log_dbg_bit_alloc(uint64_t *dbgbit, char *name, char *lname)
{
	size_t		   name_len;
	size_t		   lname_len;
	int		   i;
	uint64_t	   bit = 0;
	struct d_debug_bit *d;

	if (name == NULL || dbgbit == NULL)
		return -1;

	name_len = strlen(name);
	if (lname != NULL)
		lname_len = strlen(lname);
	else
		lname_len = 0;

	/**
	 * Allocate debug bit in gurt for given debug mask name.
	 * Currently only 10 configurable debug mask options.
	 * dbg_bit_cnt = [0-15]
	 */
	if (d_dbglog_data.dbg_bit_cnt >= (NUM_DBG_BIT_ENTRIES - 1)) {
		D_PRINT_ERR("Cannot allocate debug bit, all available debug "
			    "mask bits currently allocated.\n");
		return -1;
	}

	/**
	 * DB_ALL = DLOG_DBG, does not require a specific bit
	 */
	if (strncasecmp(name, DB_ALL_BITS, name_len) != 0) {
		bit = BIT_CNT_TO_BIT_MASK(d_dbglog_data.dbg_bit_cnt);
		d_dbglog_data.dbg_bit_cnt++;
	}

	for (i = 0; i < NUM_DBG_BIT_ENTRIES; i++) {
		/**
		 * Debug bit name already present in struct,
		 * still need to assign available bit
		 */
		d = &d_dbg_bit_dict[i];
		if (d->db_name != NULL) {
			if (strncasecmp(d->db_name, name, name_len) == 0) {
				if (*(d->db_bit) == 0) {
					/* DB_ALL = DLOG_DBG */
					if (strncasecmp(name, DB_ALL_BITS,
							name_len) == 0)
						*dbgbit = DLOG_DBG;
					else
						*dbgbit = bit;
					*(d->db_bit) = bit;
				} else /* debug bit already assigned */
					*dbgbit = *(d->db_bit);
				return 0;
			}
		/* Allocate configurable debug bit along with name */
		} else {
			d->db_name = name;
			d->db_lname = lname;
			d->db_name_size = name_len;
			d->db_lname_size = lname_len;
			*(d->db_bit) = bit;

			*dbgbit = bit;
			return 0;
		}
	}

	return -1;
}

/**
 * Create an identifier/group name for muliple debug bits
 *
 * \param[in]	grpname		debug mask group name
 * \param[in]	dbgmask		group mask
 *
 * \return			0 on success, -1 on error
 */
int
d_log_dbg_grp_alloc(uint64_t dbgmask, char *grpname)
{
	int		   i;
	size_t		   name_len;
	struct d_debug_grp *g;

	if (grpname == NULL || dbgmask == 0)
		return -1;

	name_len = strlen(grpname);

	/**
	 * Allocate debug group in gurt for given debug mask name.
	 * Currently only 10 configurable debug group options.
	 */
	if (d_dbglog_data.dbg_grp_cnt > NUM_DBG_GRP_ENTRIES) {
		D_PRINT_ERR("Cannot allocate debug group, all available debug "
			    "group currently allocated.\n");
		return -1;
	}
	d_dbglog_data.dbg_grp_cnt++;

	for (i = 0; i < NUM_DBG_GRP_ENTRIES; i++) {
		g = &d_dbg_grp_dict[i];
		if (g->dg_name == NULL) {
			g->dg_name = grpname;
			g->dg_name_size = name_len;
			g->dg_mask = dbgmask;
			return 0;
		}
	}

	/* no empty group entries available */
	return -1;
}

/** Load the priority stderr from the environment variable. */
static void
debug_prio_err_load_env(void)
{
	char	*env;
	int	i;

	env = getenv(DD_STDERR_ENV);
	if (env == NULL)
		return;

	for (i = 0; i < NUM_DBG_PRIO_ENTRIES; i++) {
		if (d_dbg_prio_dict[i].dd_name != NULL &&
		    strncasecmp(env, d_dbg_prio_dict[i].dd_name,
				d_dbg_prio_dict[i].dd_name_size) == 0) {
			d_dbglog_data.dd_prio_err = d_dbg_prio_dict[i].dd_prio;
			break;
		}
	}
	/* invalid DD_STDERR option */
	if (d_dbglog_data.dd_prio_err == 0)
		D_PRINT_ERR("DD_STDERR = %s - invalid option\n", env);
}

/** Load the debug mask from the environment variable. */
static void
debug_mask_load_env(void)
{
	char		    *mask_env;
	char		    *mask_str;
	char		    *cur;
	int		    i;
	struct d_debug_bit *d;
	struct d_debug_grp *g;

	mask_env = getenv(DD_MASK_ENV);
	if (mask_env == NULL)
		return;

	D_STRNDUP(mask_str, mask_env, DBG_ENV_MAX_LEN);
	if (mask_str == NULL) {
		D_PRINT_ERR("D_STRNDUP of debug mask failed");
		return;
	}

	cur = strtok(mask_str, DD_SEP);
	while (cur != NULL) {
		for (i = 0; i < NUM_DBG_BIT_ENTRIES; i++) {
			d = &d_dbg_bit_dict[i];
			if (d->db_name != NULL &&
			    strncasecmp(cur, d->db_name,
					d->db_name_size) == 0) {
				d_dbglog_data.dd_mask |= *(d->db_bit);
				break;
			}
			if (d->db_lname != NULL &&
			    strncasecmp(cur, d->db_lname,
					d->db_lname_size) == 0) {
				d_dbglog_data.dd_mask |= *(d->db_bit);
				break;
			}
		}
		/* check if DD_MASK entry is a group name */
		for (i = 0; i < NUM_DBG_GRP_ENTRIES; i++) {
			g = &d_dbg_grp_dict[i];
			if (g->dg_name != NULL &&
			    strncasecmp(cur, g->dg_name,
					g->dg_name_size) == 0) {
				d_dbglog_data.dd_mask |= g->dg_mask;
				break;
			}
		}
		cur = strtok(NULL, DD_SEP);
	}
	D_FREE(mask_str);
}

void
d_log_sync_mask(void)
{
	static char *log_mask;

	D_MUTEX_LOCK(&d_log_lock);

	/* Load debug mask environment (DD_MASK); only the facility log mask
	 * will be returned and debug mask will be set iff fac mask = DEBUG
	 * and dd_mask is not 0.
	 */
	debug_mask_load_env();

	/* load facility mask environment (D_LOG_MASK) */
	log_mask = getenv(D_LOG_MASK_ENV);
	if (log_mask != NULL)
		d_log_setmasks(log_mask, -1);

	D_MUTEX_UNLOCK(&d_log_lock);
}

/* this macro contains a return statement */
#define D_ADD_LOG_FAC(name, aname, lname)				\
	do {								\
		d_##name##_logfac = d_add_log_facility(aname, lname);	\
		if (d_##name##_logfac < 0) {				\
			D_ERROR("d_add_log_facility failed, "		\
				"d_##name##__logfac: %d.\n",		\
				 d_##name##_logfac);			\
			return -DER_UNINIT;				\
		}							\
	} while (0)

#define D_INIT_LOG_FAC(name, aname, lname)				\
	d_init_log_facility(&d_##name##_logfac, aname, lname)

/**
 * Setup the clog facility names and mask.
 *
 * \param[in] masks	 masks in d_log_setmasks() format, or NULL.
 */

static inline int
setup_clog_facnamemask(void)
{
	int rc;

	/* add gurt internally used log facilities */
	rc = D_INIT_LOG_FAC(misc, "MISC", "misc");
	if (rc != 0) {
		D_PRINT_ERR("MISC log facility failed to init; rc=%d\n", rc);
		return rc;
	}

	rc = D_INIT_LOG_FAC(mem, "MEM", "memory");
	if (rc != 0) {
		D_PRINT_ERR("MEM log facility failed to init; rc=%d\n", rc);
		return rc;
	}

	return 0;
}

/**
 * Setup the debug names and mask bits.
 *
 * \return	 -DER_UNINIT on error, 0 on success
 */
static inline int
setup_dbg_namebit(void)
{
	int		   i;
	int		   rc;
	uint64_t	   allocd_dbg_bit;
	struct d_debug_bit *d;

	for (i = 0; i < NUM_DBG_BIT_ENTRIES; i++) {
		d = &d_dbg_bit_dict[i];
		if (d->db_name != NULL) {
			/* register gurt debug bit masks */
			rc = d_log_dbg_bit_alloc(&allocd_dbg_bit, d->db_name,
						 d->db_lname);
			if (rc < 0) {
				D_PRINT_ERR("Error allocating debug bit for %s",
					    d->db_name);
				return -DER_UNINIT;
			}

			*(d->db_bit) = allocd_dbg_bit;
		}
	}

	return 0;
}

int
d_log_init_adv(char *log_tag, char *log_file, unsigned int flavor,
		 uint64_t def_mask, uint64_t err_mask)
{
	int rc = 0;

	D_MUTEX_LOCK(&d_log_lock);
	d_log_refcount++;
	if (d_log_refcount > 1) /* Already initialized */
		D_GOTO(out, 0);

	/* Load priority error from environment variable (DD_STDERR)
	 * A Priority error will be output to stderr by the debug system.
	 */
	debug_prio_err_load_env();
	if (d_dbglog_data.dd_prio_err != 0)
		err_mask = d_dbglog_data.dd_prio_err;

	rc = d_log_open(log_tag, 0, def_mask, err_mask, log_file, flavor);
	if (rc != 0) {
		D_PRINT_ERR("d_log_open failed: %d\n", rc);
		D_GOTO(out, rc = -DER_UNINIT);
	}

	rc = setup_dbg_namebit();
	if (rc != 0)
		D_GOTO(out, rc = -DER_UNINIT);

	rc = setup_clog_facnamemask();
	if (rc != 0)
		D_GOTO(out, rc = -DER_UNINIT);
out:
	if (rc != 0) {
		D_PRINT_ERR("ddebug_init failed, rc: %d.\n", rc);
		d_log_refcount--;
	}
	D_MUTEX_UNLOCK(&d_log_lock);
	return rc;
}

int
d_log_init(void)
{
	char *log_file;
	int flags = DLOG_FLV_LOGPID | DLOG_FLV_FAC | DLOG_FLV_TAG;

	log_file = getenv(D_LOG_FILE_ENV);
	if (log_file == NULL || strlen(log_file) == 0) {
		flags |= DLOG_FLV_STDOUT;
		log_file = NULL;
	}

	return d_log_init_adv("CaRT", log_file, flags, DLOG_WARN, DLOG_EMERG);
}

void d_log_fini(void)
{
	D_ASSERT(d_log_refcount > 0);

	D_MUTEX_LOCK(&d_log_lock);
	d_log_refcount--;
	if (d_log_refcount == 0)
		d_log_close();
	D_MUTEX_UNLOCK(&d_log_lock);
}

/**
 * Get allocated debug mask bit from bit name.
 *
 * \param[in] bitname	short name of debug bit
 * \param[out] dbgbit	bit mask allocated for given name
 *
 * \return		0 on success, -1 on error
 */
int d_log_getdbgbit(uint64_t *dbgbit, char *bitname)
{
	int		   i;
	int		   num_dbg_bit_entries;
	struct d_debug_bit *d;

	if (bitname == NULL)
		return 0;

	num_dbg_bit_entries = ARRAY_SIZE(d_dbg_bit_dict);
	for (i = 0; i < num_dbg_bit_entries; i++) {
		d = &d_dbg_bit_dict[i];
		if (d->db_name != NULL &&
		    strncasecmp(bitname, d->db_name, d->db_name_size) == 0) {
			*dbgbit = *(d->db_bit);
			return 0;
		}
	}

	return -1;
}
