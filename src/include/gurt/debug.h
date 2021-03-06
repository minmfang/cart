/* Copyright (C) 2017-2018 Intel Corporation
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

#ifndef __GURT_DEBUG_H__
#define __GURT_DEBUG_H__

#include <stddef.h>
#include <assert.h>

#include <gurt/dlog.h>
#include <gurt/errno.h>

/**
 * \file
 * Debug macros and functions
 */

/** @addtogroup GURT_DEBUG
 * @{
 */
#define DD_FAC(name)	(d_##name##_logfac)

extern int DD_FAC(misc);
extern int DD_FAC(mem);

#ifndef D_LOGFAC
#define D_LOGFAC	DD_FAC(misc)
#endif

extern uint64_t DB_ANY;
extern uint64_t DB_TRACE;
extern uint64_t DB_MEM;
extern uint64_t DB_NET;
extern uint64_t DB_IO;
extern uint64_t DB_TEST;
extern uint64_t DB_ALL;

#define DB_ALL_BITS	"all"

#define D_LOG_FILE_ENV	"D_LOG_FILE"	/**< Env to specify log file */
#define D_LOG_MASK_ENV	"D_LOG_MASK"	/**< Env to specify log mask */

/**
 * D_DEBUG/D_ERROR etc can-only be used when clog enabled. User can define other
 * similar macros using different subsystem and log-level, for example:
 * \#define DSR_DEBUG(...) d_log(DSR_DEBUG, ...)
 */
#define D_DEBUG(mask, fmt, ...)						\
	d_log((mask) | D_LOGFAC,					\
	     "%s:%d %s() " fmt, __FILE__, __LINE__, __func__,		\
	     ##__VA_ARGS__)

/** macros to output logs which are more important than D_DEBUG */
#define D_INFO(fmt, ...)	D_DEBUG(DLOG_INFO, fmt, ## __VA_ARGS__)
#define D_NOTE(fmt, ...)	D_DEBUG(DLOG_NOTE, fmt, ## __VA_ARGS__)
#define D_WARN(fmt, ...)	D_DEBUG(DLOG_WARN, fmt, ## __VA_ARGS__)
#define D_ERROR(fmt, ...)	D_DEBUG(DLOG_ERR, fmt, ## __VA_ARGS__)
#define D_CRIT(fmt, ...)	D_DEBUG(DLOG_CRIT, fmt, ## __VA_ARGS__)
#define D_FATAL(fmt, ...)	D_DEBUG(DLOG_EMERG, fmt, ## __VA_ARGS__)

/**
 * Add a new log facility.
 *
 * \param[in] aname	abbr. name for the facility, for example DSR.
 * \param[ib] lname	long name for the facility, for example DSR.
 *
 * \return		new positive facility number on success, -1 on error.
 */
static inline int
d_add_log_facility(const char *aname, const char *lname)
{
	return d_log_allocfacility(aname, lname);
}

/**
 * Add a new log facility.
 *
 * \param[out fac	facility number to be returned
 * \param[in] aname	abbr. name for the facility, for example DSR.
 * \param[ib] lname	long name for the facility, for example DSR.
 *
 * \return		0 on success, -1 on error.
 */
static inline int
d_init_log_facility(int *fac, const char *aname, const char *lname)
{
	assert(fac != NULL);

	*fac = d_add_log_facility(aname, lname);
	if (*fac < 0) {
		D_ERROR("d_add_log_facility failed, *fac: %d\n", *fac);
		return -DER_UNINIT;
	}

	return DER_SUCCESS;
}

/**
 * Get allocated debug bit for the given debug bit name.
 *
 * \param[in] bitname	short name for given bit
 * \param[out] dbgbit	allocated bit mask
 *
 * \return		0 on success, -1 on error
 */
int d_log_getdbgbit(uint64_t *dbgbit, char *bitname);

/**
 * D_PRINT_ERR must be used for any error logging before clog is enabled or
 * after it is disabled
 */
#define D_PRINT_ERR(fmt, ...)						\
	do {								\
		fprintf(stderr, "%s:%d:%d:%s() " fmt, __FILE__,		\
			getpid(), __LINE__, __func__, ## __VA_ARGS__);	\
		fflush(stderr);						\
	} while (0)

/**
 * D_PRINT can be used for output to stdout with or without clog being enabled
 */
#define D_PRINT(fmt, ...)						\
	do {								\
		fprintf(stdout, fmt, ## __VA_ARGS__);			\
		fflush(stdout);						\
	} while (0)

#define D_ASSERT(e)	assert(e)

#define D_ASSERTF(cond, fmt, ...)					\
do {									\
	if (!(cond))							\
		D_FATAL(fmt, ## __VA_ARGS__);				\
	assert(cond);							\
} while (0)

#define D_CASSERT(cond)							\
	do {switch (1) {case (cond): case 0: break; } } while (0)

#define DF_U64		"%" PRIu64
#define DF_X64		"%" PRIx64

/** @}
 */
#endif /* __GURT_DEBUG_H__ */
