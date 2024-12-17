/*******************************************************************************
 * Copyright (c) 2020, 2021 IBM Corp. and Ian Craggs
 *
 * All rights reserved. This program and the accompanying materials
 * are made available under the terms of the Eclipse Public License v2.0
 * and Eclipse Distribution License v1.0 which accompany this distribution.
 *
 * The Eclipse Public License is available at
 *    https://www.eclipse.org/legal/epl-v20.html
 * and the Eclipse Distribution License is available at
 *   http://www.eclipse.org/org/documents/edl-v10.php
 *
 * Contributors:
 *    Ian Craggs - initial implementation
 *******************************************************************************/

#include "MQTTTime.h"
#include "StackTrace.h"

#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
#else
#include <unistd.h>
#include <sys/time.h>
#endif

void MQTTTime_sleep(ELAPSED_TIME_TYPE milliseconds)
{
	FUNC_ENTRY;
#if defined(_WIN32) || defined(_WIN64)
	Sleep((DWORD)milliseconds);
#else
	usleep((useconds_t)(milliseconds*1000));
#endif
	FUNC_EXIT;
}

#if defined(_WIN32) || defined(_WIN64)
START_TIME_TYPE MQTTTime_start_clock(void)
{
#if WINVER >= _WIN32_WINNT_VISTA
	return GetTickCount64();
#else
	return GetTickCount();
#endif
}
#elif defined(AIX)
START_TIME_TYPE MQTTTime_start_clock(void)
{
	struct timespec start;
	clock_gettime(CLOCK_MONOTONIC, &start);
	return start;
}
#else
START_TIME_TYPE MQTTTime_start_clock(void)
{
	struct timeval start;
	struct timespec start_ts;

	clock_gettime(CLOCK_MONOTONIC, &start_ts);
	start.tv_sec = start_ts.tv_sec;
	start.tv_usec = start_ts.tv_nsec / 1000;
	return start;
}
#endif
START_TIME_TYPE MQTTTime_now(void)
{
	return MQTTTime_start_clock();
}


#if defined(_WIN32) || defined(_WIN64)
/*
 * @param t_new most recent time in milliseconds from GetTickCount()
 * @param t_old older time in milliseconds from GetTickCount()
 * @return difference in milliseconds
 */
DIFF_TIME_TYPE MQTTTime_difftime(START_TIME_TYPE t_new, START_TIME_TYPE t_old)
{
#if WINVER >= _WIN32_WINNT_VISTA
	return (DIFF_TIME_TYPE)(t_new - t_old);
#else
	if (t_old < t_new)       /* check for wrap around condition in GetTickCount */
		return (DIFF_TIME_TYPE)(t_new - t_old);
	else
	    return (DIFF_TIME_TYPE)((0xFFFFFFFFL - t_old) + 1 + t_new);
#endif
}
#elif defined(AIX)
#define assert(a)
DIFF_TIME_TYPE MQTTTime_difftime(START_TIME_TYPE t_new, START_TIME_TYPE t_old)
{
	struct timespec result;

	ntimersub(t_new, t_old, result);
	return (DIFF_TIME_TYPE)((result.tv_sec)*1000L + (result.tv_nsec)/1000000L); /* convert to milliseconds */
}
#else
DIFF_TIME_TYPE MQTTTime_difftime(START_TIME_TYPE t_new, START_TIME_TYPE t_old)
{
	struct timeval result;

	timersub(&t_new, &t_old, &result);
	return (DIFF_TIME_TYPE)(((DIFF_TIME_TYPE)result.tv_sec)*1000 + ((DIFF_TIME_TYPE)result.tv_usec)/1000); /* convert to milliseconds */
}
#endif


ELAPSED_TIME_TYPE MQTTTime_elapsed(START_TIME_TYPE milliseconds)
{
	return (ELAPSED_TIME_TYPE)MQTTTime_difftime(MQTTTime_now(), milliseconds);
}
