/****************************************************************
 *								*
 *	Copyright 2001, 2007 Fidelity Information Services, Inc	*
 *								*
 *	This source code contains the intellectual property	*
 *	of its copyright holder(s), and is made available	*
 *	under a license.  If you do not know the terms of	*
 *	the license, please stop and do not read further.	*
 *								*
 ****************************************************************/

#include "mdef.h"
#include "io.h"
#include "iottdef.h"
#include "iomtdef.h"
#include "io_params.h"

void
iomt_skiprecord (io_desc *dev, int count)
{
	d_mt_struct    *mt_ptr;
	error_def (ERR_MTRDTHENWRT);

	mt_ptr = (d_mt_struct *) dev->dev_sp;
	if (mt_ptr->last_op == mt_write)
	{
		if (count > 0)
			rts_error (VARLSTCNT (1) ERR_MTRDTHENWRT);
		iomt_flush (dev);
		iomt_eof (dev);

#ifdef UNIX
		if (mt_ptr->cap.req_extra_filemark)
		{
		    iomt_eof (dev);
		    iomt_qio (dev, IO_SKIPFILE, (unsigned int)-2);
		}
		else
#endif
		    iomt_qio (dev, IO_SKIPFILE, (unsigned int)-1);

	} else if (mt_ptr->last_op == mt_eof)
	{
		if (count > 0)
			rts_error (VARLSTCNT (1) ERR_MTRDTHENWRT);

#ifdef UNIX
		if (mt_ptr->cap.req_extra_filemark)
		{
		    iomt_eof (dev);
		    iomt_qio (dev, IO_SKIPFILE, (unsigned int)-1);
		}
#endif
	} else if (mt_ptr->last_op == mt_eof2 && count > 0)
	{
		rts_error (VARLSTCNT (1) ERR_MTRDTHENWRT);
	}
	iomt_qio (dev, IO_SKIPRECORD, count);
	mt_ptr->last_op = mt_null;
	return;
}
