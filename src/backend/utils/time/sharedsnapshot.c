/*-------------------------------------------------------------------------
 *
 * sharedsnapshot.c
 *	  GPDB shared snapshot management.
 *
 * Portions Copyright (c) 1996-2009, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/utils/time/sharedsnapshot.c
 *
 * In Cloudberry, as part of slice plans, many postgres processes (qExecs, QE)
 * running on a single segment database as part of the same user's SQL
 * statement. All of the qExecs that belong to a particular user on a
 * particular segment database need to have consistent visibility. Idea used
 * is called "Shared Local Snapshot". Shared-memory data structure
 * SharedSnapshotSlot shares session and transaction information among
 * session's gang processes on a particular database instance. The processes
 * are called a SegMate process group.
 *
 * A SegMate process group is a QE (Query Executor) Writer process and 0, 1 or
 * more QE Reader processes. Cloudberry needed to invent a SegMate sharing
 * mechanism because in Postgres there is only 1 backend and most needed
 * information is simply available in private memory. With Cloudberry session
 * parallelism on database instances, we need to have a way to share
 * not-yet-committed session information among the SegMates. This information
 * includes transaction snapshots, sub-transaction status, so-called combo-cid
 * mapping, etc.
 *
 * An example: the QE readers need to use the same snapshot and command number
 * information as the QE writer so they see the current data written by the QE
 * writer. During a transaction, the QE Writer writes new data into the
 * shared-memory buffered cache. Later in that same transaction, QE Readers
 * will need to recognize which tuples in the shared-memory buffered cache are
 * for its session's transaction to perform correctly.
 *
 * Another example: the QE readers need to know which sub-transactions are
 * active or committed for a session's transaction so they can properly read
 * sub-transaction data written by the QE writer for the transaction.
 *
 * So, the theme is to share private, not-yet-committed session transaction
 * information with the QE Readers so the SegMate process group can all work
 * on the transaction correctly. [We mostly think of QE Writers/Readers being
 * on the segments. However, masters have special purpose QE Reader called the
 * Entry DB Singleton. So, the SegMate module also works on the master.]
 *
 * Each shared snapshot is local only to the segment database. High level
 * Writer gang member establishes a local transaction, acquires the slot in
 * shared snapshot shmem space and copies the snapshot information into shared
 * memory where the other qExecs that are segmates can find it. Following
 * section convers details on how shared memory initialization happens, who
 * writes the snapshot, how its controlled how/when the readers can read the
 * snapshot, locking, etc..
 *
 * Shared Memory Initialization: Shared memory is setup by the postmaster. One
 * slot for every user connection on the QD is kind of needed to store a data
 * structure for a set of segmates to store their snapshot information. In
 * each slot QE writer stores information defined by SharedSnapshotSlot.
 *
 * PQsendMppStatement: Is the same as PQsendQuery except that it also sends a
 * serialized snapshot and xid. postgres.c has been modified to accept this
 * new protocol message. It does pretty much the same stuff as it would for a
 * 'Q' (normal query) except it unpacks the snapshot and xid from the QD and
 * stores it away. All QEs get sent in a QD snapshot during statement
 * dispatch.
 *
 * Global Session ID: The shared snapshot shared memory is split into slots. A
 * set of segmates for a given user requires a single slot. The snapshot
 * information and other information is stored within the snapshot. A unique
 * session id identifies all the components in the system that are working for
 * a single user session. Within a single segment database this essentially
 * defines what it means to be "segmates."  The shared snapshot slot is
 * identified by this unique session id. The unique session id is sent in from
 * the QD as a GUC called "mpp_session_id". So the slot's field "slotid" will
 * store the "mpp_session_id" that WRITER to the slot will use. Readers of the
 * slot will find the correct slot by finding the one that has the slotid
 * equal to their own mpp_session_id.
 *
 * Single Writer: Mechanism is simplified by introducing the restriction of
 * only having a single qExec in a set of segmates capable of writing. Single
 * WRITER qExec is the only qExec amongst all of its segmates that will ever
 * perform database write operations.  Benefits of the approach, Single WRITER
 * qExec is the only member of a set of segmates that need to participate in
 * global transactions. Also... only this WRITER qExec really has to do
 * anything during commit. Assumption seems since they are just reader qExecs
 * that this is not a problem. The single WRITER qExec is the only qExec that
 * is guaranteed to participate in every dispatched statement for a given user
 * (at least to that segdb). Also, it is this WRITER qExec that performs any
 * utility statement.
 *
 * Coordinating Readers and Writers: The coordination is on when the writer
 * has set the snapshot such that the readers can get it and use it. In
 * general, we cannot assume that the writer will get to setting it before the
 * reader needs it and so we need to build a mechanism for the reader to (1)
 * know that its reading the right snapshot and (2) know when it can read.
 * The Mpp_session_id stored in the SharedSnapshotSlot is the piece of
 * information that lets the reader know it has got the right slot. And it
 * knows can read it when the xid and cid in the slot match the transactionid
 * and curid sent in from the QD in the SnapshotInfo field.  Basically QE
 * READERS aren't allowed to read the shared local snapshot until the shared
 * local snapshot has the same QD statement id as the QE Reader. i.e. the QE
 * WRITER updates the local snapshot and then writes the QD statement id into
 * the slot which identifies the "freshness" of that information. Currently QE
 * readers check that value and if its not been set they sleep (gasp!) for a
 * while. Think this approach is definitely not elegant and robust would be
 * great maybe to replace it with latch based approach.
 *
 * Cursor handling through SharedSnapshot: Cursors are funny case because they
 * read through a snapshot taken when the create cursor command was executed,
 * not through the current snapshot. Originally, the SharedSnapshotSlot was
 * designed for just the current command. The default transaction isolation
 * mode is READ COMMITTED, which cause a new snapshot to be created each
 * command. Each command in an explicit transaction started with BEGIN and
 * completed with COMMIT, etc. So, cursors would read through the current
 * snapshot instead of the create cursor snapshot and see data they shouldn't
 * see. The problem turns out to be a little more subtle because of the
 * existence of QE Readers and the fact that QE Readers can be created later –
 * long after the create cursor command. So, the solution was to serialize the
 * current snapshot to a temporary file during create cursor so that
 * subsequently created QE Readers could get the right snapshot to use from
 * the temporary file and ignore the SharedSnapshotSlot.
 *
 * Sub-Transaction handling through SharedSnapshot: QE Readers need to know
 * which sub-transactions the QE Writer has committed and which are active so
 * QE Readers can see the right data. While a sub-transaction may be committed
 * in an active parent transaction, that data is not formally committed until
 * the parent commits. And, active sub-transactions are not even
 * sub-transaction committed yet. So, other transactions cannot see active or
 * committed sub-transaction work yet. Without adding special logic to a QE
 * Reader, it would be considered another transaction and not see the
 * committed or active sub-transaction work. This is because QE Readers do not
 * start their own transaction. We just set a few variables in the xact.c
 * module to fake making it look like there is a current transaction,
 * including which sub-transactions are active or committed. This is a
 * kludge. In order for the QE Reader to fake being part of the QE Writer
 * transaction, we put the current transaction id and the values of all active
 * and committed sub-transaction ids into the SharedSnapshotSlot shared-memory
 * structure. Since shared-memory is not dynamic, poses an arbitrary limit on
 * the number of sub-transaction ids we keep in the SharedSnapshotSlot
 * in-memory. Once this limit is exceeded the sub-transaction ids are written
 * to temp files on disk.  See how the TransactionIdIsCurrentTransactionId
 * procedure in xact.c checks to see if the backend executing is a QE Reader
 * (or Entry DB Singleton), and if it is, walk through the sub-transaction ids
 * in SharedSnapshotSlot.
 *
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/distributedlog.h"
#include "access/twophase.h"  /*max_prepared_xacts*/
#include "access/xact.h"
#include "cdb/cdbtm.h"
#include "cdb/cdbvars.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "lib/stringinfo.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "utils/builtins.h"
#include "utils/faultinjector.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/resowner.h"
#include "utils/sharedsnapshot.h"
#include "utils/snapmgr.h"

/*
 * Distributed Snapshot that gets sent in from the QD to processes running
 * in EXECUTE mode.
 */
DtxContext DistributedTransactionContext = DTX_CONTEXT_LOCAL_ONLY;

DtxContextInfo QEDtxContextInfo = DtxContextInfo_StaticInit;

#define DUMP_HASH_SZ    1024
typedef struct DumpEntry
{
	uint32  segmate;
	FullTransactionId localXid;
	Snapshot snapshot;
} DumpEntry;

/* local hash table to store cursor snapshot dump*/
static HTAB *dumpHtab = NULL;
static bool created_dump = false;

static ResourceOwner DumpResOwner = NULL;	/* shared snapshot dump resources */

/* MPP Shared Snapshot. */
typedef struct SharedSnapshotStruct
{
	int 		numSlots;		/* number of valid Snapshot entries */
	int			maxSlots;		/* allocated size of sharedSnapshotArray */
	int 		nextSlot;		/* points to the next avail slot. */

	/*
	 * We now allow direct indexing into this array.
	 *
	 * We allocate the XIPS below.
	 *
	 * Be very careful when accessing fields inside here.
	 */
	SharedSnapshotSlot	   *slots;
} SharedSnapshotStruct;

static volatile SharedSnapshotStruct *sharedSnapshotArray;

volatile SharedSnapshotSlot *SharedLocalSnapshotSlot = NULL;

static Size slotSize = 0;
static Size slotCount = 0;
static Size xipEntryCount = 0;

/* prototypes for internal functions */
static SharedSnapshotSlot *SharedSnapshotAdd(int32 slotId);
static SharedSnapshotSlot *SharedSnapshotLookup(int32 slotId);

/*
 * Report shared-memory space needed by CreateSharedSnapshot.
 */
Size
SharedSnapshotShmemSize(void)
{
	Size		size;

	/* should be the same as PROCARRAY_MAXPROCS */
	xipEntryCount = MaxBackends + max_prepared_xacts;

	slotSize = sizeof(SharedSnapshotSlot);
	slotSize = MAXALIGN(slotSize);

	/*
	 * We only really need MaxBackends; but for safety we multiply that by two
	 * (to account for slow de-allocation on cleanup, for instance).
	 *
	 * MaxBackends is only somewhat right.  What we really want here is the
	 * MaxBackends value from the QD.  But this is at least safe since we know
	 * we dont need *MORE* than MaxBackends.  But in general MaxBackends on a
	 * QE is going to be bigger than on a QE by a good bit.  or at least it
	 * should be.
	 */

	slotCount = NUM_SHARED_SNAPSHOT_SLOTS;

	size = sizeof(SharedSnapshotStruct);
	size = add_size(size, mul_size(slotSize, slotCount));

	RequestNamedLWLockTranche("SharedSnapshotLocks", slotCount);

	return MAXALIGN(size);
}

/*
 * Initialize the sharedSnapshot array.  This array is used to communicate
 * snapshots between qExecs that are segmates.
 */
void
CreateSharedSnapshotArray(void)
{
	bool	found;
	int		i;

	/* Create or attach to the SharedSnapshot shared structure */
	sharedSnapshotArray = (SharedSnapshotStruct *)
		ShmemInitStruct("Shared Snapshot", SharedSnapshotShmemSize(), &found);

	Assert(slotCount != 0);
	Assert(xipEntryCount != 0);

	if (!found)
	{
		/*
		 * We're the first - initialize.
		 */
		LWLockPadded *lock_base;

		sharedSnapshotArray->numSlots = 0;

		/* slotCount is initialized in SharedSnapshotShmemSize(). */
		sharedSnapshotArray->maxSlots = slotCount;
		sharedSnapshotArray->nextSlot = 0;

		/*
		 * Set slots to point to the next byte beyond what was allocated for
		 * SharedSnapshotStruct. xips is the last element in the struct but is
		 * not included in SharedSnapshotShmemSize allocation.
		 */
		sharedSnapshotArray->slots = (SharedSnapshotSlot *)(sharedSnapshotArray + 1);

		lock_base = GetNamedLWLockTranche("SharedSnapshotLocks");
		for (i=0; i < sharedSnapshotArray->maxSlots; i++)
		{
			SharedSnapshotSlot *tmpSlot = &sharedSnapshotArray->slots[i];

			tmpSlot->slotid = -1;
			tmpSlot->slotindex = i;
			tmpSlot->slotLock = &lock_base[i].lock;
			tmpSlot->snapshot_handle = DSM_HANDLE_INVALID;
			tmpSlot->snapshot_sz = 0;

			MemSet(tmpSlot->dump, 0, sizeof(SnapshotDump) * SNAPSHOTDUMPARRAYSZ);
			tmpSlot->cur_dump_id = 0;
		}
	}
}

/*
 * Used to dump the internal state of the shared slots for debugging.
 */
char *
SharedSnapshotDump(void)
{
	StringInfoData str;
	volatile SharedSnapshotStruct *arrayP = sharedSnapshotArray;
	int			index;

	Assert(LWLockHeldByMe(SharedSnapshotLock));

	initStringInfo(&str);

	appendStringInfo(&str, "Local SharedSnapshot Slot Dump: currSlots: %d maxSlots: %d ",
					 arrayP->numSlots, arrayP->maxSlots);

	for (index=0; index < arrayP->maxSlots; index++)
	{
		/* need to do byte addressing to find the right slot */
		SharedSnapshotSlot *testSlot = &arrayP->slots[index];

		if (testSlot->slotid != -1)
		{
			appendStringInfo(&str, "(SLOT index: %d slotid: %d QDxid: "UINT64_FORMAT" pid: %u)",
							 testSlot->slotindex, testSlot->slotid, testSlot->distributedXid,
							 testSlot->writer_proc ? testSlot->writer_proc->pid : 0);
		}

	}

	return str.data;
}

/* Acquires an available slot in the sharedSnapshotArray.  The slot is then
 * marked with the supplied slotId.  This slotId is what others will use to
 * find this slot.  This should only ever be called by the "writer" qExec.
 *
 * The slotId should be something that is unique amongst all the possible
 * "writer" qExecs active on a segment database at a given moment.  It also
 * will need to be communicated to the "reader" qExecs so that they can find
 * this slot.
 */
static SharedSnapshotSlot *
SharedSnapshotAdd(int32 slotId)
{
	SharedSnapshotSlot *slot;
	volatile SharedSnapshotStruct *arrayP = sharedSnapshotArray;
	int nextSlot, i;
	int retryCount = gp_snapshotadd_timeout * 10; /* .1 s per wait */

retry:
	LWLockAcquire(SharedSnapshotLock, LW_EXCLUSIVE);

	slot = NULL;

	for (i = 0; i < arrayP->maxSlots; i++)
	{
		SharedSnapshotSlot *testSlot = &arrayP->slots[i];

		if (testSlot->slotindex > arrayP->maxSlots)
		{
			char *slot_dump = SharedSnapshotDump();
			LWLockRelease(SharedSnapshotLock);
			elog(ERROR, "Shared Local Snapshots Array appears corrupted: %s", slot_dump);
		}

		if (testSlot->slotid == slotId)
		{
			slot = testSlot;
			break;
		}
	}

	if (slot != NULL)
	{
		elog(DEBUG1, "SharedSnapshotAdd: found existing entry for our session-id. id %d retry %d pid %u", slotId, retryCount,
				slot->writer_proc ? slot->writer_proc->pid : 0);

		if (retryCount > 0)
		{
			LWLockRelease(SharedSnapshotLock);
			retryCount--;

			pg_usleep(100000); /* 100ms, wait gp_snapshotadd_timeout seconds max. */
			goto retry;
		}
		else
		{
			char *slot_dump = SharedSnapshotDump();
			LWLockRelease(SharedSnapshotLock);
			elog(ERROR, "writer segworker group shared snapshot collision on id %d. Slot array dump: %s",
				 slotId, slot_dump);
		}
	}

	if (arrayP->numSlots >= arrayP->maxSlots || arrayP->nextSlot == -1)
	{
		/*
		 * Ooops, no room.  this shouldn't happen as something else should have
		 * complained if we go over MaxBackends.
		 */
		LWLockRelease(SharedSnapshotLock);
		ereport(FATAL,
				(errcode(ERRCODE_TOO_MANY_CONNECTIONS),
				 errmsg("sorry, too many clients already."),
				 errdetail("There are no more available slots in the sharedSnapshotArray."),
				 errhint("Another piece of code should have detected that we have too many clients."
						 " this probably means that someone isn't releasing their slot properly.")));
	}

	slot = &arrayP->slots[arrayP->nextSlot];

	slot->slotindex = arrayP->nextSlot;

	/*
	 * find the next available slot
	 */
	nextSlot = -1;
	for (i=arrayP->nextSlot+1; i < arrayP->maxSlots; i++)
	{
		SharedSnapshotSlot *tmpSlot = &arrayP->slots[i];

		if (tmpSlot->slotid == -1)
		{
			nextSlot = i;
			break;
		}
	}

	arrayP->nextSlot = nextSlot;
	arrayP->numSlots++;

	/* initialize some things */
	slot->slotid = slotId;
	slot->fullXid = InvalidFullTransactionId;
	slot->startTimestamp = 0;
	slot->distributedXid = InvalidDistributedTransactionId;
	slot->segmateSync = 0;
	/* Remember the writer proc for IsCurrentTransactionIdForReader */
	slot->writer_proc = MyProc;

	LWLockRelease(SharedSnapshotLock);

	return slot;
}

void
GetSlotTableDebugInfo(void **snapshotArray, int *maxSlots)
{
	*snapshotArray = (void *)sharedSnapshotArray;
	*maxSlots = sharedSnapshotArray->maxSlots;
}

/*
 * Used by "reader" qExecs to find the slot in the sharedsnapshotArray with the
 * specified slotId.  In general, we should always be able to find the specified
 * slot unless something unexpected.  If the slot is not found, then NULL is
 * returned.
 *
 * MPP-4599: retry in the same pattern as the writer.
 */
static SharedSnapshotSlot *
SharedSnapshotLookup(int32 slotId)
{
	SharedSnapshotSlot *slot = NULL;
	volatile SharedSnapshotStruct *arrayP = sharedSnapshotArray;
	int retryCount = gp_snapshotadd_timeout * 10; /* .1 s per wait */
	int index;

	for (;;)
	{
		CHECK_FOR_INTERRUPTS();

		LWLockAcquire(SharedSnapshotLock, LW_SHARED);

		for (index=0; index < arrayP->maxSlots; index++)
		{
			SharedSnapshotSlot *testSlot;

			testSlot = &arrayP->slots[index];

			if (testSlot->slotindex > arrayP->maxSlots)
				elog(ERROR, "Shared Local Snapshots Array appears corrupted: %s", SharedSnapshotDump());

			if (testSlot->slotid == slotId)
			{
				slot = testSlot;
				break;
			}
		}

		LWLockRelease(SharedSnapshotLock);

		if (slot != NULL)
		{
			break;
		}
		else
		{
			if (retryCount > 0)
			{
				retryCount--;

				pg_usleep(100000); /* 100ms, wait gp_snapshotadd_timeout seconds max. */
			}
			else
			{
				break;
			}
		}
	}

	return slot;
}


/*
 * Used by the "writer" qExec to "release" the slot it had been using.
 *
 */
void
SharedSnapshotRemove(volatile SharedSnapshotSlot *slot, char *creatorDescription)
{
	int slotId = slot->slotid;

	LWLockAcquire(SharedSnapshotLock, LW_EXCLUSIVE);

	if (slot->snapshot_handle != DSM_HANDLE_INVALID)
		dsm_unpin_segment(slot->snapshot_handle);

	/* determine if we need to modify the next available slot to use.  we
	 * only do this is our slotindex is lower then the existing one.
	 */
	if (sharedSnapshotArray->nextSlot == -1 || slot->slotindex < sharedSnapshotArray->nextSlot)
	{
		if (slot->slotindex > sharedSnapshotArray->maxSlots)
			elog(ERROR, "Shared Local Snapshots slot has a bogus slotindex: %d. slot array dump: %s",
				 slot->slotindex, SharedSnapshotDump());

		sharedSnapshotArray->nextSlot = slot->slotindex;
	}

	/* reset the slotid which marks it as being unused. */
	slot->slotid = -1;
	slot->fullXid = InvalidFullTransactionId;
	slot->startTimestamp = 0;
	slot->distributedXid = InvalidDistributedTransactionId;
	slot->segmateSync = 0;
	slot->snapshot_handle = DSM_HANDLE_INVALID;
	slot->snapshot_sz = 0;

	sharedSnapshotArray->numSlots -= 1;

	LWLockRelease(SharedSnapshotLock);

	elog((Debug_print_full_dtm ? LOG : DEBUG5),"SharedSnapshotRemove removed slot for slotId = %d, creator = %s (address %p)",
		 slotId, creatorDescription, SharedLocalSnapshotSlot);
}

void
addSharedSnapshot(char *creatorDescription, int id)
{
	SharedLocalSnapshotSlot = SharedSnapshotAdd(id);

	elog((Debug_print_full_dtm ? LOG : DEBUG5),"%s added Shared Local Snapshot slot for gp_session_id = %d (address %p)",
		 creatorDescription, id, SharedLocalSnapshotSlot);
}

void
lookupSharedSnapshot(char *lookerDescription, char *creatorDescription, int id)
{
	SharedSnapshotSlot *slot;

	slot = SharedSnapshotLookup(id);

	if (slot == NULL)
	{
		LWLockAcquire(SharedSnapshotLock, LW_SHARED);
		ereport(ERROR,
				(errmsg("%s could not find Shared Local Snapshot!",
						lookerDescription),
				 errdetail("Tried to find a shared snapshot slot with id: %d "
						   "and found none. Shared Local Snapshots dump: %s", id,
						   SharedSnapshotDump()),
				 errhint("Either this %s was created before the %s or the %s died.",
						 lookerDescription, creatorDescription, creatorDescription)));
	}

	SharedLocalSnapshotSlot = slot;

	elog((Debug_print_full_dtm ? LOG : DEBUG5),"%s found Shared Local Snapshot slot for gp_session_id = %d created by %s (address %p)",
		 lookerDescription, id, creatorDescription, SharedLocalSnapshotSlot);
}

/*
 * Dump the shared local snapshot, so that the readers can pick it up.
 */
void
dumpSharedLocalSnapshot_forCursor(void)
{
	int			id;
	char	   *src_address;
	char	   *dst_address;
	dsm_segment *src_segment;
	dsm_segment *dst_segment;
	ResourceOwner oldowner;
	SharedSnapshotSlot *src = NULL;
	volatile SnapshotDump *pDump;

	Assert(Gp_role == GP_ROLE_DISPATCH || (Gp_role == GP_ROLE_EXECUTE && Gp_is_writer));
	Assert(SharedLocalSnapshotSlot != NULL);

	if (DumpResOwner == NULL)
		DumpResOwner = ResourceOwnerCreate(NULL, "SharedSnapshotDumpResOwner");

	LWLockAcquire(SharedLocalSnapshotSlot->slotLock, LW_EXCLUSIVE);

	created_dump = true;
	oldowner = CurrentResourceOwner;
	CurrentResourceOwner = DumpResOwner;

	src = (SharedSnapshotSlot *)SharedLocalSnapshotSlot;

	id = src->cur_dump_id;
	pDump = &src->dump[id];

	Assert(SharedLocalSnapshotSlot->snapshot_handle != DSM_HANDLE_INVALID);
	src_segment = dsm_attach(SharedLocalSnapshotSlot->snapshot_handle);
	src_address = dsm_segment_address(src_segment);

	dst_segment = dsm_create(src->snapshot_sz, 0);
	dst_address = dsm_segment_address(dst_segment);

	/* Dump shared snapshot to dsm of destination */
	memcpy(dst_address, src_address, src->snapshot_sz);

	/* FIXME: can we just reuse the src dsm handler? */
	pDump->handle = dsm_segment_handle(dst_segment);
	pDump->segmateSync = src->segmateSync;
	pDump->distributedXid = src->distributedXid;
	pDump->localXid = src->fullXid;

	dsm_detach(src_segment);

	elog(DEBUG1, "Dump syncmate : %u snapshot to slot %d", src->segmateSync, id);

	src->cur_dump_id =
		(src->cur_dump_id + 1) % SNAPSHOTDUMPARRAYSZ;

	CurrentResourceOwner = oldowner;
	LWLockRelease(SharedLocalSnapshotSlot->slotLock);
}

void
readSharedLocalSnapshot_forCursor(Snapshot snapshot, DtxContext distributedTransactionContext)
{
	bool found;
	SharedSnapshotSlot *src = NULL;
	FullTransactionId localXid;

	Assert(Gp_role == GP_ROLE_EXECUTE);
	Assert(!Gp_is_writer);
	Assert(SharedLocalSnapshotSlot != NULL);
	Assert(snapshot->xip != NULL);

	SIMPLE_FAULT_INJECTOR("before_read_shared_snapshot_for_cursor");

	if (dumpHtab == NULL)
	{
		HASHCTL     hash_ctl;
		memset(&hash_ctl, 0, sizeof(hash_ctl));
		hash_ctl.keysize = sizeof(uint32);
		hash_ctl.entrysize = sizeof(DumpEntry);
		hash_ctl.hcxt = TopTransactionContext;
		dumpHtab = hash_create("snapshot dump",
		                       DUMP_HASH_SZ  ,
		                       &hash_ctl,
		                       HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	}

	/* check segmate in local memory, only sync from shared memory once */
	DumpEntry *entry = hash_search(dumpHtab, &QEDtxContextInfo.segmateSync, HASH_ENTER, &found);
	volatile SnapshotDump *pDump = NULL;

	if (!found)
	{
		src = (SharedSnapshotSlot *)SharedLocalSnapshotSlot;
		LWLockAcquire(src->slotLock, LW_SHARED);
		int search_finish_id = src->cur_dump_id;
		int search_iter = search_finish_id;

		do{
			if (search_iter < 0)
				search_iter = SNAPSHOTDUMPARRAYSZ - 1;

			if (src->dump[search_iter].segmateSync == QEDtxContextInfo.segmateSync &&
				src->dump[search_iter].distributedXid == QEDtxContextInfo.distributedXid)
			{
				pDump = &src->dump[search_iter];
				found = true;
				break;
			}

			search_iter --;

		} while (search_iter != search_finish_id);

		if (!found)
		{
			LWLockRelease(SharedLocalSnapshotSlot->slotLock);
			LWLockAcquire(SharedSnapshotLock, LW_SHARED); /* For SharedSnapshotDump() */
			ereport(ERROR, (errmsg("could not find Shared Local Snapshot!"),
				errdetail("Tried to set the shared local snapshot slot with segmate: %u "
				          "and failed. Shared Local Snapshots dump: %s", QEDtxContextInfo.segmateSync,
				          SharedSnapshotDump())));
		}

		Assert(pDump->handle != 0);

		dsm_segment* segment = dsm_attach(pDump->handle);
		char *ptr = dsm_segment_address(segment);

		entry->snapshot = RestoreSnapshot(ptr);
		entry->localXid = pDump->localXid;

		dsm_detach(segment);

		LWLockRelease(SharedLocalSnapshotSlot->slotLock);
	}

	Snapshot dumpsnapshot = entry->snapshot;
	localXid = entry->localXid;
	snapshot->xmin = dumpsnapshot->xmin;
	snapshot->xmax = dumpsnapshot->xmax;
	snapshot->xcnt = dumpsnapshot->xcnt;

	memcpy(snapshot->xip, dumpsnapshot->xip, snapshot->xcnt * sizeof(TransactionId));

	/* zero out the slack in the xip-array */
	memset(snapshot->xip + snapshot->xcnt, 0, (xipEntryCount - snapshot->xcnt)*sizeof(TransactionId));

	snapshot->curcid = dumpsnapshot->curcid;

	SetSharedTransactionId_reader(
		localXid,
		snapshot->curcid,
		distributedTransactionContext);

}

/*
 * Free any shared snapshot files.
 */
void
AtEOXact_SharedSnapshot(void)
{
	dumpHtab = NULL;

	if (created_dump)
	{
		LWLockAcquire(SharedLocalSnapshotSlot->slotLock, LW_EXCLUSIVE);

		/* release dump dsm */
		ResourceOwnerRelease(DumpResOwner,
		                     RESOURCE_RELEASE_BEFORE_LOCKS,
		                     false, /* isCommit */
		                     true); /* isTopLevel */

		SharedLocalSnapshotSlot->cur_dump_id = 0;
		MemSet(SharedLocalSnapshotSlot->dump, 0, sizeof(SnapshotDump) * SNAPSHOTDUMPARRAYSZ);
		created_dump = false;
		LWLockRelease(SharedLocalSnapshotSlot->slotLock);

	}
}

/*
 * LogDistributedSnapshotInfo
 *   Log the distributed snapshot info in a given snapshot.
 *
 * The 'prefix' is used to prefix the log message.
 */
void
LogDistributedSnapshotInfo(Snapshot snapshot, const char *prefix)
{
	if (!IsMVCCSnapshot(snapshot))
		return;

	StringInfoData buf;
	initStringInfo(&buf);

	DistributedSnapshotWithLocalMapping *mapping =
		&(snapshot->distribSnapshotWithLocalMapping);

	DistributedSnapshot *ds = &mapping->ds;

	appendStringInfo(&buf, "%s Distributed snapshot info: "
			 "xminAllDistributedSnapshots="UINT64_FORMAT", distribSnapshotId=%d"
			 ", xmin="UINT64_FORMAT", xmax="UINT64_FORMAT", count=%d",
			 prefix,
			 ds->xminAllDistributedSnapshots,
			 ds->distribSnapshotId,
			 ds->xmin,
			 ds->xmax,
			 ds->count);

	appendStringInfoString(&buf, ", In progress array: {");

	for (int no = 0; no < ds->count; no++)
	{
		if (no != 0)
		{
			appendStringInfo(&buf, ", (dx"UINT64_FORMAT")",
					 ds->inProgressXidArray[no]);
		}
		else
		{
			appendStringInfo(&buf, " (dx"UINT64_FORMAT")",
					 ds->inProgressXidArray[no]);
		}
	}
	appendStringInfoString(&buf, "}");

	elog(LOG, "%s", buf.data);
	pfree(buf.data);
}
