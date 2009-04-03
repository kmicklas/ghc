/* -----------------------------------------------------------------------------
 *
 * (c) The GHC Team 1998-2008
 *
 * Generational garbage collector: utilities
 *
 * Documentation on the architecture of the Garbage Collector can be
 * found in the online commentary:
 * 
 *   http://hackage.haskell.org/trac/ghc/wiki/Commentary/Rts/Storage/GC
 *
 * ---------------------------------------------------------------------------*/

#include "Rts.h"
#include "RtsFlags.h"
#include "Storage.h"
#include "GC.h"
#include "GCThread.h"
#include "GCUtils.h"
#include "Printer.h"
#include "Trace.h"
#ifdef THREADED_RTS
#include "WSDeque.h"
#endif

#ifdef THREADED_RTS
SpinLock gc_alloc_block_sync;
#endif

bdescr *
allocBlock_sync(void)
{
    bdescr *bd;
    ACQUIRE_SPIN_LOCK(&gc_alloc_block_sync);
    bd = allocBlock();
    RELEASE_SPIN_LOCK(&gc_alloc_block_sync);
    return bd;
}


#if 0
static void
allocBlocks_sync(nat n, bdescr **hd, bdescr **tl, 
                 nat gen_no, step *stp,
                 StgWord32 flags)
{
    bdescr *bd;
    nat i;
    ACQUIRE_SPIN_LOCK(&gc_alloc_block_sync);
    bd = allocGroup(n);
    for (i = 0; i < n; i++) {
        bd[i].blocks = 1;
        bd[i].gen_no = gen_no;
        bd[i].step = stp;
        bd[i].flags = flags;
        bd[i].link = &bd[i+1];
        bd[i].u.scan = bd[i].free = bd[i].start;
    }
    *hd = bd;
    *tl = &bd[n-1];
    RELEASE_SPIN_LOCK(&gc_alloc_block_sync);
}
#endif

void
freeChain_sync(bdescr *bd)
{
    ACQUIRE_SPIN_LOCK(&gc_alloc_block_sync);
    freeChain(bd);
    RELEASE_SPIN_LOCK(&gc_alloc_block_sync);
}

/* -----------------------------------------------------------------------------
   Workspace utilities
   -------------------------------------------------------------------------- */

bdescr *
grab_local_todo_block (step_workspace *ws)
{
    bdescr *bd;
    step *stp;

    stp = ws->step;

    bd = ws->todo_overflow;
    if (bd != NULL)
    {
        ws->todo_overflow = bd->link;
        bd->link = NULL;
        ws->n_todo_overflow--;
	return bd;
    }

    bd = popWSDeque(ws->todo_q);
    if (bd != NULL)
    {
	ASSERT(bd->link == NULL);
	return bd;
    }

    return NULL;
}

bdescr *
steal_todo_block (nat s)
{
    nat n;
    bdescr *bd;

    // look for work to steal
    for (n = 0; n < n_gc_threads; n++) {
        if (n == gct->thread_index) continue;
        bd = stealWSDeque(gc_threads[n]->steps[s].todo_q);
        if (bd) {
            return bd;
        }
    }
    return NULL;
}

void
push_scanned_block (bdescr *bd, step_workspace *ws)
{
    ASSERT(bd != NULL);
    ASSERT(bd->link == NULL);
    ASSERT(bd->step == ws->step);
    ASSERT(bd->u.scan == bd->free);

    if (bd->start + BLOCK_SIZE_W - bd->free > WORK_UNIT_WORDS)
    {
        // a partially full block: put it on the part_list list.
        bd->link = ws->part_list;
        ws->part_list = bd;
        ws->n_part_blocks++;
        IF_DEBUG(sanity, 
                 ASSERT(countBlocks(ws->part_list) == ws->n_part_blocks));
    }
    else
    {
        // put the scan block on the ws->scavd_list.
        bd->link = ws->scavd_list;
        ws->scavd_list = bd;
        ws->n_scavd_blocks ++;
        IF_DEBUG(sanity, 
                 ASSERT(countBlocks(ws->scavd_list) == ws->n_scavd_blocks));
    }
}

StgPtr
todo_block_full (nat size, step_workspace *ws)
{
    StgPtr p;
    bdescr *bd;

    // todo_free has been pre-incremented by Evac.c:alloc_for_copy().  We
    // are expected to leave it bumped when we've finished here.
    ws->todo_free -= size;

    bd = ws->todo_bd;

    ASSERT(bd != NULL);
    ASSERT(bd->link == NULL);
    ASSERT(bd->step == ws->step);

    // If the global list is not empty, or there's not much work in
    // this block to push, and there's enough room in
    // this block to evacuate the current object, then just increase
    // the limit.
    if (!looksEmptyWSDeque(ws->todo_q) || 
        (ws->todo_free - bd->u.scan < WORK_UNIT_WORDS / 2)) {
        if (ws->todo_free + size < bd->start + BLOCK_SIZE_W) {
            ws->todo_lim = stg_min(bd->start + BLOCK_SIZE_W,
                                   ws->todo_lim + stg_max(WORK_UNIT_WORDS,size));
            debugTrace(DEBUG_gc, "increasing limit for %p to %p", bd->start, ws->todo_lim);
            p = ws->todo_free;
            ws->todo_free += size;
            return p;
        }
    }
    
    gct->copied += ws->todo_free - bd->free;
    bd->free = ws->todo_free;

    ASSERT(bd->u.scan >= bd->start && bd->u.scan <= bd->free);

    // If this block is not the scan block, we want to push it out and
    // make room for a new todo block.
    if (bd != gct->scan_bd)
    {
        // If this block does not have enough space to allocate the
        // current object, but it also doesn't have any work to push, then 
        // push it on to the scanned list.  It cannot be empty, because
        // then there would be enough room to copy the current object.
        if (bd->u.scan == bd->free)
        {
            ASSERT(bd->free != bd->start);
            push_scanned_block(bd, ws);
        }
        // Otherwise, push this block out to the global list.
        else 
        {
            step *stp;
            stp = ws->step;
            debugTrace(DEBUG_gc, "push todo block %p (%ld words), step %d, todo_q: %ld", 
                  bd->start, (unsigned long)(bd->free - bd->u.scan),
                  stp->abs_no, dequeElements(ws->todo_q));

            if (!pushWSDeque(ws->todo_q, bd)) {
                bd->link = ws->todo_overflow;
                ws->todo_overflow = bd;
                ws->n_todo_overflow++;
            }
        }
    }

    ws->todo_bd   = NULL;
    ws->todo_free = NULL;
    ws->todo_lim  = NULL;

    alloc_todo_block(ws, size);

    p = ws->todo_free;
    ws->todo_free += size;
    return p;
}

StgPtr
alloc_todo_block (step_workspace *ws, nat size)
{
    bdescr *bd/*, *hd, *tl */;

    // Grab a part block if we have one, and it has enough room
    if (ws->part_list != NULL && 
        ws->part_list->start + BLOCK_SIZE_W - ws->part_list->free > (int)size)
    {
        bd = ws->part_list;
        ws->part_list = bd->link;
        ws->n_part_blocks--;
    }
    else
    {
        // blocks in to-space get the BF_EVACUATED flag.

//        allocBlocks_sync(16, &hd, &tl, 
//                         ws->step->gen_no, ws->step, BF_EVACUATED);
//
//        tl->link = ws->part_list;
//        ws->part_list = hd->link;
//        ws->n_part_blocks += 15;
//
//        bd = hd;

        bd = allocBlock_sync();
        bd->step = ws->step;
        bd->gen_no = ws->step->gen_no;
        bd->flags = BF_EVACUATED;
        bd->u.scan = bd->free = bd->start;
    }

    bd->link = NULL;

    ws->todo_bd = bd;
    ws->todo_free = bd->free;
    ws->todo_lim  = stg_min(bd->start + BLOCK_SIZE_W,
                            bd->free + stg_max(WORK_UNIT_WORDS,size));

    debugTrace(DEBUG_gc, "alloc new todo block %p for step %d", 
               bd->free, ws->step->abs_no);

    return ws->todo_free;
}

/* -----------------------------------------------------------------------------
 * Debugging
 * -------------------------------------------------------------------------- */

#if DEBUG
void
printMutableList(generation *gen)
{
    bdescr *bd;
    StgPtr p;

    debugBelch("mutable list %p: ", gen->mut_list);

    for (bd = gen->mut_list; bd != NULL; bd = bd->link) {
	for (p = bd->start; p < bd->free; p++) {
	    debugBelch("%p (%s), ", (void *)*p, info_type((StgClosure *)*p));
	}
    }
    debugBelch("\n");
}
#endif /* DEBUG */
