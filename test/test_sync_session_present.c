/*******************************************************************************
 * Copyright (c) 2009, 2024 IBM Corp. and others
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
 *    Ian Craggs - Test program utilities
 *    Milan Tucic - Session present test1
 *******************************************************************************/


/**
 * @file
 * Tests for the MQ Telemetry MQTT C client
 */

#include "MQTTClient.h"
#include <string.h>
#include <stdlib.h>

#if !defined(_WINDOWS)
  #include <sys/time.h>
  #include <sys/socket.h>
  #include <unistd.h>
  #include <errno.h>
#else
  #include <windows.h>
  #define setenv(a, b, c) _putenv_s(a, b)
#endif

#define ARRAY_SIZE(a) (sizeof(a) / sizeof(a[0]))

void usage(void)
{
    printf("help!!\n");
    exit(EXIT_FAILURE);
}

struct Options
{
    char* connection;         /**< connection to system under test. */
    char** haconnections;
    char* proxy_connection;
    char* client_id;
    char* username;
    char* password;
    char* log;
    int hacount;
    int verbose;
    int test_no;
    int MQTTVersion;
    int iterations;
    int reconnect_period;
} options =
{
    "tcp://localhost:1883",
    NULL,
    "tcp://localhost:1884",
    "cli/test",
    NULL,
    NULL,
    NULL,
    0,
    0,
    0,
    MQTTVERSION_DEFAULT,
    1,
    3
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
        else if (strcmp(argv[count], "--connection") == 0)
        {
            if (++count < argc)
            {
                options.connection = argv[count];
                printf("\nSetting connection to %s\n", options.connection);
            }
            else
                usage();
        }
        else if (strcmp(argv[count], "--haconnections") == 0)
        {
            if (++count < argc)
            {
                char* tok = strtok(argv[count], " ");
                options.hacount = 0;
                options.haconnections = malloc(sizeof(char*) * 5);
                while (tok)
                {
                    options.haconnections[options.hacount] = malloc(strlen(tok) + 1);
                    strcpy(options.haconnections[options.hacount], tok);
                    options.hacount++;
                    tok = strtok(NULL, " ");
                }
            }
            else
                usage();
        }
        else if (strcmp(argv[count], "--proxy_connection") == 0)
        {
            if (++count < argc)
                options.proxy_connection = argv[count];
            else
                usage();
        }
        else if (strcmp(argv[count], "--client_id") == 0)
        {
            if (++count < argc)
                options.client_id = argv[count];
            else
                usage();
        }
        else if (strcmp(argv[count], "--username") == 0)
        {
            if (++count < argc)
                options.username = argv[count];
            else
                usage();
        }
        else if (strcmp(argv[count], "--password") == 0)
        {
            if (++count < argc)
                options.password = argv[count];
            else
                usage();
        }
        else if (strcmp(argv[count], "--log") == 0)
        {
            if (++count < argc)
                options.log = argv[count];
            else
                usage();
        }
        else if (strcmp(argv[count], "--MQTTversion") == 0)
        {
            if (++count < argc)
            {
                options.MQTTVersion = atoi(argv[count]);
                printf("setting MQTT version to %d\n", options.MQTTVersion);
            }
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
        else if (strcmp(argv[count], "--reconnection_period") == 0)
        {
            if (++count < argc)
                options.reconnect_period = atoi(argv[count]);
            else
                usage();
        }
        else if (strcmp(argv[count], "--verbose") == 0)
        {
            options.verbose = 1;
            printf("\nSetting verbose on\n");
        }
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
	struct tm timeinfo;

	if (LOGA_level == LOGA_DEBUG && options.verbose == 0)
	  return;

#if defined(_WIN32) || defined(_WINDOWS)
	ftime(&ts);
	localtime_s(&timeinfo, &ts.time);
#else
	gettimeofday(&ts, NULL);
	localtime_r(&ts.tv_sec, &timeinfo);
#endif
	strftime(msg_buf, 80, "%Y%m%d %H%M%S", &timeinfo);

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
        MyLog(LOGA_INFO, "Assertion failed, file %s, line %d, description: %s\n", filename, lineno, description);

        va_start(args, format);
        vprintf(format, args);
        va_end(args);

        cur_output += sprintf(cur_output, "<failure type=\"%s\">file %s, line %d </failure>\n",
                        description, filename, lineno);
    }
    else
        MyLog(LOGA_DEBUG, "Assertion succeeded, file %s, line %d, description: %s", filename, lineno, description);
}

void mysleep(int seconds)
{
#if defined(_WIN32)
    Sleep(1000L*seconds);
#else
    sleep(seconds);
#endif
}

/*********************************************************************

Test 1: clean session and reconnect with session present

*********************************************************************/
MQTTClient test1_c1;

void test1_connectionLost(void* context, char* cause)
{
    printf("Callback: connection lost\n");
}

void test1_deliveryComplete(void* context, MQTTClient_deliveryToken token)
{
    printf("Callback: publish complete for token %d\n", token);
}

int test1_messageArrived(void* context, char* topicName, int topicLen, MQTTClient_message* m)
{
    MQTTClient c = (MQTTClient)context;
    printf("Callback: message received on topic '%s' is '%.*s'.\n", topicName, m->payloadlen, (char*)(m->payload));
    MQTTClient_free(topicName);
    MQTTClient_freeMessage(&m);
    return 1;
}

int test1(struct Options options)
{
    char* testname = "test1";
    MQTTClient_connectOptions opts = MQTTClient_connectOptions_initializer;
    int rc;

    failures = 0;
    MyLog(LOGA_INFO, "Starting test 1 - clean session and reconnect with session present");
    fprintf(xml, "<testcase classname=\"test1\" name=\"connectionLost and will messages\"");
    global_start_time = start_clock();

    opts.keepAliveInterval = 60;
    opts.cleansession = 1;
    opts.MQTTVersion = MQTTVERSION_3_1_1;
    if (options.haconnections != NULL)
    {
        opts.serverURIs = options.haconnections;
        opts.serverURIcount = options.hacount;
    }
    if (options.username)
    {
        opts.username = options.username;
    }
    if (options.password)
    {
        opts.password = options.password;
    }

    rc = MQTTClient_create(&test1_c1, options.connection, options.client_id, MQTTCLIENT_PERSISTENCE_DEFAULT, NULL);
    assert("good rc from create", rc == MQTTCLIENT_SUCCESS, "rc was %d\n", rc);
    if (rc != MQTTCLIENT_SUCCESS)
        goto exit;

    rc = MQTTClient_setCallbacks(test1_c1, (void*)test1_c1, test1_connectionLost, test1_messageArrived, test1_deliveryComplete);
    assert("good rc from setCallbacks",  rc == MQTTCLIENT_SUCCESS, "rc was %d\n", rc);
    if (rc != MQTTCLIENT_SUCCESS)
        goto exit;

    /* Connect to the broker with clean session = true */
    rc = MQTTClient_connect(test1_c1, &opts);
    assert("good rc from connect with clean session = true",  rc == MQTTCLIENT_SUCCESS, "rc was %d\n", rc);
    if (rc != MQTTCLIENT_SUCCESS)
        goto exit;

    assert("connected, session cleaned", opts.returned.sessionPresent == 0,
                     "opts.returned.sessionPresent = %d\n", opts.returned.sessionPresent);

    /* Disconnect */
    rc = MQTTClient_disconnect(test1_c1, 1000);
    assert("good rc from disconnect",  rc == MQTTCLIENT_SUCCESS, "rc was %d\n", rc);
    if (rc != MQTTCLIENT_SUCCESS)
        goto exit;

    MyLog(LOGA_INFO, "Sleeping after session cleaned %d s ...", options.reconnect_period);
    mysleep(options.reconnect_period);

    /* Connect to the broker with clean session = false */
    opts.cleansession = 0;
    rc = MQTTClient_connect(test1_c1, &opts);
    assert("good rc from connect with clean session = false",  rc == MQTTCLIENT_SUCCESS, "rc was %d\n", rc);
    if (rc != MQTTCLIENT_SUCCESS)
        goto exit;

    assert("connected, session clean", opts.returned.sessionPresent == 0,
                     "opts.returned.sessionPresent = %d\n", opts.returned.sessionPresent);

    /* Disconnect */
    rc = MQTTClient_disconnect(test1_c1, 1000);
    assert("good rc from disconnect",  rc == MQTTCLIENT_SUCCESS, "rc was %d\n", rc);
    if (rc != MQTTCLIENT_SUCCESS)
        goto exit;

    MyLog(LOGA_INFO, "Sleeping after session persist %d s ...", options.reconnect_period);
    mysleep(options.reconnect_period);

    /* Connect to the broker with clean session = false, expected to have session */
    opts.cleansession = 0;
    rc = MQTTClient_connect(test1_c1, &opts);
    assert("good rc from second connect with clean session = false",  rc == MQTTCLIENT_SUCCESS, "rc was %d\n", rc);
    if (rc != MQTTCLIENT_SUCCESS)
        goto exit;

    assert("connected, session present", opts.returned.sessionPresent == 1,
                     "opts.returned.sessionPresent = %d\n", opts.returned.sessionPresent);

    MQTTClient_destroy(&test1_c1);

exit:
    MyLog(LOGA_INFO, "%s: test %s. %d tests run, %d failures.\n",
            (failures == 0) ? "passed" : "failed", testname, tests, failures);
    write_test_result();
    return failures;
}

int main(int argc, char** argv)
{
    int rc = 0;
    int (*tests[])(struct Options) = {NULL, test1};
    int i;
    unsigned test_i;

    xml = fopen("TEST-test1.xml", "w");
    fprintf(xml, "<testsuite name=\"test1\" tests=\"%d\">\n", (int)(ARRAY_SIZE(tests) - 1));

    getopts(argc, argv);

    if (options.log)
    {
        setenv("MQTT_C_CLIENT_TRACE", "ON", 1);
        setenv("MQTT_C_CLIENT_TRACE_LEVEL", options.log, 1);
    }


    MyLog(LOGA_INFO, "Running %d iteration(s)", options.iterations);

    for (i = 0; i < options.iterations; ++i)
    {
        if (options.test_no == 0)
        { /* run all the tests */
            for (test_i = 1; test_i < ARRAY_SIZE(tests); ++test_i)
                rc += tests[test_i](options); /* return number of failures.  0 = test succeeded */
        }
        else
            rc = tests[options.test_no](options); /* run just the selected test */
    }

    if (rc == 0)
        MyLog(LOGA_INFO, "verdict pass");
    else
        MyLog(LOGA_INFO, "verdict fail");

    fprintf(xml, "</testsuite>\n");
    fclose(xml);
    return rc;
}
