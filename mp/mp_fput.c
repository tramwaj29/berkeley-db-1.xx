/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 1996, 1997
 *	Sleepycat Software.  All rights reserved.
 */
#include "config.h"

#ifndef lint
static const char sccsid[] = "@(#)mp_fput.c	10.17 (Sleepycat) 12/20/97";
#endif /* not lint */

#ifndef NO_SYSTEM_INCLUDES
#include <sys/types.h>

#include <errno.h>
#include <stdlib.h>
#endif

#include "db_int.h"
#include "shqueue.h"
#include "db_shash.h"
#include "mp.h"
#include "common_ext.h"

/*
 * memp_fput --
 *	Mpool file put function.
 */
int
memp_fput(dbmfp, pgaddr, flags)
	DB_MPOOLFILE *dbmfp;
	void *pgaddr;
	int flags;
{
	BH *bhp;
	DB_MPOOL *dbmp;
	MPOOL *mp;
	MPOOLFILE *mfp;
	int wrote, ret;

	dbmp = dbmfp->dbmp;
	mp = dbmp->mp;

	/* Validate arguments. */
	if (flags) {
		if ((ret = __db_fchk(dbmp->dbenv, "memp_fput", flags,
		    DB_MPOOL_CLEAN | DB_MPOOL_DIRTY | DB_MPOOL_DISCARD)) != 0)
			return (ret);
		if ((ret = __db_fcchk(dbmp->dbenv, "memp_fput",
		    flags, DB_MPOOL_CLEAN, DB_MPOOL_DIRTY)) != 0)
			return (ret);

		if (LF_ISSET(DB_MPOOL_DIRTY) && F_ISSET(dbmfp, MP_READONLY)) {
			__db_err(dbmp->dbenv,
			    "%s: dirty flag set for readonly file page",
			    __memp_fn(dbmfp));
			return (EACCES);
		}
	}

	/* Decrement the pinned reference count. */
	LOCKHANDLE(dbmp, dbmfp->mutexp);
	if (dbmfp->pinref == 0)
		__db_err(dbmp->dbenv,
		    "%s: put: more blocks returned than retrieved",
		    __memp_fn(dbmfp));
	else
		--dbmfp->pinref;
	UNLOCKHANDLE(dbmp, dbmfp->mutexp);

	/*
	 * If we're mapping the file, there's nothing to do.  Because we can
	 * quit mapping at any time, we have to check on each buffer to see
	 * if it's in the map region.
	 */
	if (dbmfp->addr != NULL && pgaddr >= dbmfp->addr &&
	    (u_int8_t *)pgaddr <= (u_int8_t *)dbmfp->addr + dbmfp->len)
		return (0);

	/* Convert the page address to a buffer header. */
	bhp = (BH *)((u_int8_t *)pgaddr - SSZA(BH, buf));

	LOCKREGION(dbmp);

	/* Set/clear the page bits. */
	if (LF_ISSET(DB_MPOOL_CLEAN) && F_ISSET(bhp, BH_DIRTY)) {
		++mp->stat.st_page_clean;
		--mp->stat.st_page_dirty;
		F_CLR(bhp, BH_DIRTY);
	}
	if (LF_ISSET(DB_MPOOL_DIRTY) && !F_ISSET(bhp, BH_DIRTY)) {
		--mp->stat.st_page_clean;
		++mp->stat.st_page_dirty;
		F_SET(bhp, BH_DIRTY);
	}
	if (LF_ISSET(DB_MPOOL_DISCARD))
		F_SET(bhp, BH_DISCARD);

	/*
	 * If more than one reference to the page, we're done.  Ignore discard
	 * flags (for now) and leave it at its position in the LRU chain.  The
	 * rest gets done at last reference close.
	 */
#ifdef DEBUG
	if (bhp->ref == 0) {
		__db_err(dbmp->dbenv,
    "Unpinned page returned: reference count on page %lu went negative.",
		    (u_long)bhp->pgno);
		abort();
	}
#endif
	if (--bhp->ref > 0) {
		UNLOCKREGION(dbmp);
		return (0);
	}

	/* Move the buffer to the head/tail of the LRU chain. */
	SH_TAILQ_REMOVE(&mp->bhq, bhp, q, __bh);
	if (F_ISSET(bhp, BH_DISCARD))
		SH_TAILQ_INSERT_HEAD(&mp->bhq, bhp, q, __bh);
	else
		SH_TAILQ_INSERT_TAIL(&mp->bhq, bhp, q);

	/*
	 * If this buffer is scheduled for writing because of a checkpoint,
	 * write it now.  If we can't write it, set a flag so that the next
	 * time the memp_sync function is called we try writing it there,
	 * as the checkpoint application better be able to write all of the
	 * files.
	 */
	if (F_ISSET(bhp, BH_WRITE))
		if (F_ISSET(bhp, BH_DIRTY)) {
			if (__memp_bhwrite(dbmp,
			    dbmfp->mfp, bhp, NULL, &wrote) != 0 || !wrote)
				F_SET(mp, MP_LSN_RETRY);
		} else {
			F_CLR(bhp, BH_WRITE);

			mfp = R_ADDR(dbmp, bhp->mf_offset);
			--mfp->lsn_cnt;

			--mp->lsn_cnt;
		}

	UNLOCKREGION(dbmp);
	return (0);
}
