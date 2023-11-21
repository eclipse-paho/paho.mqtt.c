/*******************************************************************************
 * Copyright (c) 2009, 2023 IBM Corp.
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
 *    Ian Craggs - initial API and implementation and/or initial documentation
 *******************************************************************************/


/**
 * @file
 * Unit tests for threading
 */


#include "Thread.h"
#include <string.h>
#include <stdlib.h>

#if !defined(_WINDOWS)
	#include <sys/time.h>
  #include <sys/socket.h>
	#include <unistd.h>
  #include <errno.h>
  #define WINAPI
#else
	#include <windows.h>
#endif

#define ARRAY_SIZE(a) (sizeof(a) / sizeof(a[0]))

void usage(void)
{
	printf("help!!\n");
	exit(EXIT_FAILURE);
}

struct Options
{
	int verbose;
	int test_no;
	int iterations;
} options =
{
	0,
	-1,
	1,
};

void getopts(int argc, char** argv)
{
	int count = 1;

	while (count < argc)
	{
		if (strcmp(argv[count], "--test_no") == 0)
		{
			if (++count < argc)
				options.test_no = atoi(argv[count]);
			else
				usage();
		}
		else if (strcmp(argv[count], "--iterations") == 0)
		{
			if (++count < argc)
				options.iterations = atoi(argv[count]);
			else
				usage();
		}
		else if (strcmp(argv[count], "--verbose") == 0)
			options.verbose = 1;
		count++;
	}
}

#define LOGA_DEBUG 0
#define LOGA_INFO 1
#include <stdarg.h>
#include <time.h>
#include <sys/timeb.h>
void MyLog(int LOGA_level, char* format, ...)
{
	static char msg_buf[256];
	va_list args;
#if defined(_WIN32) || defined(_WINDOWS)
	struct timeb ts;
#else
	struct timeval ts;
#endif
	struct tm *timeinfo;

	if (LOGA_level == LOGA_DEBUG && options.verbose == 0)
	  return;

#if defined(_WIN32) || defined(_WINDOWS)
	ftime(&ts);
	timeinfo = localtime(&ts.time);
#else
	gettimeofday(&ts, NULL);
	timeinfo = localtime(&ts.tv_sec);
#endif
	strftime(msg_buf, 80, "%Y%m%d %H%M%S", timeinfo);

#if defined(_WIN32) || defined(_WINDOWS)
	sprintf(&msg_buf[strlen(msg_buf)], ".%.3hu ", ts.millitm);
#else
	sprintf(&msg_buf[strlen(msg_buf)], ".%.3lu ", ts.tv_usec / 1000L);
#endif

	va_start(args, format);
	vsnprintf(&msg_buf[strlen(msg_buf)], sizeof(msg_buf) - strlen(msg_buf), format, args);
	va_end(args);

	printf("%s\n", msg_buf);
	fflush(stdout);
}


#if defined(_WIN32) || defined(_WINDOWS)
#define mysleep(A) Sleep(1000*A)
#define START_TIME_TYPE DWORD
static DWORD start_time = 0;
START_TIME_TYPE start_clock(void)
{
	return GetTickCount();
}
#elif defined(AIX)
#define mysleep sleep
#define START_TIME_TYPE struct timespec
START_TIME_TYPE start_clock(void)
{
	static struct timespec start;
	clock_gettime(CLOCK_REALTIME, &start);
	return start;
}
#else
#define mysleep sleep
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

#define assert(a, b, c, d) myassert(__FILE__, __LINE__, a, b, c, d)
#define assert1(a, b, c, d, e) myassert(__FILE__, __LINE__, a, b, c, d, e)

int tests = 0;
int failures = 0;
FILE* xml;
START_TIME_TYPE global_start_time;
char output[3000];
char* cur_output = output;

void write_test_result(void)
{
	long duration = elapsed(global_start_time);

	fprintf(xml, " time=\"%ld.%.3ld\" >\n", duration / 1000, duration % 1000);
	if (cur_output != output)
	{
		fprintf(xml, "%s", output);
		cur_output = output;
	}
	fprintf(xml, "</testcase>\n");
}

void myassert(char* filename, int lineno, char* description, int value, char* format, ...)
{
	++tests;
	if (!value)
	{
		va_list args;

		++failures;
		printf("Assertion failed, file %s, line %d, description: %s, ", filename, lineno, description);

		va_start(args, format);
		vprintf(format, args);
		va_end(args);

		printf("\n");

		cur_output += sprintf(cur_output, "<failure type=\"%s\">file %s, line %d </failure>\n",
                        description, filename, lineno);
	}
    else
    	MyLog(LOGA_DEBUG, "Assertion succeeded, file %s, line %d, description: %s", filename, lineno, description);
}

static thread_return_type WINAPI sem_secondary(void* n)
{
	int rc = 0;
	sem_type sem = n;
	START_TIME_TYPE start;
	long duration;

	MyLog(LOGA_DEBUG, "Secondary semaphore pointer %p", sem);

	rc = Thread_check_sem(sem);
	assert("rc 0 from check_sem", rc == 0, "rc was %d", rc);

	MyLog(LOGA_DEBUG, "Secondary thread about to wait");
	start = start_clock();
	rc = Thread_wait_sem(sem, 99999);
	duration = elapsed(start);
	assert("rc 0 from lock mutex", rc == 0, "rc was %d", rc);
	MyLog(LOGA_INFO, "Lock duration was %ld", duration);
	assert("duration is 2s", duration >= 2000L, "duration was %ld", duration);

	MyLog(LOGA_DEBUG, "Secondary thread ending");
	return 0;
}


int test_sem(struct Options options)
{
	char* testname = "test_sem";
	int rc = 0, i = 0;
	START_TIME_TYPE start;
	long duration;
	sem_type sem = Thread_create_sem(&rc);

	MyLog(LOGA_INFO, "Starting semaphore test");
	fprintf(xml, "<testcase classname=\"test\" name=\"%s\"", testname);
	global_start_time = start_clock();

	MyLog(LOGA_DEBUG, "Primary semaphore pointer %p\n", sem);

	/* The semaphore should be created non-signaled */
	rc = Thread_check_sem(sem);
	assert("rc 0 from check_sem", rc == 0, "rc was %d\n", rc);

	MyLog(LOGA_DEBUG, "Post semaphore so then check should be 1\n");
	rc = Thread_post_sem(sem);
	assert("rc 0 from post_sem", rc == 0, "rc was %d\n", rc);

	/* should be 1, and then reset to 0 */
	rc = Thread_check_sem(sem);
	assert("rc 1 from check_sem", rc == 1, "rc was %d", rc);

	/* so now it'll be 0 */
	rc = Thread_check_sem(sem);
	assert("rc 0 from check_sem", rc == 0, "rc was %d", rc);

	/* multiple posts */
	for (i = 0; i < 10; ++i)
	{
		rc = Thread_post_sem(sem);
		assert("rc 0 from post_sem", rc == 0, "rc was %d\n", rc);
	}

	for (i = 0; i < 10; ++i)
	{
		rc = Thread_check_sem(sem);
		assert("rc 1 from check_sem", rc == 1, "rc was %d", rc);
	}
	rc = Thread_check_sem(sem);
	assert("rc 0 from check_sem", rc == 0, "rc was %d", rc);

	MyLog(LOGA_DEBUG, "Check timeout");
	start = start_clock();
	rc = Thread_wait_sem(sem, 1500);
	duration = elapsed(start);
	assert("rc ETIMEDOUT from lock mutex", rc == ETIMEDOUT, "rc was %d", rc);
	MyLog(LOGA_INFO, "Lock duration was %ld", duration);
	assert("duration is 2s", duration >= 1500L, "duration was %ld", duration);

	MyLog(LOGA_DEBUG, "Starting secondary thread");
	Paho_thread_start(sem_secondary, (void*)sem);

	mysleep(2);
	MyLog(LOGA_DEBUG, "post secondary");
	rc = Thread_post_sem(sem);
	assert("rc 1 from post_sem", rc == 1, "rc was %d", rc);

	mysleep(1);

	MyLog(LOGA_DEBUG, "Main thread ending");

	/*exit: */ MyLog(LOGA_INFO, "%s: test %s. %d tests run, %d failures.",
			(failures == 0) ? "passed" : "failed", testname, tests, failures);
	write_test_result();

	return failures;
}

#if !defined(_WIN32) && !defined(_WIN64)
thread_return_type cond_secondary(void* n)
{
	int rc = 0;
	cond_type cond = n;
	START_TIME_TYPE start;
	long duration;

	MyLog(LOGA_DEBUG, "This will time out");
	start = start_clock();
	rc = Thread_wait_cond(cond, 1);
	duration = elapsed(start);
	MyLog(LOGA_INFO, "Lock duration was %ld", duration);
	assert("duration is about 1s", duration >= 1000L && duration <= 1050L, "duration was %ld", duration);
	assert("rc non 0 from wait_cond", rc == ETIMEDOUT, "rc was %d", rc);

	MyLog(LOGA_DEBUG, "This should hang around a few seconds");
	start = start_clock();
	rc = Thread_wait_cond(cond, 99999);
	duration = elapsed(start);
	MyLog(LOGA_INFO, "Lock duration was %ld", duration);
	assert("duration is around 1s", duration >= 990L && duration <= 1010L, "duration was %ld", duration);
	assert("rc 9 from wait_cond", rc == 0, "rc was %d", rc);

	MyLog(LOGA_DEBUG, "Secondary cond thread ending");
	return 0;
}


int test_cond(struct Options options)
{
	char* testname = "test_cond";
	int rc = 0, i = 0;
	START_TIME_TYPE start;
	long duration;
	cond_type cond = Thread_create_cond(&rc);

	MyLog(LOGA_INFO, "Starting condition variable test");
	fprintf(xml, "<testcase classname=\"cond\" name=\"%s\"", testname);
	global_start_time = start_clock();

	/* The semaphore should be created non-signaled */
	rc = Thread_wait_cond(cond, 0);
	assert("rc 0 from wait_cond", rc == ETIMEDOUT, "rc was %d", rc);

	MyLog(LOGA_DEBUG, "Check timeout");
	start = start_clock();
	rc = Thread_wait_cond(cond, 2);
	duration = elapsed(start);
	assert("rc ETIMEDOUT from lock mutex", rc == ETIMEDOUT, "rc was %d", rc);
	MyLog(LOGA_INFO, "Lock duration was %ld", duration);
	assert("duration is 2s", duration >= 2000L, "duration was %ld", duration);

	/* multiple posts */
	for (i = 0; i < 10; ++i)
	{
		rc = Thread_signal_cond(cond);
		assert("rc 0 from signal cond", rc == 0, "rc was %d\n", rc);
	}

	/* the signals are not stored */
	for (i = 0; i < 10; ++i)
	{
		rc = Thread_wait_cond(cond, 0);
		assert("rc non-zero from wait_cond", rc == ETIMEDOUT, "rc was %d", rc);
	}
	rc = Thread_wait_cond(cond, 0);
	assert("rc non-zero from wait_cond", rc == ETIMEDOUT, "rc was %d", rc);

	MyLog(LOGA_DEBUG, "Post secondary but it will time out");
	rc = Thread_signal_cond(cond);
	assert("rc 0 from signal cond", rc == 0, "rc was %d", rc);

	MyLog(LOGA_DEBUG, "Starting secondary thread");
	Paho_thread_start(cond_secondary, (void*)cond);

	MyLog(LOGA_DEBUG, "wait for secondary thread to enter second wait");
	mysleep(2);

	MyLog(LOGA_DEBUG, "post secondary");
	rc = Thread_signal_cond(cond);
	assert("rc 0 from signal cond", rc == 0, "rc was %d", rc);

	mysleep(1);

	MyLog(LOGA_DEBUG, "Main thread ending");

	exit: MyLog(LOGA_INFO, "%s: test %s. %d tests run, %d failures.",
			(failures == 0) ? "passed" : "failed", testname, tests, failures);
	write_test_result();

	return failures;
}
#endif


static thread_return_type WINAPI mutex_secondary(void* n)
{
	int rc = 0;
	mutex_type mutex = n;
	START_TIME_TYPE start;
	long duration;

	/* this should take 2s, as there is another lock held */
	start = start_clock();
	rc = Paho_thread_lock_mutex(mutex);
	duration = elapsed(start);
	assert("rc 0 from lock mutex", rc == 0, "rc was %d", rc);
	MyLog(LOGA_INFO, "Lock duration was %ld", duration);
	assert("duration is 2s", duration >= 1000L, "duration was %ld", duration);

	rc = Paho_thread_unlock_mutex(mutex);
	assert("rc 0 from unlock mutex", rc == 0, "rc was %d", rc);
	MyLog(LOGA_DEBUG, "Secondary thread ending");
	return 0;
}


int test_mutex(struct Options options)
{
	char* testname = "test_mutex";
	int rc = 0;
	mutex_type mutex = Paho_thread_create_mutex(&rc);
	START_TIME_TYPE start;
	long duration;

	MyLog(LOGA_INFO, "Starting mutex test");
	fprintf(xml, "<testcase classname=\"test\" name=\"%s\"", testname);
	global_start_time = start_clock();

	/* this should happen immediately, as there is no other lock held */
	start = start_clock();
	rc = Paho_thread_lock_mutex(mutex);
	duration = elapsed(start);
	assert("rc 0 from lock mutex", rc == 0, "rc was %d", rc);
	MyLog(LOGA_INFO, "Lock duration was %ld", duration);
	assert("duration is very low", duration < 5L, "duration was %ld", duration);

	MyLog(LOGA_DEBUG, "Starting secondary thread");
	Paho_thread_start(mutex_secondary, (void*)mutex);

	mysleep(2);
	rc = Paho_thread_unlock_mutex(mutex); /* let background thread have it */
	assert("rc 0 from unlock mutex", rc == 0, "rc was %d", rc);

	start = start_clock();
	rc = Paho_thread_lock_mutex(mutex); /* make sure background thread hasn't locked it */
	duration = elapsed(start);
	assert("rc 0 from lock mutex", rc == 0, "rc was %d", rc);
	MyLog(LOGA_INFO, "Lock duration was %ld", duration);
	assert("duration is very low", duration < 5L, "duration was %ld", duration);

	Paho_thread_destroy_mutex(mutex);

	MyLog(LOGA_DEBUG, "Main thread ending");

	/*exit:*/ MyLog(LOGA_INFO, "%s: test %s. %d tests run, %d failures.",
			(failures == 0) ? "passed" : "failed", testname, tests, failures);
	write_test_result();

	return failures;
}



int main(int argc, char** argv)
{
	int rc = -1;
 	int (*tests[])() = {NULL,
 		test_mutex,
 		test_sem,
#if !defined(_WIN32) && !defined(_WIN64)
		test_cond
#endif
 	}; /* indexed starting from 1 */
	int i;

	xml = fopen("TEST-thread.xml", "w");
	fprintf(xml, "<testsuite name=\"thread\" tests=\"%d\">\n", (int)(ARRAY_SIZE(tests)) - 1);

	getopts(argc, argv);

	for (i = 0; i < options.iterations; ++i)
	{
	 	if (options.test_no == -1)
		{ /* run all the tests */
			for (options.test_no = 1; options.test_no < ARRAY_SIZE(tests); ++options.test_no)
			{
				failures = rc = 0;
				rc += tests[options.test_no](options); /* return number of failures.  0 = test succeeded */
			}
		}
		else
		{
			if (options.test_no >= ARRAY_SIZE(tests))
				MyLog(LOGA_INFO, "No test number %d", options.test_no);
			else
			{
				rc = tests[options.test_no](options); /* run just the selected test */
			}
		}
	}

	if (rc == 0)
		MyLog(LOGA_INFO, "verdict pass");
	else
		MyLog(LOGA_INFO, "verdict fail");

	fprintf(xml, "</testsuite>\n");
	fclose(xml);

	return rc;
}
