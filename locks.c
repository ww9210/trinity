#include <errno.h>
#include <signal.h>
#include <sys/types.h>
#include <unistd.h>
#include "debug.h"
#include "locks.h"
#include "log.h"
#include "pids.h"
#include "trinity.h"
#include "utils.h"

/*
 * Check that the processes holding locks are still alive.
 * And if they are, ensure they haven't held them for an
 * excessive length of time.
 */
#define STEAL_THRESHOLD 1000000

static bool check_lock(lock_t *_lock)
{
	pid_t pid;

	/* We don't care about unlocked or locking-in-progress */
	if (_lock->lock != LOCKED)
		return FALSE;

	/* First the easy case. If it's held by a dead pid, release it. */
	pid = _lock->owner;
	if (pid_alive(pid) == -1) {
		if (errno != ESRCH)
			return TRUE;

		debugf("Found a lock held by dead pid %d. Freeing.\n", pid);
		goto unlock;
	}

	//FIXME: Remove the whole stealing mechanism.
	/* If a pid has had a lock a long time, something is up. */
	if (_lock->contention > STEAL_THRESHOLD) {
		debugf("pid %d has held lock for too long. Releasing, and killing.\n", pid);
		kill_pid(pid);
		goto unlock;
	}
	return FALSE;
unlock:
	unlock(_lock);
	return TRUE;
}

/* returns TRUE if something is awry */
bool check_all_locks(void)
{
	unsigned int i;
	bool ret = FALSE;

	check_lock(&shm->reaper_lock);
	check_lock(&shm->syscalltable_lock);

	for_each_child(i)
		ret |= check_lock(&shm->children[i]->syscall.lock);

	return ret;
}

void lock(lock_t *_lock)
{
	pid_t pid = getpid();

	while (_lock->lock != UNLOCKED) {
		if (_lock->owner == pid) {
			debugf("lol, already have lock!\n");
			show_backtrace();
			panic(EXIT_LOCKING_CATASTROPHE);
			_exit(EXIT_FAILURE);
		}

		/* This is pretty horrible. But if we call lock()
		 * from stuck_syscall_info(), and a child is hogging a lock
		 * (or worse, a dead child), we'll deadlock, because main won't
		 *  ever get back, and subsequently check_lock().
		 * So we add an extra explicit check here.
		 */
		if (pid == shm->mainpid) {
			check_lock(_lock);
		} else {
			/* Ok, we're a child pid.
			 * if something bad happened, like main crashed,
			 * we don't want to spin forever, so just get out.
			 */
			if ((shm->exit_reason != STILL_RUNNING) &&
			    (shm->exit_reason != EXIT_REACHED_COUNT)) {
				_exit(EXIT_FAILURE);
			}
		}

		_lock->contention++;
		usleep(1);
	}

	_lock->lock = LOCKING;
	_lock->contention = 0;
	_lock->owner = pid;
	_lock->lock = LOCKED;
}

void unlock(lock_t *_lock)
{
	asm volatile("" ::: "memory");
	_lock->contention = 0;
	_lock->owner = 0;
	_lock->lock = UNLOCKED;
}

/*
 * Release a lock we already hold.
 *
 * This function should be used sparingly. It's pretty much never something
 * that you'll need, just for rare occasions like when we return from a
 * signal handler with a lock held.
 */
void bust_lock(lock_t *_lock)
{
	if (_lock->lock == UNLOCKED)
		return;
	if (getpid() != _lock->owner)
		return;
	unlock(_lock);
}
