/****************************************************************
 *								*
 * Copyright (c) 2001-2019 Fidelity National Information	*
 * Services, Inc. and/or its subsidiaries. All rights reserved.	*
 *								*
 * Copyright (c) 2018-2019 YottaDB LLC and/or its subsidiaries.	*
 * All rights reserved.						*
 *								*
 *	This source code contains the intellectual property	*
 *	of its copyright holder(s), and is made available	*
 *	under a license.  If you do not know the terms of	*
 *	the license, please stop and do not read further.	*
 *								*
 ****************************************************************/

#include "mdef.h"

#include "gtm_string.h"
#include "gtm_limits.h"
#include <dlfcn.h>

#include "real_len.h"	/* for COPY_DLERR_MSG */
#include "lv_val.h"	/* needed for "fgncal.h" */
#include "fgncal.h"
#include "util.h"
#include "gtmmsg.h"
#include "error.h"
#include "restrict.h"	/* Needed for restrictions */
#include "have_crit.h"	/* Needed for defer interrupts */

error_def(ERR_DLLNOCLOSE);
error_def(ERR_DLLNOOPEN);
error_def(ERR_DLLNORTN);
error_def(ERR_RESTRICTEDOP);
error_def(ERR_TEXT);

GBLREF char		ydb_dist[GTM_PATH_MAX];

/* below comments applicable only to tru64 */
/* dlsym() is bound to return short pointer because of -taso loader flag
 * dlopen() returns a long pointer. All the callers of fgn_getpak() should take care of this.
 * dlclose() uses the handle generated above. So, the same semantics apply.
 * dlerror() returns a char pointer, which is again long.
 */
/* --- end of tru64 comments --- */

/* fgn_getinfo.c - external call lookup package
 *
 * Version Common to all *NIX platforms execept zOS
 * Note :- There is another zOS-specific version of the same file.
 */


/* Lookup package. Return package handle if success, NULL otherwise.
 * package_name - DLL name
 * msgtype - message severity of the errors reported if any.
 * Note - Errors are not issued if msgtype is SUCCESS, which is to be used if the callers are not
 * interested in message report and not willing to have condition handler overhead.
 */
void_ptr_t fgn_getpak(char *package_name, int msgtype)
{
	void_ptr_t 	ret_handle;
	char_ptr_t	dummy_err_str;
	char		err_str[MAX_ERRSTR_LEN]; /* needed as util_out_print doesn't handle 64bit pointers */
	char	 	librarypath[GTM_PATH_MAX], *lpath = NULL;
	intrpt_state_t	prev_intrpt_state;

	if ((RESTRICTED(library_load_path)) && (0 != memcmp(ydb_dist, package_name, STRLEN(ydb_dist))))
	{	/* Restrictions in place and the path is not somewhere under $ydb_dist */
		SNPRINTF(librarypath, SIZEOF(librarypath), GTM_PLUGIN_FMT_FULL, ydb_dist, strrchr(package_name, '/'));
		lpath = librarypath;
	} else
		lpath = package_name;
	DEFER_INTERRUPTS(INTRPT_IN_FUNC_WITH_MALLOC, prev_intrpt_state);
	ret_handle = dlopen(lpath, RTLD_LAZY);
	ENABLE_INTERRUPTS(INTRPT_IN_FUNC_WITH_MALLOC, prev_intrpt_state);
	if (NULL == ret_handle)
	{
		if (SUCCESS != msgtype)
		{
			assert(!(msgtype & ~SEV_MSK));
			if (RESTRICTED(library_load_path))
			{
				SNPRINTF(err_str, MAX_ERRSTR_LEN, "dlopen(%s)", lpath);
				if ((INFO == msgtype))
					gtm_putmsg_csa(CSA_ARG(NULL) VARLSTCNT(3)
							MAKE_MSG_TYPE(ERR_RESTRICTEDOP, msgtype), 1, err_str);
				else
					rts_error_csa(CSA_ARG(NULL) VARLSTCNT(3)
							MAKE_MSG_TYPE(ERR_RESTRICTEDOP, msgtype), 1, err_str);
			}
			COPY_DLLERR_MSG(dummy_err_str, err_str);
			if ((INFO == msgtype))
				gtm_putmsg_csa(CSA_ARG(NULL) VARLSTCNT(8) MAKE_MSG_TYPE(ERR_DLLNOOPEN, msgtype),
					2, LEN_AND_STR(lpath), ERR_TEXT, 2, LEN_AND_STR(err_str));
			else
				rts_error_csa(CSA_ARG(NULL) VARLSTCNT(8) MAKE_MSG_TYPE(ERR_DLLNOOPEN, msgtype),
					2, LEN_AND_STR(lpath), ERR_TEXT, 2, LEN_AND_STR(err_str));
		}
	}
	return ret_handle;
}

/* Lookup an external function. Return function address if success, NULL otherwise.
 * package_handle - DLL handle returned by fgn_getpak
 * entry_name - symbol name to be looked up
 * msgtype - message severity of the errors reported if any.
 * Note: If msgtype is SUCCESS, errors are not issued. It is useful if the callers are not
 * interested in message report and not willing to have condition handler overhead (eg. zro_search).
 */
fgnfnc fgn_getrtn(void_ptr_t package_handle, mstr *entry_name, int msgtype)
{
	void_ptr_t	sym_addr;
	char_ptr_t	dummy_err_str;
	void		*short_sym_addr;
	char		err_str[MAX_ERRSTR_LEN]; /* needed as util_out_print doesn't handle 64bit pointers */

	if (!(sym_addr = dlsym(package_handle, entry_name->addr)))
	{
		if (SUCCESS != msgtype)
		{
			assert(!(msgtype & ~SEV_MSK));
			COPY_DLLERR_MSG(dummy_err_str, err_str);
			if ((INFO == msgtype))
				gtm_putmsg_csa(CSA_ARG(NULL) VARLSTCNT(8) MAKE_MSG_TYPE(ERR_DLLNORTN, msgtype),
					2, LEN_AND_STR(entry_name->addr), ERR_TEXT, 2, LEN_AND_STR(err_str));
			else
				rts_error_csa(CSA_ARG(NULL) VARLSTCNT(8) MAKE_MSG_TYPE(ERR_DLLNORTN, msgtype),
					2, LEN_AND_STR(entry_name->addr), ERR_TEXT, 2, LEN_AND_STR(err_str));
		}
	} else
	{  /* Tru64 - dlsym() is bound to return short pointer because of ld -taso flag used for GT.M */
#ifdef	__osf__
		short_sym_addr = sym_addr;
		if (short_sym_addr != sym_addr)
		{
			sym_addr = NULL;
			/* always report an error irrespective of msgtype - since this code should never
			 * have executed and/or the DLL might need to be rebuilt with 32-bit options */
			rts_error_csa(CSA_ARG(NULL) VARLSTCNT(8) ERR_DLLNORTN, 2, LEN_AND_STR(entry_name->addr),
				ERR_TEXT, 2, LEN_AND_LIT("Symbol is loaded above the lower 31-bit address space"));
		}
#endif
	}
	return (fgnfnc)sym_addr;
}

void fgn_closepak(void_ptr_t package_handle, int msgtype)
{
	char_ptr_t      dummy_err_str;
  	int 		status;
	char		err_str[MAX_ERRSTR_LEN];

	status = dlclose(package_handle);
	if (0 != status && SUCCESS != msgtype)
	{
		assert(!(msgtype & ~SEV_MSK));
		COPY_DLLERR_MSG(dummy_err_str, err_str);
		if ((INFO == msgtype))
			gtm_putmsg_csa(CSA_ARG(NULL) VARLSTCNT(6) MAKE_MSG_TYPE(ERR_DLLNOCLOSE, msgtype), 0,
				ERR_TEXT, 2, LEN_AND_STR(err_str));
			else
			rts_error_csa(CSA_ARG(NULL) VARLSTCNT(6) MAKE_MSG_TYPE(ERR_DLLNOCLOSE, msgtype), 0,
				ERR_TEXT, 2, LEN_AND_STR(err_str));
	}
}
