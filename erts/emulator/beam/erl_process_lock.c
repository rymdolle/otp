/*
 * %CopyrightBegin%
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Copyright Ericsson AB 2007-2025. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * %CopyrightEnd%
 */


/*
 * Description:	Implementation of Erlang process locks.
 *
 * Author: 	Rickard Green
 */

/*
 * A short explanation of the process lock implementation:
 *     Each process has a lock bitfield and a number of lock wait
 *   queues.
 *     The bit field contains of a number of lock flags (L1, L2, ...)
 *   and a number of wait flags (W1, W2, ...). Each lock flag has a
 *   corresponding wait flag. The bit field isn't guaranteed to be
 *   larger than 32-bits which sets a maximum of 16 different locks
 *   per process. Currently, only 4 locks per process are used. The
 *   bit field is operated on by use of atomic operations (custom
 *   made bitwise atomic operations). When a lock is locked the
 *   corresponding lock bit is set. When a thread is waiting on a
 *   lock the wait flag for the lock is set.
 *     The process table is protected by pix (process index) locks
 *   which is spinlocks that protects a number of process indices in
 *   the process table. The pix locks also protects the lock queues
 *   and modifications of wait flags.
 *     When acquiring a process lock we first try to set the lock
 *   flag. If we are able to set the lock flag and the wait flag
 *   isn't set we are done. If the lock flag was already set we
 *   have to acquire the pix lock, set the wait flag, and put
 *   ourselves in the wait queue.
 *   Process locks will always be acquired in fifo order.
 *     When releasing a process lock we first unset all lock flags
 *   whose corresponding wait flag is clear (which will succeed).
 *   If wait flags were set for the locks being released, we acquire
 *   the pix lock, and transfer the lock to the first thread
 *   in the wait queue.
 *     Note that wait flags may be read without the pix lock, but
 *   it is important that wait flags only are modified when the pix
 *   lock is held.
 *     This implementation assumes that erts_atomic_or_retold()
 *   provides necessary memorybarriers for a lock operation, and that
 *   erts_atomic_and_retold() provides necessary memorybarriers
 *   for an unlock operation.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "erl_process.h"
#include "erl_thr_progress.h"


#if ERTS_PROC_LOCK_OWN_IMPL

#define ERTS_PROC_LOCK_SPIN_COUNT_MAX  2000
#define ERTS_PROC_LOCK_SPIN_COUNT_SCHED_INC 32
#define ERTS_PROC_LOCK_SPIN_COUNT_BASE 1000
#define ERTS_PROC_LOCK_AUX_SPIN_COUNT 50

#define ERTS_PROC_LOCK_SPIN_UNTIL_YIELD 25

#ifdef ERTS_PROC_LOCK_DEBUG
#define ERTS_PROC_LOCK_HARD_DEBUG
#endif

#ifdef ERTS_PROC_LOCK_HARD_DEBUG
static void check_queue(erts_proc_lock_t *lck);
#endif

#if SIZEOF_INT < 4
#error "The size of the 'uflgs' field of the erts_tse_t type is too small"
#endif

static int proc_lock_spin_count;
static int aux_thr_proc_lock_spin_count;

static void cleanup_tse(void);

#endif /* ERTS_PROC_LOCK_OWN_IMPL */

#ifdef ERTS_ENABLE_LOCK_CHECK
static struct {
    Sint16 proc_lock_main;
    Sint16 proc_lock_msgq;
    Sint16 proc_lock_btm;
    Sint16 proc_lock_status;
    Sint16 proc_lock_trace;
} lc_id;
#endif

erts_pix_lock_t erts_pix_locks[ERTS_NO_OF_PIX_LOCKS];

void
erts_init_proc_lock(int cpus)
{
    int i;
    for (i = 0; i < ERTS_NO_OF_PIX_LOCKS; i++) {
        erts_mtx_init(&erts_pix_locks[i].u.mtx, "pix_lock", make_small(i),
            ERTS_LOCK_FLAGS_PROPERTY_STATIC | ERTS_LOCK_FLAGS_CATEGORY_PROCESS);
    }
#if ERTS_PROC_LOCK_OWN_IMPL
    erts_thr_install_exit_handler(cleanup_tse);
    if (cpus > 1) {
	proc_lock_spin_count = ERTS_PROC_LOCK_SPIN_COUNT_BASE;
	proc_lock_spin_count += (ERTS_PROC_LOCK_SPIN_COUNT_SCHED_INC
				 * ((int) erts_no_schedulers));
	aux_thr_proc_lock_spin_count = ERTS_PROC_LOCK_AUX_SPIN_COUNT;
    }
    else if (cpus == 1) {
	proc_lock_spin_count = 0;
	aux_thr_proc_lock_spin_count = 0;
    }
    else { /* No of cpus unknown. Assume multi proc, but be conservative. */
	proc_lock_spin_count = ERTS_PROC_LOCK_SPIN_COUNT_BASE/2;
	aux_thr_proc_lock_spin_count = ERTS_PROC_LOCK_AUX_SPIN_COUNT/2;
    }
    if (proc_lock_spin_count > ERTS_PROC_LOCK_SPIN_COUNT_MAX)
	proc_lock_spin_count = ERTS_PROC_LOCK_SPIN_COUNT_MAX;
#endif
#ifdef ERTS_ENABLE_LOCK_CHECK
    lc_id.proc_lock_main	= erts_lc_get_lock_order_id("proc_main");
    lc_id.proc_lock_msgq	= erts_lc_get_lock_order_id("proc_msgq");
    lc_id.proc_lock_btm		= erts_lc_get_lock_order_id("proc_btm");
    lc_id.proc_lock_status	= erts_lc_get_lock_order_id("proc_status");
    lc_id.proc_lock_trace	= erts_lc_get_lock_order_id("proc_trace");
#endif
}

#if ERTS_PROC_LOCK_OWN_IMPL

#ifdef ERTS_ENABLE_LOCK_CHECK
#define CHECK_UNUSED_TSE(W) ASSERT((W)->uflgs == 0)
#else
#define CHECK_UNUSED_TSE(W)
#endif

static ERTS_INLINE erts_tse_t *
tse_fetch(erts_pix_lock_t *pix_lock)
{
    erts_tse_t *tse = erts_tse_fetch();
    tse->uflgs = 0;
    return tse;
}

static ERTS_INLINE void
tse_return(erts_tse_t *tse)
{
    CHECK_UNUSED_TSE(tse);
    erts_tse_return(tse);
}

static void
cleanup_tse(void)
{
    erts_tse_t *tse = erts_tse_fetch();
    if (tse)
	erts_tse_return(tse);
}


/*
 * Waiters are queued in a circular double linked list;
 * where lck->queue[lock_ix] is the first waiter in queue, and
 * lck->queue[lock_ix]->prev is the last waiter in queue.
 */

static ERTS_INLINE void
enqueue_waiter(erts_proc_lock_t *lck, int ix, erts_tse_t *wtr)
{
    if (!lck->queue[ix]) {
	lck->queue[ix] = wtr;
	wtr->next = wtr;
	wtr->prev = wtr;
    }
    else {
	ASSERT(lck->queue[ix]->next && lck->queue[ix]->prev);
	wtr->next = lck->queue[ix];
	wtr->prev = lck->queue[ix]->prev;
	wtr->prev->next = wtr;
	lck->queue[ix]->prev = wtr;
    }
}

static erts_tse_t *
dequeue_waiter(erts_proc_lock_t *lck, int ix)
{
    erts_tse_t *wtr = lck->queue[ix];
    ASSERT(lck->queue[ix]);
    if (wtr->next == wtr) {
	ASSERT(lck->queue[ix]->prev == wtr);
	lck->queue[ix] = NULL;
    }
    else {
	ASSERT(wtr->next != wtr);
	ASSERT(wtr->prev != wtr);
	wtr->next->prev = wtr->prev;
	wtr->prev->next = wtr->next;
	lck->queue[ix] = wtr->next;
    }
    return wtr;
}

/*
 * Tries to acquire as many locks as possible in lock order,
 * and sets the wait flag on the first lock not possible to
 * acquire.
 *
 * Note: We need the pix lock during this operation. Wait
 *       flags are only allowed to be manipulated under pix
 *       lock.
 */
static ERTS_INLINE void
try_aquire(erts_proc_lock_t *lck, erts_tse_t *wtr)
{
    ErtsProcLocks got_locks = (ErtsProcLocks) 0;
    ErtsProcLocks locks = wtr->uflgs;
    int lock_no;

    ASSERT(got_locks != locks);

    for (lock_no = 0; lock_no <= ERTS_PROC_LOCK_MAX_BIT; lock_no++) {
	ErtsProcLocks lock = ((ErtsProcLocks) 1) << lock_no;
	if (locks & lock) {
	    ErtsProcLocks wflg, old_lflgs;
	    if (lck->queue[lock_no]) {
		/* Others already waiting */
	    enqueue:
		ASSERT(ERTS_PROC_LOCK_FLGS_READ_(lck)
			       & (lock << ERTS_PROC_LOCK_WAITER_SHIFT));
		enqueue_waiter(lck, lock_no, wtr);
		break;
	    }
	    wflg = lock << ERTS_PROC_LOCK_WAITER_SHIFT;
	    old_lflgs = ERTS_PROC_LOCK_FLGS_BOR_ACQB_(lck, wflg | lock);
	    if (old_lflgs & lock) {
		/* Didn't get the lock */
		goto enqueue;
	    }
	    else {
		/* Got the lock */
		got_locks |= lock;
		ASSERT(!(old_lflgs & wflg));
		/* No one else can be waiting for the lock; remove wait flag */
		(void) ERTS_PROC_LOCK_FLGS_BAND_(lck, ~wflg);
		if (got_locks == locks)
		    break;
	    }
	}
    }

    wtr->uflgs &= ~got_locks;
}

/*
 * Transfer 'trnsfr_lcks' held by this executing thread to other
 * threads waiting for the locks. When a lock has been transferred
 * we also have to try to acquire as many lock as possible for the
 * other thread.
 */
static int
transfer_locks(Process *p,
	       ErtsProcLocks trnsfr_lcks,
	       erts_pix_lock_t *pix_lock,
	       int unlock)
{
    int transferred = 0;
    erts_tse_t *wake = NULL;
    erts_tse_t *wtr;
    ErtsProcLocks unset_waiter = 0;
    ErtsProcLocks tlocks = trnsfr_lcks;
    int lock_no;

    ERTS_LC_ASSERT(erts_lc_pix_lock_is_locked(pix_lock));

#ifdef ERTS_PROC_LOCK_HARD_DEBUG
    check_queue(&p->lock);
#endif

    for (lock_no = 0; tlocks && lock_no <= ERTS_PROC_LOCK_MAX_BIT; lock_no++) {
	ErtsProcLocks lock = ((ErtsProcLocks) 1) << lock_no;
	if (tlocks & lock) {
	    /* Transfer lock */
#ifdef ERTS_ENABLE_LOCK_CHECK
	    tlocks &= ~lock;
#endif
	    ASSERT(ERTS_PROC_LOCK_FLGS_READ_(&p->lock)
			   & (lock << ERTS_PROC_LOCK_WAITER_SHIFT));
	    transferred++;
	    wtr = dequeue_waiter(&p->lock, lock_no);
	    ASSERT(wtr != NULL);
	    if (!p->lock.queue[lock_no])
		unset_waiter |= lock;
	    ASSERT(wtr->uflgs & lock);
	    wtr->uflgs &= ~lock;
	    if (wtr->uflgs)
		try_aquire(&p->lock, wtr);
	    if (!wtr->uflgs) {
		/*
		 * The other thread got all locks it needs;
		 * need to wake it up.
		 */
		wtr->next = wake;
		wake = wtr;
	    }
	}

    }

    if (unset_waiter) {
	unset_waiter <<= ERTS_PROC_LOCK_WAITER_SHIFT;
	(void) ERTS_PROC_LOCK_FLGS_BAND_(&p->lock, ~unset_waiter);
    }

#ifdef ERTS_PROC_LOCK_HARD_DEBUG
    check_queue(&p->lock);
#endif

    ASSERT(tlocks == 0); /* We should have transferred all of them */

    if (!wake) {
	if (unlock)
	    erts_pix_unlock(pix_lock);
    }
    else {
	erts_pix_unlock(pix_lock);
    
	do {
	    erts_tse_t *tmp = wake;
	    wake = wake->next;
	    erts_atomic32_set_nob(&tmp->uaflgs, 0);
	    erts_tse_set(tmp);
	} while (wake);

	if (!unlock)
	    erts_pix_lock(pix_lock);
    }
    return transferred;
}

/*
 * Determine which locks in 'need_locks' are not currently locked in
 * 'in_use', but do not return any locks "above" some lock we need,
 * so we do not attempt to grab locks out of order.
 *
 * For example, if we want to lock 10111, and 00100 was already locked, this
 * would return 00011, indicating we should not try for 10000 yet because
 * that would be a lock-ordering violation.
 */
static ERTS_INLINE ErtsProcLocks
in_order_locks(ErtsProcLocks in_use, ErtsProcLocks need_locks)
{
    /* All locks we want that are already locked by someone else. */
    ErtsProcLocks busy = in_use & need_locks;

    /* Just the lowest numbered lock we want that's in use; 0 if none. */
    ErtsProcLocks lowest_busy = busy & -busy;

    /* All locks below the lowest one we want that's in use already. */
    return need_locks & (lowest_busy - 1);
}

/*
 * Try to grab locks one at a time in lock order and wait on the lowest
 * lock we fail to grab, if any.
 *
 * If successful, this returns 0 and all locks in 'need_locks' are held.
 *
 * On entry, the pix lock is held iff !ERTS_PROC_LOCK_ATOMIC_IMPL.
 * On exit it is not held.
 */
static void
wait_for_locks(Process *p,
               erts_pix_lock_t *pixlck,
	       ErtsProcLocks locks,
               ErtsProcLocks need_locks,
               ErtsProcLocks olflgs)
{
    erts_pix_lock_t *pix_lock = pixlck ? pixlck : ERTS_PID2PIXLOCK(p->common.id);
    erts_tse_t *wtr;

    /* Acquire a waiter object on which this thread can wait. */
    wtr = tse_fetch(pix_lock);
    
    /* Record which locks this waiter needs. */
    wtr->uflgs = need_locks;

    ASSERT((wtr->uflgs & ~ERTS_PROC_LOCKS_ALL) == 0);

#if ERTS_PROC_LOCK_ATOMIC_IMPL
    erts_pix_lock(pix_lock);
#endif

    ERTS_LC_ASSERT(erts_lc_pix_lock_is_locked(pix_lock));

#ifdef ERTS_PROC_LOCK_HARD_DEBUG
    check_queue(&p->lock);
#endif

    /* Try to acquire locks one at a time in lock order and set wait flag */
    try_aquire(&p->lock, wtr);

    ASSERT((wtr->uflgs & ~ERTS_PROC_LOCKS_ALL) == 0);

#ifdef ERTS_PROC_LOCK_HARD_DEBUG
    check_queue(&p->lock);
#endif

    if (wtr->uflgs == 0)
	erts_pix_unlock(pix_lock);
    else {
	/* We didn't get them all; need to wait... */

	ASSERT((wtr->uflgs & ~ERTS_PROC_LOCKS_ALL) == 0);

	erts_atomic32_set_nob(&wtr->uaflgs, 1);
	erts_pix_unlock(pix_lock);

	while (1) {
	    int res;
	    erts_tse_reset(wtr);

	    if (erts_atomic32_read_nob(&wtr->uaflgs) == 0)
		break;

	    /*
	     * Wait for needed locks. When we are woken all needed locks have
	     * have been acquired by other threads and transferred to us.
	     * However, we need to be prepared for spurious wakeups.
	     */
	    do {
		res = erts_tse_wait(wtr); /* might return EINTR */
	    } while (res != 0);
	}

	ASSERT(wtr->uflgs == 0);
    }

    ASSERT(locks == (ERTS_PROC_LOCK_FLGS_READ_(&p->lock) & locks));

    tse_return(wtr);
}

/*
 * erts_proc_lock_failed() is called when erts_proc_lock()
 * wasn't able to lock all locks. We may need to transfer locks
 * to waiters and wait for our turn on locks.
 *
 * Iff !ERTS_PROC_LOCK_ATOMIC_IMPL, the pix lock is locked on entry.
 *
 * This always returns with the pix lock unlocked.
 */
void
erts_proc_lock_failed(Process *p,
		      erts_pix_lock_t *pixlck,
		      ErtsProcLocks locks,
		      ErtsProcLocks old_lflgs)
{
    int until_yield = ERTS_PROC_LOCK_SPIN_UNTIL_YIELD;
    int thr_spin_count;
    int spin_count;
    ErtsProcLocks need_locks = locks;
    ErtsProcLocks olflgs = old_lflgs;

    if (erts_thr_get_main_status())
	thr_spin_count = proc_lock_spin_count;
    else
	thr_spin_count = aux_thr_proc_lock_spin_count;

    spin_count = thr_spin_count;

    while (need_locks != 0) {
        ErtsProcLocks can_grab;

	can_grab = in_order_locks(olflgs, need_locks);

        if (can_grab == 0) {
            /* Someone already has the lowest-numbered lock we want. */

            if (spin_count-- <= 0) {
                /* Too many retries, give up and sleep for the lock. */
                wait_for_locks(p, pixlck, locks, need_locks, olflgs);
                return;
            }

	    ERTS_SPIN_BODY;

	    if (--until_yield == 0) {
		until_yield = ERTS_PROC_LOCK_SPIN_UNTIL_YIELD;
		erts_thr_yield();
	    }

            olflgs = ERTS_PROC_LOCK_FLGS_READ_(&p->lock);
        }
        else {
            /* Try to grab all of the grabbable locks at once with cmpxchg. */
            ErtsProcLocks grabbed = olflgs | can_grab;
            ErtsProcLocks nflgs =
                ERTS_PROC_LOCK_FLGS_CMPXCHG_ACQB_(&p->lock, grabbed, olflgs);

            if (nflgs == olflgs) {
                /* Success! We grabbed the 'can_grab' locks. */
                olflgs = grabbed;
                need_locks &= ~can_grab;

                /* Since we made progress, reset the spin count. */
                spin_count = thr_spin_count;
            }
            else {
                /* Compare-and-exchange failed, try again. */
                olflgs = nflgs;
            }
        }
    }

   /* Now we have all of the locks we wanted. */

#if !ERTS_PROC_LOCK_ATOMIC_IMPL
    erts_pix_unlock(pixlck);
#endif
}

/*
 * erts_proc_unlock_failed() is called when erts_proc_unlock()
 * wasn't able to unlock all locks. We may need to transfer locks
 * to waiters.
 */
void
erts_proc_unlock_failed(Process *p,
			erts_pix_lock_t *pixlck,
			ErtsProcLocks wait_locks)
{
    erts_pix_lock_t *pix_lock = pixlck ? pixlck : ERTS_PID2PIXLOCK(p->common.id);

#if ERTS_PROC_LOCK_ATOMIC_IMPL
    erts_pix_lock(pix_lock);
#endif

    transfer_locks(p, wait_locks, pix_lock, 1); /* unlocks pix_lock */
}

#endif /* ERTS_PROC_LOCK_OWN_IMPL */

void
erts_proc_lock_prepare_proc_lock_waiter(void)
{
#if ERTS_PROC_LOCK_OWN_IMPL
    tse_return(tse_fetch(NULL));
#endif
}

/*
 * proc_safelock() locks process locks on two processes. In order
 * to avoid a deadlock, proc_safelock() unlocks those locks that
 * needs to be unlocked,  and then acquires locks in lock order
 * (including the previously unlocked ones).
 */

static void
proc_safelock(int is_managed,
	      Process *a_proc,
	      ErtsProcLocks a_have_locks,
	      ErtsProcLocks a_need_locks,
	      Process *b_proc,
	      ErtsProcLocks b_have_locks,
	      ErtsProcLocks b_need_locks)
{
    Process *p1, *p2;
#ifdef ERTS_ENABLE_LOCK_CHECK
    Eterm pid1, pid2;
#endif
    ErtsProcLocks need_locks1, have_locks1, need_locks2, have_locks2;
    ErtsProcLocks unlock_mask;
    int lock_no, refc1 = 0, refc2 = 0;

    ASSERT(b_proc);


    /* Determine inter process lock order...
     * Locks with the same lock order should be locked on p1 before p2.
     */
    if (a_proc) {
	if (a_proc->common.id < b_proc->common.id) {
	    p1 = a_proc;
#ifdef ERTS_ENABLE_LOCK_CHECK
	    pid1 = a_proc->common.id;
#endif
	    need_locks1 = a_need_locks;
	    have_locks1 = a_have_locks;
	    p2 = b_proc;
#ifdef ERTS_ENABLE_LOCK_CHECK
	    pid2 = b_proc->common.id;
#endif
	    need_locks2 = b_need_locks;
	    have_locks2 = b_have_locks;
	}
	else if (a_proc->common.id > b_proc->common.id) {
	    p1 = b_proc;
#ifdef ERTS_ENABLE_LOCK_CHECK
	    pid1 = b_proc->common.id;
#endif
	    need_locks1 = b_need_locks;
	    have_locks1 = b_have_locks;
	    p2 = a_proc;
#ifdef ERTS_ENABLE_LOCK_CHECK
	    pid2 = a_proc->common.id;
#endif
	    need_locks2 = a_need_locks;
	    have_locks2 = a_have_locks;
	}
	else {
	    ASSERT(a_proc == b_proc);
	    ASSERT(a_proc->common.id == b_proc->common.id);
	    p1 = a_proc;
#ifdef ERTS_ENABLE_LOCK_CHECK
	    pid1 = a_proc->common.id;
#endif
	    need_locks1 = a_need_locks | b_need_locks;
	    have_locks1 = a_have_locks | b_have_locks;
	    p2 = NULL;
#ifdef ERTS_ENABLE_LOCK_CHECK
	    pid2 = 0;
#endif
	    need_locks2 = 0;
	    have_locks2 = 0;
	}
    }
    else {
	p1 = b_proc;
#ifdef ERTS_ENABLE_LOCK_CHECK
	pid1 = b_proc->common.id;
#endif
	need_locks1 = b_need_locks;
	have_locks1 = b_have_locks;
	p2 = NULL;
#ifdef ERTS_ENABLE_LOCK_CHECK
	pid2 = 0;
#endif
	need_locks2 = 0;
	have_locks2 = 0;
#ifdef ERTS_ENABLE_LOCK_CHECK
	a_need_locks = 0;
	a_have_locks = 0;
#endif
    }

#ifdef ERTS_ENABLE_LOCK_CHECK
    if (p1)
	erts_proc_lc_chk_proc_locks(p1, have_locks1);
    if (p2)
	erts_proc_lc_chk_proc_locks(p2, have_locks2);

    if ((need_locks1 & have_locks1) != have_locks1)
	erts_lc_fail("Thread tries to release process lock(s) "
		     "on %T via erts_proc_safelock().", pid1);
    if ((need_locks2 & have_locks2) != have_locks2)
	erts_lc_fail("Thread tries to release process lock(s) "
		     "on %T via erts_proc_safelock().",
		     pid2);
#endif


    need_locks1 &= ~have_locks1;
    need_locks2 &= ~have_locks2;

    /* Figure out the range of locks that needs to be unlocked... */
    unlock_mask = ERTS_PROC_LOCKS_ALL;
    for (lock_no = 0;
	 lock_no <= ERTS_PROC_LOCK_MAX_BIT;
	 lock_no++) {
	ErtsProcLocks lock = (1 << lock_no);
	if (lock & need_locks1)
	    break;
	unlock_mask &= ~lock;
	if (lock & need_locks2)
	    break;
    }

    /* ... and unlock locks in that range... */
    if (have_locks1 || have_locks2) {
	ErtsProcLocks unlock_locks;
	unlock_locks = unlock_mask & have_locks1;
	if (unlock_locks) {
	    have_locks1 &= ~unlock_locks;
	    need_locks1 |= unlock_locks;
	    if (!is_managed && !have_locks1) {
		refc1 = 1;
		erts_proc_inc_refc(p1);
	    }
	    erts_proc_unlock(p1, unlock_locks);
	}
	unlock_locks = unlock_mask & have_locks2;
	if (unlock_locks) {
	    have_locks2 &= ~unlock_locks;
	    need_locks2 |= unlock_locks;
	    if (!is_managed && !have_locks2) {
		refc2 = 1;
		erts_proc_inc_refc(p2);
	    }
	    erts_proc_unlock(p2, unlock_locks);
	}
    }

    /*
     * lock_no equals the number of the first lock to lock on
     * either p1 *or* p2.
     */


#ifdef ERTS_ENABLE_LOCK_CHECK
    if (p1)
	erts_proc_lc_chk_proc_locks(p1, have_locks1);
    if (p2)
	erts_proc_lc_chk_proc_locks(p2, have_locks2);
#endif

    /* Lock locks in lock order... */
    while (lock_no <= ERTS_PROC_LOCK_MAX_BIT) {
	ErtsProcLocks locks;
	ErtsProcLocks lock = (1 << lock_no);
	ErtsProcLocks lock_mask = 0;
	if (need_locks1 & lock) {
	    do {
		lock = (1 << lock_no++);
		lock_mask |= lock;
	    } while (lock_no <= ERTS_PROC_LOCK_MAX_BIT
		     && !(need_locks2 & lock));
	    if (need_locks2 & lock)
		lock_no--;
	    locks = need_locks1 & lock_mask;
	    erts_proc_lock(p1, locks);
	    have_locks1 |= locks;
	    need_locks1 &= ~locks;
	}
	else if (need_locks2 & lock) {
	    while (lock_no <= ERTS_PROC_LOCK_MAX_BIT
		   && !(need_locks1 & lock)) {
		lock_mask |= lock;
		lock = (1 << ++lock_no);
	    }
	    locks = need_locks2 & lock_mask;
	    erts_proc_lock(p2, locks);
	    have_locks2 |= locks;
	    need_locks2 &= ~locks;
	}
	else
	    lock_no++;
    }

#ifdef ERTS_ENABLE_LOCK_CHECK
    if (p1)
	erts_proc_lc_chk_proc_locks(p1, have_locks1);
    if (p2)
	erts_proc_lc_chk_proc_locks(p2, have_locks2);

    if (p1 && p2) {
	if (p1 == a_proc) {
	    ASSERT(a_need_locks == have_locks1);
	    ASSERT(b_need_locks == have_locks2);
	}
	else {
	    ASSERT(a_need_locks == have_locks2);
	    ASSERT(b_need_locks == have_locks1);
	}
    }
    else {
	ASSERT(p1);
	if (a_proc) {
	    ASSERT(have_locks1 == (a_need_locks | b_need_locks));
	}
	else {
	    ASSERT(have_locks1 == b_need_locks);
	}
    }
#endif

    if (!is_managed) {
	if (refc1)
	    erts_proc_dec_refc(p1);
	if (refc2)
	    erts_proc_dec_refc(p2);
    }
}

void
erts_proc_safelock(Process *a_proc,
		   ErtsProcLocks a_have_locks,
		   ErtsProcLocks a_need_locks,
		   Process *b_proc,
		   ErtsProcLocks b_have_locks,
		   ErtsProcLocks b_need_locks)
{
    proc_safelock(erts_get_scheduler_id() != 0,
		  a_proc,
		  a_have_locks,
		  a_need_locks,
		  b_proc,
		  b_have_locks,
		  b_need_locks);
}

Process *
erts_pid2proc_opt(Process *c_p,
		  ErtsProcLocks c_p_have_locks,
		  Eterm pid,
		  ErtsProcLocks pid_need_locks,
		  int flags)
{
    Process *dec_refc_proc = NULL;
    ErtsThrPrgrDelayHandle dhndl;
    ErtsProcLocks need_locks;
    Uint pix;
    Process *proc;
#if ERTS_PROC_LOCK_OWN_IMPL && defined(ERTS_ENABLE_LOCK_COUNT)
    ErtsProcLocks lcnt_locks;
#endif

#ifdef ERTS_ENABLE_LOCK_CHECK
    if (c_p) {
	ErtsProcLocks might_unlock = c_p_have_locks & pid_need_locks;
	if (might_unlock)
	    erts_proc_lc_might_unlock(c_p, might_unlock);
    }
#endif

    if (is_not_internal_pid(pid))
	return NULL;
    pix = internal_pid_index(pid);

    ASSERT((pid_need_locks & ERTS_PROC_LOCKS_ALL) == pid_need_locks);
    need_locks = pid_need_locks;

    if (c_p && c_p->common.id == pid) {
	ASSERT(c_p->common.id != ERTS_INVALID_PID);
	ASSERT(c_p == erts_pix2proc(pix));

	if (!(flags & ERTS_P2P_FLG_ALLOW_OTHER_X)
	    && ERTS_PROC_IS_EXITING(c_p))
	    return NULL;
	need_locks &= ~c_p_have_locks;
	if (!need_locks) {
	    if (flags & ERTS_P2P_FLG_INC_REFC)
		erts_proc_inc_refc(c_p);
	    return c_p;
	}
    }

    dhndl = erts_thr_progress_unmanaged_delay();

    proc = (Process *) erts_ptab_pix2intptr_ddrb(&erts_proc, pix);

    if (proc) {
	if (proc->common.id != pid)
	    proc = NULL;
	else if (!need_locks) {
	    if (flags & ERTS_P2P_FLG_INC_REFC)
		erts_proc_inc_refc(proc);
	}
	else {
	    int busy;

#if ERTS_PROC_LOCK_OWN_IMPL
#ifdef ERTS_ENABLE_LOCK_COUNT
	    lcnt_locks = need_locks;
	    if (!(flags & ERTS_P2P_FLG_TRY_LOCK)) {
	    	erts_lcnt_proc_lock(&proc->lock, need_locks);
	    }
#endif

#ifdef ERTS_ENABLE_LOCK_CHECK
	    /* Make sure erts_pid2proc_safelock() is enough to handle
	       a potential lock order violation situation... */
	    busy = erts_proc_lc_trylock_force_busy(proc, need_locks);
	    if (!busy)
#endif
#endif /* ERTS_PROC_LOCK_OWN_IMPL */
	    {
		/* Try a quick trylock to grab all the locks we need. */
		busy = (int) erts_proc_raw_trylock__(proc, need_locks);

#if ERTS_PROC_LOCK_OWN_IMPL && defined(ERTS_ENABLE_LOCK_CHECK)
		erts_proc_lc_trylock(proc, need_locks, !busy, __FILE__,__LINE__);
#endif
#ifdef ERTS_PROC_LOCK_DEBUG
		if (!busy)
		    erts_proc_lock_op_debug(proc, need_locks, 1);
#endif
	    }

#if ERTS_PROC_LOCK_OWN_IMPL && defined(ERTS_ENABLE_LOCK_COUNT)
	    if (flags & ERTS_P2P_FLG_TRY_LOCK)
		erts_lcnt_proc_trylock(&proc->lock, need_locks,
				       busy ? EBUSY : 0);
#endif

	    if (!busy) {
		if (flags & ERTS_P2P_FLG_INC_REFC)
		    erts_proc_inc_refc(proc);

#if ERTS_PROC_LOCK_OWN_IMPL && defined(ERTS_ENABLE_LOCK_COUNT)
	    	/* all is great */
	    	if (!(flags & ERTS_P2P_FLG_TRY_LOCK))
		    erts_lcnt_proc_lock_post_x(&proc->lock, lcnt_locks,
					       __FILE__, __LINE__);
#endif

	    }
	    else {
		if (flags & ERTS_P2P_FLG_TRY_LOCK)
		    proc = ERTS_PROC_LOCK_BUSY;
		else {
		    int managed;
		    if (flags & ERTS_P2P_FLG_INC_REFC)
			erts_proc_inc_refc(proc);

#if ERTS_PROC_LOCK_OWN_IMPL && defined(ERTS_ENABLE_LOCK_COUNT)
		    erts_lcnt_proc_lock_unacquire(&proc->lock, lcnt_locks);
#endif

		    managed = dhndl == ERTS_THR_PRGR_DHANDLE_MANAGED;
		    if (!managed) {
			erts_proc_inc_refc(proc);
			erts_thr_progress_unmanaged_continue(dhndl);
			dec_refc_proc = proc;

			/*
			 * We don't want to call
			 * erts_thr_progress_unmanaged_continue()
			 * again.
			 */
			dhndl = ERTS_THR_PRGR_DHANDLE_MANAGED;
		    }

		    proc_safelock(managed,
				  c_p,
				  c_p_have_locks,
				  c_p_have_locks,
				  proc,
				  0,
				  need_locks);
		}
	    }
        }
    }

    if (dhndl != ERTS_THR_PRGR_DHANDLE_MANAGED)
	erts_thr_progress_unmanaged_continue(dhndl);

    if (need_locks
	&& proc
	&& proc != ERTS_PROC_LOCK_BUSY
	&& (!(flags & ERTS_P2P_FLG_ALLOW_OTHER_X)
	    ? ERTS_PROC_IS_EXITING(proc)
	    : (proc
	       != (Process *) erts_ptab_pix2intptr_nob(&erts_proc, pix)))) {

	erts_proc_unlock(proc, need_locks);

	if (flags & ERTS_P2P_FLG_INC_REFC)
	    dec_refc_proc = proc;
	proc = NULL;

    }

    if (dec_refc_proc)
	erts_proc_dec_refc(dec_refc_proc);

#if ERTS_PROC_LOCK_OWN_IMPL && defined(ERTS_PROC_LOCK_DEBUG)
    ASSERT(!proc
           || proc == ERTS_PROC_LOCK_BUSY
           || (pid_need_locks ==
               (ERTS_PROC_LOCK_FLGS_READ_(&proc->lock)
                & pid_need_locks)));
#endif

    return proc;
}

static ERTS_INLINE
Process *proc_lookup_inc_refc(Eterm pid, int allow_exit)
{
    Process *proc;
    ErtsThrPrgrDelayHandle dhndl;

    dhndl = erts_thr_progress_unmanaged_delay();

    proc = erts_proc_lookup_raw(pid);
    if (proc) {
        if (!allow_exit && ERTS_PROC_IS_EXITING(proc))
            proc = NULL;
        else
            erts_proc_inc_refc(proc);
    }

    erts_thr_progress_unmanaged_continue(dhndl);

    return proc;
}

Process *erts_proc_lookup_inc_refc(Eterm pid)
{
    return proc_lookup_inc_refc(pid, 0);
}

Process *erts_proc_lookup_raw_inc_refc(Eterm pid)
{
    return proc_lookup_inc_refc(pid, 1);
}

void
erts_proc_lock_init(Process *p)
{
#if ERTS_PROC_LOCK_OWN_IMPL || defined(ERTS_PROC_LOCK_DEBUG)
    int i;
#endif
#if ERTS_PROC_LOCK_OWN_IMPL
    /* We always start with all locks locked */
#if ERTS_PROC_LOCK_ATOMIC_IMPL
    erts_atomic32_init_nob(&p->lock.flags,
			       (erts_aint32_t) ERTS_PROC_LOCKS_ALL);
#else
    p->lock.flags = ERTS_PROC_LOCKS_ALL;
#endif
    for (i = 0; i <= ERTS_PROC_LOCK_MAX_BIT; i++)
	p->lock.queue[i] = NULL;
#ifdef ERTS_ENABLE_LOCK_CHECK
    erts_proc_lc_trylock(p, ERTS_PROC_LOCKS_ALL, 1,__FILE__,__LINE__);
#endif
#elif ERTS_PROC_LOCK_RAW_MUTEX_IMPL

    erts_mtx_init(&p->lock.main, "proc_main", p->common.id,
        ERTS_LOCK_FLAGS_CATEGORY_PROCESS);
    ethr_mutex_lock(&p->lock.main.mtx);
#ifdef ERTS_ENABLE_LOCK_CHECK
    erts_lc_trylock(1, &p->lock.main.lc);
#endif
    erts_mtx_init(&p->lock.msgq, "proc_msgq", p->common.id,
        ERTS_LOCK_FLAGS_CATEGORY_PROCESS);
    ethr_mutex_lock(&p->lock.msgq.mtx);
#ifdef ERTS_ENABLE_LOCK_CHECK
    erts_lc_trylock(1, &p->lock.msgq.lc);
#endif
    erts_mtx_init(&p->lock.btm, "proc_btm", p->common.id,
        ERTS_LOCK_FLAGS_CATEGORY_PROCESS);
    ethr_mutex_lock(&p->lock.btm.mtx);
#ifdef ERTS_ENABLE_LOCK_CHECK
    erts_lc_trylock(1, &p->lock.btm.lc);
#endif
    erts_mtx_init(&p->lock.status, "proc_status", p->common.id,
        ERTS_LOCK_FLAGS_CATEGORY_PROCESS);
    ethr_mutex_lock(&p->lock.status.mtx);
#ifdef ERTS_ENABLE_LOCK_CHECK
    erts_lc_trylock(1, &p->lock.status.lc);
#endif
    erts_mtx_init(&p->lock.trace, "proc_trace", p->common.id,
        ERTS_LOCK_FLAGS_CATEGORY_PROCESS);
    ethr_mutex_lock(&p->lock.trace.mtx);
#ifdef ERTS_ENABLE_LOCK_CHECK
    erts_lc_trylock(1, &p->lock.trace.lc);
#endif
#endif
#ifdef ERTS_PROC_LOCK_DEBUG
    for (i = 0; i <= ERTS_PROC_LOCK_MAX_BIT; i++)
	erts_atomic32_init_nob(&p->lock.locked[i], (erts_aint32_t) 1);
#endif
#ifdef ERTS_ENABLE_LOCK_COUNT
    erts_lcnt_proc_lock_init(p);
    erts_lcnt_proc_lock(&(p->lock), ERTS_PROC_LOCKS_ALL);
    erts_lcnt_proc_lock_post_x(&(p->lock), ERTS_PROC_LOCKS_ALL, __FILE__, __LINE__);
#endif
}

void
erts_proc_lock_fin(Process *p)
{
#if ERTS_PROC_LOCK_RAW_MUTEX_IMPL
    erts_mtx_destroy(&p->lock.main);
    erts_mtx_destroy(&p->lock.msgq);
    erts_mtx_destroy(&p->lock.btm);
    erts_mtx_destroy(&p->lock.status);
    erts_mtx_destroy(&p->lock.trace);
#endif
#if defined(ERTS_ENABLE_LOCK_COUNT)
    erts_lcnt_proc_lock_destroy(p);
#endif
}

/* --- Process lock counting ----------------------------------------------- */

#if ERTS_PROC_LOCK_OWN_IMPL && defined(ERTS_ENABLE_LOCK_COUNT)

void erts_lcnt_proc_lock_init(Process *p) {
    erts_lcnt_init_ref(&p->lock.lcnt_carrier);

    if(erts_lcnt_check_enabled(ERTS_LOCK_FLAGS_CATEGORY_PROCESS)) {
        erts_lcnt_enable_proc_lock_count(p, 1);
    }
} /* logic reversed */

void erts_lcnt_proc_lock_destroy(Process *p) {
    erts_lcnt_uninstall(&p->lock.lcnt_carrier);
}

void erts_lcnt_enable_proc_lock_count(Process *proc, int enable) {
    if(proc->common.id == ERTS_INVALID_PID) {
        /* Locks without an id are more trouble than they're worth; there's no
         * way to look them up and we can't track them with _STATIC since it's
         * too early to tell whether we're a system process (proc->static_flags
         * hasn't been not set yet). */
    } else if(!enable) {
        erts_lcnt_proc_lock_destroy(proc);
    } else if(!erts_lcnt_check_ref_installed(&proc->lock.lcnt_carrier)) {
        erts_lcnt_lock_info_carrier_t *carrier;

        carrier = erts_lcnt_create_lock_info_carrier(ERTS_LCNT_PROCLOCK_COUNT);

        erts_lcnt_init_lock_info_idx(carrier, ERTS_LCNT_PROCLOCK_IDX_MAIN,
            "proc_main", proc->common.id, ERTS_LOCK_TYPE_PROCLOCK);
        erts_lcnt_init_lock_info_idx(carrier, ERTS_LCNT_PROCLOCK_IDX_MSGQ,
            "proc_msgq", proc->common.id, ERTS_LOCK_TYPE_PROCLOCK);
        erts_lcnt_init_lock_info_idx(carrier, ERTS_LCNT_PROCLOCK_IDX_BTM,
            "proc_btm", proc->common.id, ERTS_LOCK_TYPE_PROCLOCK);
        erts_lcnt_init_lock_info_idx(carrier, ERTS_LCNT_PROCLOCK_IDX_STATUS,
            "proc_status",proc->common.id, ERTS_LOCK_TYPE_PROCLOCK);
        erts_lcnt_init_lock_info_idx(carrier, ERTS_LCNT_PROCLOCK_IDX_TRACE,
            "proc_trace", proc->common.id, ERTS_LOCK_TYPE_PROCLOCK);

        erts_lcnt_install(&proc->lock.lcnt_carrier, carrier);
    }
}

void erts_lcnt_update_process_locks(int enable) {
    int i, max;

    max = erts_ptab_max(&erts_proc);

    for(i = 0; i < max; i++) {
        int delay_handle;
        Process *proc;

        delay_handle = erts_thr_progress_unmanaged_delay();
        proc = erts_pix2proc(i);

        if(proc != NULL) {
            erts_lcnt_enable_proc_lock_count(proc, enable);
        }

        if(delay_handle != ERTS_THR_PRGR_DHANDLE_MANAGED) {
            erts_thr_progress_unmanaged_continue(delay_handle);
        }
    }
}

#endif /* ERTS_ENABLE_LOCK_COUNT */


/* --- Process lock checking ----------------------------------------------- */

#ifdef ERTS_ENABLE_LOCK_CHECK

#if ERTS_PROC_LOCK_OWN_IMPL

void
erts_proc_lc_lock(Process *p, ErtsProcLocks locks, const char *file, unsigned int line)
{
    erts_lc_lock_t lck = ERTS_LC_LOCK_INIT(-1,
					   p->common.id,
					   ERTS_LOCK_TYPE_PROCLOCK);
    if (locks & ERTS_PROC_LOCK_MAIN) {
	lck.id = lc_id.proc_lock_main;
	erts_lc_lock_x(&lck,file,line);
    }
    if (locks & ERTS_PROC_LOCK_MSGQ) {
	lck.id = lc_id.proc_lock_msgq;
	erts_lc_lock_x(&lck,file,line);
    }
    if (locks & ERTS_PROC_LOCK_BTM) {
	lck.id = lc_id.proc_lock_btm;
	erts_lc_lock_x(&lck,file,line);
    }
    if (locks & ERTS_PROC_LOCK_STATUS) {
	lck.id = lc_id.proc_lock_status;
	erts_lc_lock_x(&lck,file,line);
    }
    if (locks & ERTS_PROC_LOCK_TRACE) {
	lck.id = lc_id.proc_lock_trace;
	erts_lc_lock_x(&lck,file,line);
    }
}

void
erts_proc_lc_trylock(Process *p, ErtsProcLocks locks, int locked,
		     const char *file, unsigned int line)
{
    erts_lc_lock_t lck = ERTS_LC_LOCK_INIT(-1,
					   p->common.id,
					   ERTS_LOCK_TYPE_PROCLOCK);
    if (locks & ERTS_PROC_LOCK_MAIN) {
	lck.id = lc_id.proc_lock_main;
	erts_lc_trylock_x(locked, &lck, file, line);
    }
    if (locks & ERTS_PROC_LOCK_MSGQ) {
	lck.id = lc_id.proc_lock_msgq;
	erts_lc_trylock_x(locked, &lck, file, line);
    }
    if (locks & ERTS_PROC_LOCK_BTM) {
	lck.id = lc_id.proc_lock_btm;
	erts_lc_trylock_x(locked, &lck, file, line);
    }
    if (locks & ERTS_PROC_LOCK_STATUS) {
	lck.id = lc_id.proc_lock_status;
	erts_lc_trylock_x(locked, &lck, file, line);
    }
    if (locks & ERTS_PROC_LOCK_TRACE) {
	lck.id = lc_id.proc_lock_trace;
	erts_lc_trylock_x(locked, &lck, file, line);
    }
}

void
erts_proc_lc_unlock(Process *p, ErtsProcLocks locks)
{
    erts_lc_lock_t lck = ERTS_LC_LOCK_INIT(-1,
					   p->common.id,
					   ERTS_LOCK_TYPE_PROCLOCK);
    if (locks & ERTS_PROC_LOCK_TRACE) {
	lck.id = lc_id.proc_lock_trace;
	erts_lc_unlock(&lck);
    }
    if (locks & ERTS_PROC_LOCK_STATUS) {
	lck.id = lc_id.proc_lock_status;
	erts_lc_unlock(&lck);
    }
    if (locks & ERTS_PROC_LOCK_BTM) {
	lck.id = lc_id.proc_lock_btm;
	erts_lc_unlock(&lck);
    }
    if (locks & ERTS_PROC_LOCK_MSGQ) {
	lck.id = lc_id.proc_lock_msgq;
	erts_lc_unlock(&lck);
    }
    if (locks & ERTS_PROC_LOCK_MAIN) {
	lck.id = lc_id.proc_lock_main;
	erts_lc_unlock(&lck);
    }
}

#endif /* ERTS_PROC_LOCK_OWN_IMPL */

void
erts_proc_lc_might_unlock(Process *p, ErtsProcLocks locks)
{
#if ERTS_PROC_LOCK_OWN_IMPL
    erts_lc_lock_t lck = ERTS_LC_LOCK_INIT(-1,
					   p->common.id,
					   ERTS_LOCK_TYPE_PROCLOCK);
    if (locks & ERTS_PROC_LOCK_TRACE) {
	lck.id = lc_id.proc_lock_trace;
	erts_lc_might_unlock(&lck);
    }
    if (locks & ERTS_PROC_LOCK_STATUS) {
	lck.id = lc_id.proc_lock_status;
	erts_lc_might_unlock(&lck);
    }
    if (locks & ERTS_PROC_LOCK_BTM) {
	lck.id = lc_id.proc_lock_btm;
	erts_lc_might_unlock(&lck);
    }
    if (locks & ERTS_PROC_LOCK_MSGQ) {
	lck.id = lc_id.proc_lock_msgq;
	erts_lc_might_unlock(&lck);
    }
    if (locks & ERTS_PROC_LOCK_MAIN) {
	lck.id = lc_id.proc_lock_main;
	erts_lc_might_unlock(&lck);
    }
#elif ERTS_PROC_LOCK_RAW_MUTEX_IMPL
    if (locks & ERTS_PROC_LOCK_MAIN)
	erts_lc_might_unlock(&p->lock.main.lc);
    if (locks & ERTS_PROC_LOCK_MSGQ)
	erts_lc_might_unlock(&p->lock.msgq.lc);
    if (locks & ERTS_PROC_LOCK_BTM)
	erts_lc_might_unlock(&p->lock.btm.lc);
    if (locks & ERTS_PROC_LOCK_STATUS)
	erts_lc_might_unlock(&p->lock.status.lc);
    if (locks & ERTS_PROC_LOCK_TRACE)
	erts_lc_might_unlock(&p->lock.trace.lc);
#endif
}

void
erts_proc_lc_require_lock(Process *p, ErtsProcLocks locks, const char *file,
			  unsigned int line)
{
#if ERTS_PROC_LOCK_OWN_IMPL
    erts_lc_lock_t lck = ERTS_LC_LOCK_INIT(-1,
					   p->common.id,
					   ERTS_LOCK_TYPE_PROCLOCK);
    if (locks & ERTS_PROC_LOCK_MAIN) {
	lck.id = lc_id.proc_lock_main;
	erts_lc_require_lock(&lck, file, line);
    }
    if (locks & ERTS_PROC_LOCK_MSGQ) {
	lck.id = lc_id.proc_lock_msgq;
	erts_lc_require_lock(&lck, file, line);
    }
    if (locks & ERTS_PROC_LOCK_BTM) {
	lck.id = lc_id.proc_lock_btm;
	erts_lc_require_lock(&lck, file, line);
    }
    if (locks & ERTS_PROC_LOCK_STATUS) {
	lck.id = lc_id.proc_lock_status;
	erts_lc_require_lock(&lck, file, line);
    }
    if (locks & ERTS_PROC_LOCK_TRACE) {
	lck.id = lc_id.proc_lock_trace;
	erts_lc_require_lock(&lck, file, line);
    }
#elif ERTS_PROC_LOCK_RAW_MUTEX_IMPL
    if (locks & ERTS_PROC_LOCK_MAIN)
	erts_lc_require_lock(&p->lock.main.lc, file, line);
    if (locks & ERTS_PROC_LOCK_MSGQ)
	erts_lc_require_lock(&p->lock.msgq.lc, file, line);
    if (locks & ERTS_PROC_LOCK_BTM)
	erts_lc_require_lock(&p->lock.btm.lc, file, line);
    if (locks & ERTS_PROC_LOCK_STATUS)
	erts_lc_require_lock(&p->lock.status.lc, file, line);
    if (locks & ERTS_PROC_LOCK_TRACE)
	erts_lc_require_lock(&p->lock.trace.lc, file, line);
#endif
}

void
erts_proc_lc_unrequire_lock(Process *p, ErtsProcLocks locks)
{
#if ERTS_PROC_LOCK_OWN_IMPL
    erts_lc_lock_t lck = ERTS_LC_LOCK_INIT(-1,
					   p->common.id,
					   ERTS_LOCK_TYPE_PROCLOCK);
    if (locks & ERTS_PROC_LOCK_TRACE) {
	lck.id = lc_id.proc_lock_trace;
	erts_lc_unrequire_lock(&lck);
    }
    if (locks & ERTS_PROC_LOCK_STATUS) {
	lck.id = lc_id.proc_lock_status;
	erts_lc_unrequire_lock(&lck);
    }
    if (locks & ERTS_PROC_LOCK_BTM) {
	lck.id = lc_id.proc_lock_btm;
	erts_lc_unrequire_lock(&lck);
    }
    if (locks & ERTS_PROC_LOCK_MSGQ) {
	lck.id = lc_id.proc_lock_msgq;
	erts_lc_unrequire_lock(&lck);
    }
    if (locks & ERTS_PROC_LOCK_MAIN) {
	lck.id = lc_id.proc_lock_main;
	erts_lc_unrequire_lock(&lck);
    }
#elif ERTS_PROC_LOCK_RAW_MUTEX_IMPL
    if (locks & ERTS_PROC_LOCK_MAIN)
	erts_lc_unrequire_lock(&p->lock.main.lc);
    if (locks & ERTS_PROC_LOCK_MSGQ)
	erts_lc_unrequire_lock(&p->lock.msgq.lc);
    if (locks & ERTS_PROC_LOCK_BTM)
	erts_lc_unrequire_lock(&p->lock.btm.lc);
    if (locks & ERTS_PROC_LOCK_STATUS)
	erts_lc_unrequire_lock(&p->lock.status.lc);
    if (locks & ERTS_PROC_LOCK_TRACE)
	erts_lc_unrequire_lock(&p->lock.trace.lc);
#endif
}

#if ERTS_PROC_LOCK_OWN_IMPL

int
erts_proc_lc_trylock_force_busy(Process *p, ErtsProcLocks locks)
{
    if (locks & ERTS_PROC_LOCKS_ALL) {
	erts_lc_lock_t lck = ERTS_LC_LOCK_INIT(-1,
					       p->common.id,
					       ERTS_LOCK_TYPE_PROCLOCK);

	if (locks & ERTS_PROC_LOCK_MAIN)
	    lck.id = lc_id.proc_lock_main;
	else if (locks & ERTS_PROC_LOCK_MSGQ)
	    lck.id = lc_id.proc_lock_msgq;
	else if (locks & ERTS_PROC_LOCK_BTM)
	    lck.id = lc_id.proc_lock_btm;
	else if (locks & ERTS_PROC_LOCK_STATUS)
	    lck.id = lc_id.proc_lock_status;
	else if (locks & ERTS_PROC_LOCK_TRACE)
	    lck.id = lc_id.proc_lock_trace;
	else
	    erts_lc_fail("Unknown proc lock found");

	return erts_lc_trylock_force_busy(&lck);
    }
    return 0;
}

#endif /* ERTS_PROC_LOCK_OWN_IMPL */

void erts_proc_lc_chk_only_proc_main(Process *p)
{
    erts_proc_lc_chk_only_proc(p, ERTS_PROC_LOCK_MAIN);
}

#if ERTS_PROC_LOCK_OWN_IMPL
#define ERTS_PROC_LC_EMPTY_LOCK_INIT \
  ERTS_LC_LOCK_INIT(-1, THE_NON_VALUE, ERTS_LOCK_TYPE_PROCLOCK)
#endif /* ERTS_PROC_LOCK_OWN_IMPL */

void erts_proc_lc_chk_only_proc(Process *p, ErtsProcLocks locks)
{
    int have_locks_len = 0;
#if ERTS_PROC_LOCK_OWN_IMPL
    erts_lc_lock_t have_locks[6] = {ERTS_PROC_LC_EMPTY_LOCK_INIT,
				    ERTS_PROC_LC_EMPTY_LOCK_INIT,
				    ERTS_PROC_LC_EMPTY_LOCK_INIT,
				    ERTS_PROC_LC_EMPTY_LOCK_INIT,
				    ERTS_PROC_LC_EMPTY_LOCK_INIT,
                                    ERTS_PROC_LC_EMPTY_LOCK_INIT};
    if (locks & ERTS_PROC_LOCK_MAIN) {
	have_locks[have_locks_len].id = lc_id.proc_lock_main;
	have_locks[have_locks_len++].extra = p->common.id;
    }
    if (locks & ERTS_PROC_LOCK_MSGQ) {
	have_locks[have_locks_len].id = lc_id.proc_lock_msgq;
	have_locks[have_locks_len++].extra = p->common.id;
    }
    if (locks & ERTS_PROC_LOCK_BTM) {
	have_locks[have_locks_len].id = lc_id.proc_lock_btm;
	have_locks[have_locks_len++].extra = p->common.id;
    }
    if (locks & ERTS_PROC_LOCK_STATUS) {
	have_locks[have_locks_len].id = lc_id.proc_lock_status;
	have_locks[have_locks_len++].extra = p->common.id;
    }
    if (locks & ERTS_PROC_LOCK_TRACE) {
	have_locks[have_locks_len].id = lc_id.proc_lock_trace;
	have_locks[have_locks_len++].extra = p->common.id;
    }
#elif ERTS_PROC_LOCK_RAW_MUTEX_IMPL
    erts_lc_lock_t have_locks[6];
    if (locks & ERTS_PROC_LOCK_MAIN)
	have_locks[have_locks_len++] = p->lock.main.lc;
    if (locks & ERTS_PROC_LOCK_MSGQ)
	have_locks[have_locks_len++] = p->lock.msgq.lc;
    if (locks & ERTS_PROC_LOCK_BTM)
	have_locks[have_locks_len++] = p->lock.btm.lc;
    if (locks & ERTS_PROC_LOCK_STATUS)
	have_locks[have_locks_len++] = p->lock.status.lc;
    if (locks & ERTS_PROC_LOCK_TRACE)
	have_locks[have_locks_len++] = p->lock.trace.lc;
#endif
    erts_lc_check_exact(have_locks, have_locks_len);
}

void
erts_proc_lc_chk_have_proc_locks(Process *p, ErtsProcLocks locks)
{
    int have_locks_len = 0;
#if ERTS_PROC_LOCK_OWN_IMPL
    erts_lc_lock_t have_locks[6] = {ERTS_PROC_LC_EMPTY_LOCK_INIT,
				    ERTS_PROC_LC_EMPTY_LOCK_INIT,
				    ERTS_PROC_LC_EMPTY_LOCK_INIT,
                                    ERTS_PROC_LC_EMPTY_LOCK_INIT,
				    ERTS_PROC_LC_EMPTY_LOCK_INIT,
				    ERTS_PROC_LC_EMPTY_LOCK_INIT};
    if (locks & ERTS_PROC_LOCK_MAIN) {
	have_locks[have_locks_len].id = lc_id.proc_lock_main;
	have_locks[have_locks_len++].extra = p->common.id;
    }
    if (locks & ERTS_PROC_LOCK_MSGQ) {
	have_locks[have_locks_len].id = lc_id.proc_lock_msgq;
	have_locks[have_locks_len++].extra = p->common.id;
    }
    if (locks & ERTS_PROC_LOCK_BTM) {
	have_locks[have_locks_len].id = lc_id.proc_lock_btm;
	have_locks[have_locks_len++].extra = p->common.id;
    }
    if (locks & ERTS_PROC_LOCK_STATUS) {
	have_locks[have_locks_len].id = lc_id.proc_lock_status;
	have_locks[have_locks_len++].extra = p->common.id;
    }
    if (locks & ERTS_PROC_LOCK_TRACE) {
	have_locks[have_locks_len].id = lc_id.proc_lock_trace;
	have_locks[have_locks_len++].extra = p->common.id;
    }
#elif ERTS_PROC_LOCK_RAW_MUTEX_IMPL
    erts_lc_lock_t have_locks[6];
    if (locks & ERTS_PROC_LOCK_MAIN)
	have_locks[have_locks_len++] = p->lock.main.lc;
    if (locks & ERTS_PROC_LOCK_MSGQ)
	have_locks[have_locks_len++] = p->lock.msgq.lc;
    if (locks & ERTS_PROC_LOCK_BTM)
	have_locks[have_locks_len++] = p->lock.btm.lc;
    if (locks & ERTS_PROC_LOCK_STATUS)
	have_locks[have_locks_len++] = p->lock.status.lc;
    if (locks & ERTS_PROC_LOCK_TRACE)
	have_locks[have_locks_len++] = p->lock.trace.lc;
#endif
    erts_lc_check(have_locks, have_locks_len, NULL, 0);
}

void
erts_proc_lc_chk_proc_locks(Process *p, ErtsProcLocks locks)
{
    int have_locks_len = 0;
    int have_not_locks_len = 0;
#if ERTS_PROC_LOCK_OWN_IMPL
    erts_lc_lock_t have_locks[6] = {ERTS_PROC_LC_EMPTY_LOCK_INIT,
				    ERTS_PROC_LC_EMPTY_LOCK_INIT,
                                    ERTS_PROC_LC_EMPTY_LOCK_INIT,
				    ERTS_PROC_LC_EMPTY_LOCK_INIT,
				    ERTS_PROC_LC_EMPTY_LOCK_INIT};
    erts_lc_lock_t have_not_locks[6] = {ERTS_PROC_LC_EMPTY_LOCK_INIT,
					ERTS_PROC_LC_EMPTY_LOCK_INIT,
                                        ERTS_PROC_LC_EMPTY_LOCK_INIT,
					ERTS_PROC_LC_EMPTY_LOCK_INIT,
					ERTS_PROC_LC_EMPTY_LOCK_INIT};

    if (locks & ERTS_PROC_LOCK_MAIN) {
	have_locks[have_locks_len].id = lc_id.proc_lock_main;
	have_locks[have_locks_len++].extra = p->common.id;
    }
    else {
	have_not_locks[have_not_locks_len].id = lc_id.proc_lock_main;
	have_not_locks[have_not_locks_len++].extra = p->common.id;
    }
    if (locks & ERTS_PROC_LOCK_MSGQ) {
	have_locks[have_locks_len].id = lc_id.proc_lock_msgq;
	have_locks[have_locks_len++].extra = p->common.id;
    }
    else {
	have_not_locks[have_not_locks_len].id = lc_id.proc_lock_msgq;
	have_not_locks[have_not_locks_len++].extra = p->common.id;
    }
    if (locks & ERTS_PROC_LOCK_BTM) {
	have_locks[have_locks_len].id = lc_id.proc_lock_btm;
	have_locks[have_locks_len++].extra = p->common.id;
    }
    else {
	have_not_locks[have_not_locks_len].id = lc_id.proc_lock_btm;
	have_not_locks[have_not_locks_len++].extra = p->common.id;
    }
    if (locks & ERTS_PROC_LOCK_STATUS) {
	have_locks[have_locks_len].id = lc_id.proc_lock_status;
	have_locks[have_locks_len++].extra = p->common.id;
    }
    else {
	have_not_locks[have_not_locks_len].id = lc_id.proc_lock_status;
	have_not_locks[have_not_locks_len++].extra = p->common.id;
    }
    if (locks & ERTS_PROC_LOCK_TRACE) {
	have_locks[have_locks_len].id = lc_id.proc_lock_trace;
	have_locks[have_locks_len++].extra = p->common.id;
    }
    else {
	have_not_locks[have_not_locks_len].id = lc_id.proc_lock_trace;
	have_not_locks[have_not_locks_len++].extra = p->common.id;
    }
#elif ERTS_PROC_LOCK_RAW_MUTEX_IMPL
    erts_lc_lock_t have_locks[6];
    erts_lc_lock_t have_not_locks[6];

    if (locks & ERTS_PROC_LOCK_MAIN)
	have_locks[have_locks_len++] = p->lock.main.lc;
    else
	have_not_locks[have_not_locks_len++] = p->lock.main.lc;
    if (locks & ERTS_PROC_LOCK_MSGQ)
	have_locks[have_locks_len++] = p->lock.msgq.lc;
    else
	have_not_locks[have_not_locks_len++] = p->lock.msgq.lc;
    if (locks & ERTS_PROC_LOCK_BTM)
	have_locks[have_locks_len++] = p->lock.btm.lc;
    else
	have_not_locks[have_not_locks_len++] = p->lock.btm.lc;
    if (locks & ERTS_PROC_LOCK_STATUS)
	have_locks[have_locks_len++] = p->lock.status.lc;
    else
	have_not_locks[have_not_locks_len++] = p->lock.status.lc;
    if (locks & ERTS_PROC_LOCK_TRACE)
	have_locks[have_locks_len++] = p->lock.trace.lc;
    else
	have_not_locks[have_not_locks_len++] = p->lock.trace.lc;
#endif

    erts_lc_check(have_locks, have_locks_len,
		  have_not_locks, have_not_locks_len);
}

ErtsProcLocks
erts_proc_lc_my_proc_locks(Process *p)
{
    int resv[5];
    ErtsProcLocks res = 0;
#if ERTS_PROC_LOCK_OWN_IMPL
    erts_lc_lock_t locks[5] = {ERTS_LC_LOCK_INIT(lc_id.proc_lock_main,
						 p->common.id,
						 ERTS_LOCK_TYPE_PROCLOCK),
			       ERTS_LC_LOCK_INIT(lc_id.proc_lock_msgq,
						 p->common.id,
						 ERTS_LOCK_TYPE_PROCLOCK),
			       ERTS_LC_LOCK_INIT(lc_id.proc_lock_btm,
						 p->common.id,
						 ERTS_LOCK_TYPE_PROCLOCK),
			       ERTS_LC_LOCK_INIT(lc_id.proc_lock_status,
						 p->common.id,
						 ERTS_LOCK_TYPE_PROCLOCK),
			       ERTS_LC_LOCK_INIT(lc_id.proc_lock_trace,
						 p->common.id,
						 ERTS_LOCK_TYPE_PROCLOCK)};
#elif ERTS_PROC_LOCK_RAW_MUTEX_IMPL
    erts_lc_lock_t locks[5] = {p->lock.main.lc,
			       p->lock.msgq.lc,
			       p->lock.btm.lc,
			       p->lock.status.lc,
			       p->lock.trace.lc};
#endif

    erts_lc_have_locks(resv, locks, 5);
    if (resv[0])
	res |= ERTS_PROC_LOCK_MAIN;
    if (resv[1])
	res |= ERTS_PROC_LOCK_MSGQ;
    if (resv[2])
	res |= ERTS_PROC_LOCK_BTM;
    if (resv[3])
	res |= ERTS_PROC_LOCK_STATUS;
    if (resv[4])
	res |= ERTS_PROC_LOCK_TRACE;

    return res;
}

void
erts_proc_lc_chk_no_proc_locks(const char *file, int line)
{
    int resv[5];
    int ids[5] = {lc_id.proc_lock_main,
		  lc_id.proc_lock_msgq,
		  lc_id.proc_lock_btm,
		  lc_id.proc_lock_status,
		  lc_id.proc_lock_trace};
    erts_lc_have_lock_ids(resv, ids, 5);
    if (!ERTS_IS_CRASH_DUMPING && (resv[0] || resv[1] || resv[2] || resv[3] || resv[4])) {
	erts_lc_fail("%s:%d: Thread has process locks locked when expected "
		     "not to have any process locks locked",
		     file, line);
    }
}

#endif /* #ifdef ERTS_ENABLE_LOCK_CHECK */

#if ERTS_PROC_LOCK_OWN_IMPL && defined(ERTS_PROC_LOCK_HARD_DEBUG)
void
check_queue(erts_proc_lock_t *lck)
{
    int lock_no;
    ErtsProcLocks lflgs = ERTS_PROC_LOCK_FLGS_READ_(lck);

    for (lock_no = 0; lock_no <= ERTS_PROC_LOCK_MAX_BIT; lock_no++) {
	ErtsProcLocks bit;
	bit = (((ErtsProcLocks) 1) << lock_no) << ERTS_PROC_LOCK_WAITER_SHIFT;
	if (lflgs & bit) {
	    int n;
	    erts_tse_t *wtr;
	    ERTS_ASSERT(lck->queue[lock_no]);
	    wtr = lck->queue[lock_no];
	    n = 0;
	    do {
		wtr = wtr->next;
		n++;
	    } while (wtr != lck->queue[lock_no]);
	    do {
		wtr = wtr->prev;
		n--;
	    } while (wtr != lck->queue[lock_no]);
	    ERTS_ASSERT(n == 0);
	}
	else {
	    ERTS_ASSERT(!lck->queue[lock_no]);
	}
    }
}
#endif

