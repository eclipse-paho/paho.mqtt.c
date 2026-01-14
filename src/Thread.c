/*******************************************************************************
 * Copyright (c) 2009, 2025, 2026 IBM Corp. and Ian Craggs
 *
 * All rights reserved. This program and the accompanying materials
 * are made available under the terms of the Eclipse Public License v2.0
 * and Eclipse Distribution License v1.0 which accompany this distribution.
 *
 * The Eclipse Public License is available at
 *    https://www.eclipse.org/legal/epl-2.0/
 * and the Eclipse Distribution License is available at
 *   http://www.eclipse.org/org/documents/edl-v10.php.
 *
 * Contributors:
 *    Ian Craggs - initial implementation
 *    Ian Craggs, Allan Stockdill-Mander - async client updates
 *    Ian Craggs - bug #415042 - start Linux thread as disconnected
 *    Ian Craggs - fix for bug #420851
 *    Ian Craggs - change MacOS semaphore implementation
 *    Ian Craggs - fix for clock #284
 *    Frank Pagliughi - Consolidated semaphores and conditions into "events"
 *******************************************************************************/

/**
 * @file
 * \brief Threading related functions
 *
 * Used to create platform independent threading functions
 */


#include "Thread.h"
#if defined(THREAD_UNIT_TESTS)
#define NOSTACKTRACE
#endif
#include "Log.h"
#include "StackTrace.h"

#undef malloc
#undef realloc
#undef free

#if !defined(_WIN32)
#include <errno.h>
#include <unistd.h>
#include <sys/time.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <limits.h>
#endif
#include <stdlib.h>

#include "OsWrapper.h"

#if !defined(NSEC_PER_SEC)
#define NSEC_PER_SEC 1000000000L
#endif

/**
 * Start a new thread
 * @param fn the function to run, must be of the correct signature
 * @param parameter pointer to the function parameter, can be NULL
 */
void Paho_thread_start(thread_fn fn, void* parameter)
{
#if defined(_WIN32)
	thread_type thread = NULL;
#else
	thread_type thread = 0;
	pthread_attr_t attr;
#endif

	FUNC_ENTRY;
#if defined(_WIN32)
	thread = CreateThread(NULL, 0, fn, parameter, 0, NULL);
    CloseHandle(thread);
#else
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	if (pthread_create(&thread, &attr, fn, parameter) != 0)
		thread = 0;
	pthread_attr_destroy(&attr);
#endif
	FUNC_EXIT;
}


int Thread_set_name(const char* thread_name)
{
	int rc = 0;
/*
#if defined(_WIN32)
#define MAX_THREAD_NAME_LENGTH 30
	wchar_t wchars[MAX_THREAD_NAME_LENGTH];
#endif
*/
	FUNC_ENTRY;

#if defined(_WIN32)
/* Using NTDDI_VERSION rather than WINVER for more detailed version targeting */
/* Can't get this conditional compilation to work so remove it for now */
/*#if NTDDI_VERSION >= NTDDI_WIN10_RS1
	mbstowcs(wchars, thread_name, MAX_THREAD_NAME_LENGTH);
	rc = (int)SetThreadDescription(GetCurrentThread(), wchars);
#endif*/
#elif defined(OSX)
	/* pthread_setname_np __API_AVAILABLE(macos(10.6), ios(3.2)) */
#if defined(__APPLE__) && __MAC_OS_X_VERSION_MIN_REQUIRED >= MAC_OS_X_VERSION_10_6
	rc = pthread_setname_np(thread_name);
#endif
#else
#if defined(__GNUC__) && defined(__linux__)
#if __GLIBC__ >= 2 && __GLIBC_MINOR__ >= 12
	rc = pthread_setname_np(Paho_thread_getid(), thread_name);
#endif
#endif
#endif
	FUNC_EXIT_RC(rc);
	return rc;
}

/**
 * Get the thread id of the thread from which this function is called
 * @return thread id, type varying according to OS
 */
thread_id_type Paho_thread_getid(void)
{
	#if defined(_WIN32)
		return GetCurrentThreadId();
	#else
		return pthread_self();
	#endif
}

#if !defined(_WIN32)
struct timespec Thread_time_from_now(int ms)
{
	struct timespec from_now;
	struct timespec interval;
	interval.tv_sec = ms / 1000;
	interval.tv_nsec = (ms % 1000) * 1000000L;

    /*
     * Note that MacOS v10.11 was fully deprecated in Aug 2018.
     * So this __APPLE__ path only applies to very old, deprecated systems.
    */
#if defined(__APPLE__) && __MAC_OS_X_VERSION_MIN_REQUIRED < 101200
	struct timeval cur_time;
	gettimeofday(&cur_time, NULL);
	from_now.tv_sec = cur_time.tv_sec;
	from_now.tv_nsec = cur_time.tv_usec * 1000;
#else
	clock_gettime(CLOCK_REALTIME, &from_now);
#endif

	from_now.tv_sec += interval.tv_sec;
	from_now.tv_nsec += interval.tv_sec;

	while (from_now.tv_nsec >= NSEC_PER_SEC)
	{
		from_now.tv_sec++;
		from_now.tv_nsec -= NSEC_PER_SEC;
	}

	return from_now;
}
#endif

/* Mutex Functions */

/**
 * Create a new mutex
 * @param rc return code: 0 for success, negative otherwise
 * @return the new mutex
 */
mutex_type Paho_thread_create_mutex(int* rc)
{
	mutex_type mutex = NULL;

	FUNC_ENTRY;
	*rc = -1;
	#if defined(_WIN32)
		mutex = CreateMutex(NULL, 0, NULL);
		*rc = (mutex == NULL) ? GetLastError() : 0;
	#else
		mutex = malloc(sizeof(pthread_mutex_t));
		if (mutex)
			*rc = pthread_mutex_init(mutex, NULL);
	#endif
	FUNC_EXIT_RC(*rc);
	return mutex;
}


/**
 * Lock a mutex which has alrea
 * @return completion code, 0 is success
 */
int Paho_thread_lock_mutex(mutex_type mutex)
{
	int rc = -1;

	/* don't add entry/exit trace points as the stack log uses mutexes - recursion beckons */
	#if defined(_WIN32)
		/* WaitForSingleObject returns WAIT_OBJECT_0 (0), on success */
		rc = WaitForSingleObject(mutex, INFINITE);
	#else
		rc = pthread_mutex_lock(mutex);
	#endif

	return rc;
}


/**
 * Unlock a mutex which has already been locked
 * @param mutex the mutex
 * @return completion code, 0 is success
 */
int Paho_thread_unlock_mutex(mutex_type mutex)
{
	int rc = -1;

	/* don't add entry/exit trace points as the stack log uses mutexes - recursion beckons */
	#if defined(_WIN32)
		/* if ReleaseMutex fails, the return value is 0 */
		if (ReleaseMutex(mutex) == 0)
			rc = GetLastError();
		else
			rc = 0;
	#else
		rc = pthread_mutex_unlock(mutex);
	#endif

	return rc;
}


/**
 * Destroy a mutex which has already been created
 * @param mutex the mutex
 */
int Paho_thread_destroy_mutex(mutex_type mutex)
{
	int rc = 0;

	FUNC_ENTRY;
	#if defined(_WIN32)
		rc = CloseHandle(mutex);
	#else
		rc = pthread_mutex_destroy(mutex);
		free(mutex);
	#endif
	FUNC_EXIT_RC(rc);
	return rc;
}


/* Event functions */

/**
 * Create a new event
 * @return the event struct
 */
evt_type Thread_create_evt(int *rc)
{
	evt_type evt = NULL;
#if !defined(_WIN32)
	pthread_condattr_t attr;
#endif

	FUNC_ENTRY;
	*rc = -1;
#if defined(_WIN32)
    evt = CreateEvent(
        NULL,     /* default security attributes */
        FALSE,    /* manual-reset event? */
        FALSE,    /* initial state is nonsignaled */
        NULL      /* object name */
    );
    if (rc)
        *rc = (evt == NULL) ? GetLastError() : 0;
#else
    pthread_condattr_init(&attr);

	evt = malloc(sizeof(evt_type_struct));
	if (evt)
	{
		*rc = pthread_cond_init(&evt->cond, &attr);
		*rc = pthread_mutex_init(&evt->mutex, NULL);
		evt->val = 0;
	}
#endif
	FUNC_EXIT_RC(*rc);
	return evt;
}

/**
 * Signal an event
 * @return completion code, 0 is success
 */
int Thread_signal_evt(evt_type evt)
{
	int rc = 0;

    FUNC_ENTRY;
#if defined(_WIN32)
	if (SetEvent(evt) == 0)
		rc = GetLastError();
#else
	pthread_mutex_lock(&evt->mutex);
    evt->val = 1;
	// TODO: Determine if this could be more efficient with pthread_cond_signal()
	rc = pthread_cond_broadcast(&evt->cond);
	pthread_mutex_unlock(&evt->mutex);
#endif
 	FUNC_EXIT_RC(rc);
 	return rc;
}

/**
 * Wait with a timeout (ms) for the event to become signaled.
 * @return The completion code: 0 for success, ETIMEDOUT otherwise
 */
int Thread_wait_evt(evt_type evt, int timeout_ms)
{
	int rc = 0;

	FUNC_ENTRY;
#if defined(_WIN32)
	/* returns 0 (WAIT_OBJECT_0) on success, non-zero (WAIT_TIMEOUT) if timeout occurred */
	rc = WaitForSingleObject(evt, timeout_ms < 0 ? 0 : timeout_ms);
	if (rc == WAIT_TIMEOUT)
		rc = ETIMEDOUT;
#else
	struct timespec evt_timeout;
	evt_timeout = Thread_time_from_now(timeout_ms);

	pthread_mutex_lock(&evt->mutex);
	while (evt->val == 0 &&
		   (rc = pthread_cond_timedwait(&evt->cond, &evt->mutex, &evt_timeout)) == 0)
		;
	if (rc == 0) {
		evt->val = 0;
	}
	pthread_mutex_unlock(&evt->mutex);
#endif
	FUNC_EXIT_RC(rc);
	return rc;
}

/**
 * Destroy an event object
 * @return completion code, 0 for success
 */
int Thread_destroy_evt(evt_type evt)
{
	int rc = 0;

    FUNC_ENTRY;
#if defined(_WIN32)
	rc = CloseHandle(evt);
#else
	rc = pthread_mutex_destroy(&evt->mutex);
	int rcc = pthread_cond_destroy(&evt->cond);
    if (rcc != 0)
        rc = rcc;
	free(evt);
#endif
	FUNC_EXIT_RC(rc);
	return rc;
}


#if defined(THREAD_UNIT_TESTS)

#if defined(_WIN32) || defined(_WINDOWS)
#define mqsleep(A) Sleep(1000*A)
#define START_TIME_TYPE DWORD
static DWORD start_time = 0;
START_TIME_TYPE start_clock(void)
{
	return GetTickCount();
}
#elif defined(AIX)
#define mqsleep sleep
#define START_TIME_TYPE struct timespec
START_TIME_TYPE start_clock(void)
{
	static struct timespec start;
	clock_gettime(CLOCK_REALTIME, &start);
	return start;
}
#else
#define mqsleep sleep
#define START_TIME_TYPE struct timeval
/* TODO - unused - remove? static struct timeval start_time; */
START_TIME_TYPE start_clock(void)
{
	struct timeval start_time;
	gettimeofday(&start_time, NULL);
	return start_time;
}
#endif


#if defined(_WIN32)
long elapsed(START_TIME_TYPE start_time)
{
	return GetTickCount() - start_time;
}
#elif defined(AIX)
#define assert(a)
long elapsed(struct timespec start)
{
	struct timespec now, res;

	clock_gettime(CLOCK_REALTIME, &now);
	ntimersub(now, start, res);
	return (res.tv_sec)*1000L + (res.tv_nsec)/1000000L;
}
#else
long elapsed(START_TIME_TYPE start_time)
{
	struct timeval now, res;

	gettimeofday(&now, NULL);
	timersub(&now, &start_time, &res);
	return (res.tv_sec)*1000 + (res.tv_usec)/1000;
}
#endif


int tests = 0, failures = 0;

void myassert(char* filename, int lineno, char* description, int value, char* format, ...)
{
	++tests;
	if (!value)
	{
		va_list args;

		++failures;
		printf("Assertion failed, file %s, line %d, description: %s\n", filename, lineno, description);

		va_start(args, format);
		vprintf(format, args);
		va_end(args);

		//cur_output += sprintf(cur_output, "<failure type=\"%s\">file %s, line %d </failure>\n",
        //                description, filename, lineno);
	}
    else
    		printf("Assertion succeeded, file %s, line %d, description: %s\n", filename, lineno, description);
}

#define assert(a, b, c, d) myassert(__FILE__, __LINE__, a, b, c, d)
#define assert1(a, b, c, d, e) myassert(__FILE__, __LINE__, a, b, c, d, e)

#include <stdio.h>

thread_return_type cond_secondary(void* n)
{
	int rc = 0;
	evt_type evt = n;

	printf("This should return immediately as it was posted already\n");
	rc = Thread_wait_evt(evt, 99999);
	assert("rc 1 from wait_evt", rc == 1, "rc was %d", rc);

	printf("This should hang around a few seconds\n");
	rc = Thread_wait_evt(evt, 99999);
	assert("rc 1 from wait_evt", rc == 1, "rc was %d", rc);

	printf("Secondary evt thread ending\n");
	return 0;
}


int evt_test()
{
	int rc = 0;
	evt_type evt = Thread_create_evt();

	printf("Post secondary so it should return immediately\n");
	rc = Thread_signal_evt(evt);
	assert("rc 0 from signal evt", rc == 0, "rc was %d", rc);

	printf("Starting secondary thread\n");
	Thread_start(cond_secondary, (void*)cond);

	sleep(3);

	printf("post secondary\n");
	rc = Thread_signal_evt(evt);
	assert("rc 1 from signal evt", rc == 1, "rc was %d", rc);

	sleep(3);

	printf("Main thread ending\n");

	return failures;
}

int main(int argc, char *argv[])
{
	sem_test();
	//cond_test();
}

#endif
