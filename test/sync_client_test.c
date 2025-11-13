/*******************************************************************
  Copyright (c) 2013, 2020 IBM Corp.
 
  All rights reserved. This program and the accompanying materials
  are made available under the terms of the Eclipse Public License v2.0
  and Eclipse Distribution License v1.0 which accompany this distribution. 
 
  The Eclipse Public License is available at 
     https://www.eclipse.org/legal/epl-2.0/
  and the Eclipse Distribution License is available at 
    http://www.eclipse.org/org/documents/edl-v10.php.
 
  Contributors:
     Ian Craggs - initial implementation and/or documentation
*******************************************************************/

#include "MQTTClient.h"
#include <string.h>
#include <stdlib.h>

#if !defined(_WINDOWS)
	#include <sys/time.h>
  	#include <sys/socket.h>
	#include <unistd.h>
  	#include <errno.h>
#else
#include <winsock2.h>
#include <ws2tcpip.h>
#define MAXHOSTNAMELEN 256
#define EAGAIN WSAEWOULDBLOCK
#define EINTR WSAEINTR
#define EINPROGRESS WSAEINPROGRESS
#define EWOULDBLOCK WSAEWOULDBLOCK
#define ENOTCONN WSAENOTCONN
#define ECONNRESET WSAECONNRESET
#endif

#define ARRAY_SIZE(a) (sizeof(a) / sizeof(a[0]))


char* topics[] =  {"TopicA", "TopicA/B", "Topic/C", "TopicA/C", "/TopicA"};
char* wildtopics[] = {"TopicA/+", "+/C", "#", "/#", "/+", "+/+", "TopicA/#"};
char* nosubscribe_topics[] = {"nosubscribe",};

struct Options
{
	char* connection;         /**< connection to system under test. */
	char* clientid1;
	char* clientid2;
	char* username;
	char* password;
	int verbose;
	int MQTTVersion;
	int iterations;
	int run_dollar_topics_test;
	int run_subscribe_failure_test;
} options =
{
	"tcp://localhost:1883",
	"myclientid",
	"myclientid2",
	NULL,
	NULL,
	0,
	MQTTVERSION_3_1_1,
	1,
	0,
	0,
};


void usage(void)
{
	printf("options:\n  connection, clientid1, clientid2, username, password, MQTTversion, iterations, verbose\n");
	exit(EXIT_FAILURE);
}

void getopts(int argc, char** argv)
{
	int count = 1;
	
	while (count < argc)
	{
		if (strcmp(argv[count], "--dollar_topics_test") == 0 || strcmp(argv[count], "--$") == 0)
		{
			options.run_dollar_topics_test = 1;
			printf("Running $ topics test\n");
		}
		else if (strcmp(argv[count], "--subscribe_failure_test") == 0 || strcmp(argv[count], "-s") == 0)
		{
			options.run_subscribe_failure_test = 1;
			printf("Running subscribe failure test\n");
		}
		else if (strcmp(argv[count], "--connection") == 0)
		{
			if (++count < argc)
			{
				options.connection = argv[count];
				printf("Setting connection to %s\n", options.connection);
			}
			else
				usage();
		}
		else if (strcmp(argv[count], "--clientid1") == 0)
		{
			if (++count < argc)
			{
				options.clientid1 = argv[count];
				printf("Setting clientid1 to %s\n", options.clientid1);
			}
			else
				usage();
		}
		else if (strcmp(argv[count], "--clientid2") == 0)
		{
			if (++count < argc)
			{
				options.clientid2 = argv[count];
				printf("Setting clientid2 to %s\n", options.clientid2);
			}
			else
				usage();
		}
		else if (strcmp(argv[count], "--username") == 0)
		{
			if (++count < argc)
			{
				options.username = argv[count];
				printf("Setting username to %s\n", options.username);
			}
			else
				usage();
		}
		else if (strcmp(argv[count], "--password") == 0)
		{
			if (++count < argc)
			{
				options.password = argv[count];
				printf("Setting password to %s\n", options.password);
			}
			else
				usage();
		}
		else if (strcmp(argv[count], "--MQTTversion") == 0)
		{
			if (++count < argc)
			{
				options.MQTTVersion = atoi(argv[count]);
				printf("Setting MQTT version to %d\n", options.MQTTVersion);
			}
			else
				usage();
		}
		else if (strcmp(argv[count], "--iterations") == 0)
		{
			if (++count < argc)
			{
				options.iterations = atoi(argv[count]);
				printf("Setting iterations to %d\n", options.iterations);
			}
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


#if defined(_WIN32) || defined(_WINDOWS)
#define msleep Sleep
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
#define msleep(A) usleep(A*1000)
#define START_TIME_TYPE struct timeval
/* TODO - unused - remove? static struct timeval start_time; */
START_TIME_TYPE start_clock(void)
{
	struct timeval start_time;
	gettimeofday(&start_time, NULL);
	return start_time;
}
#endif

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

int tests = 0;
int failures = 0;


void myassert(char* filename, int lineno, char* description, int value, char* format, ...)
{
	++tests;
	if (!value)
	{
		int count;
		va_list args;

		++failures;
		printf("Assertion failed, file %s, line %d, description: %s\n", filename, lineno, description);

		va_start(args, format);
		count = vprintf(format, args);
		va_end(args);
		if (count)
			printf("\n");

		//cur_output += sprintf(cur_output, "<failure type=\"%s\">file %s, line %d </failure>\n", 
        //               description, filename, lineno);
	}
    else
    	MyLog(LOGA_DEBUG, "Assertion succeeded, file %s, line %d, description: %s", filename, lineno, description);  
}


#define assert(a, b, c, d) myassert(__FILE__, __LINE__, a, b, c, d)
#define assert1(a, b, c, d, e) myassert(__FILE__, __LINE__, a, b, c, d, e)

typedef struct
{
	char* topicName;
	int topicLen;
	MQTTClient_message* m;
} messageStruct;

messageStruct messagesArrived[1000];
int messageCount = 0;

int messageArrived(void* context, char* topicName, int topicLen, MQTTClient_message* m)
{
	messagesArrived[messageCount].topicName = topicName;
	messagesArrived[messageCount].topicLen = topicLen;
	messagesArrived[messageCount++].m = m;
	MyLog(LOGA_DEBUG, "Callback: %d message received on topic %s is %.*s.",
					messageCount, topicName, m->payloadlen, (char*)(m->payload));
	return 1;
}


void clearMessages(void)
{
	int i;

	for (i = 0; i < messageCount; ++i)
	{
		MQTTClient_free(messagesArrived[i].topicName);
		MQTTClient_freeMessage(&messagesArrived[i].m);
	}
	messageCount = 0;
}

void cleanup(void)
{
	// clean all client state
	char* clientids[] = {options.clientid1, options.clientid2};
	int i, rc;
	MQTTClient_connectOptions opts = MQTTClient_connectOptions_initializer;
	MQTTClient aclient;

	MyLog(LOGA_INFO, "Cleaning up");

	opts.keepAliveInterval = 20;
	opts.cleansession = 1;
	opts.username = options.username;
	opts.password = options.password;
	opts.MQTTVersion = options.MQTTVersion;
	
	for (i = 0; i < 2; ++i)
	{
		rc = MQTTClient_create(&aclient, options.connection, clientids[i], MQTTCLIENT_PERSISTENCE_DEFAULT, NULL);
		assert("good rc from create",  rc == MQTTCLIENT_SUCCESS, "rc was %d\n", rc);

		rc = MQTTClient_connect(aclient, &opts);
		assert("Good rc from connect", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);

		rc = MQTTClient_disconnect(aclient, 100);
		assert("Disconnect successful", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);

		MQTTClient_destroy(&aclient);
	}

	// clean retained messages 
	rc = MQTTClient_create(&aclient, options.connection, options.clientid1, MQTTCLIENT_PERSISTENCE_DEFAULT, NULL);
	assert("good rc from create",  rc == MQTTCLIENT_SUCCESS, "rc was %d\n", rc);

	rc = MQTTClient_setCallbacks(aclient, NULL, NULL, messageArrived, NULL);
	assert("Good rc from setCallbacks", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);

	rc = MQTTClient_connect(aclient, &opts);
	assert("Good rc from connect", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);

	rc = MQTTClient_subscribe(aclient, "#", 0);
	assert("Good rc from subscribe", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);

	msleep(2000); // wait for all retained messages to arrive

	rc = MQTTClient_unsubscribe(aclient, "#");
	assert("Good rc from unsubscribe", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);

	for (i = 0; i < messageCount; ++i)
	{
		if (messagesArrived[i].m->retained)
		{
			MyLog(LOGA_INFO, "Deleting retained message for topic %s", (char*)messagesArrived[i].topicName);
			rc = MQTTClient_publish(aclient, messagesArrived[i].topicName, 0, "", 0, 1, NULL);
			assert("Good rc from publish", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);
		}
	}

	rc = MQTTClient_disconnect(aclient, 100);
	assert("Disconnect successful", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);

	MQTTClient_destroy(&aclient);

	clearMessages();

	MyLog(LOGA_INFO, "Finished cleaning up");
}

 
int basic_test(void)
{
	int i, rc;
	MQTTClient_connectOptions opts = MQTTClient_connectOptions_initializer;
	MQTTClient aclient;

	MyLog(LOGA_INFO, "Starting basic test");

	tests = failures = 0;

	opts.keepAliveInterval = 20;
	opts.cleansession = 1;
	opts.username = options.username;
	opts.password = options.password;
	opts.MQTTVersion = options.MQTTVersion;

	rc = MQTTClient_create(&aclient, options.connection, options.clientid1, MQTTCLIENT_PERSISTENCE_DEFAULT, NULL);
	assert("good rc from create",  rc == MQTTCLIENT_SUCCESS, "rc was %d\n", rc);

	rc = MQTTClient_setCallbacks(aclient, NULL, NULL, messageArrived, NULL);
	assert("Good rc from setCallbacks", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);

	rc = MQTTClient_connect(aclient, &opts);
	assert("Good rc from connect", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);

	rc = MQTTClient_disconnect(aclient, 100);
	assert("Disconnect successful", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);

	rc = MQTTClient_connect(aclient, &opts);
	assert("Good rc from connect", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);

	rc = MQTTClient_subscribe(aclient, topics[0], 0);
	assert("Good rc from subscribe", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);

	rc = MQTTClient_publish(aclient, topics[0], 5, "qos 0", 0, 0, NULL);
	assert("Good rc from publish", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);

	rc = MQTTClient_publish(aclient, topics[0], 5, "qos 1", 1, 0, NULL);
	assert("Good rc from publish", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);

	rc = MQTTClient_publish(aclient, topics[0], 5, "qos 2", 2, 0, NULL);
	assert("Good rc from publish", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);

	msleep(1000);

	rc = MQTTClient_disconnect(aclient, 10000);
	assert("Disconnect successful", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);

	assert("3 Messages received", messageCount == 3, "messageCount was %d", messageCount);
	clearMessages();

	/*opts.MQTTVersion = MQTTVERSION_3_1;
	rc = MQTTClient_connect(aclient, &opts); // should fail - wrong protocol version
	assert("Bad rc from connect", rc == MQTTCLIENT_FAILURE, "rc was %d", rc);*/
	
	MQTTClient_destroy(&aclient);

	MyLog(LOGA_INFO, "Basic test %s", (failures == 0) ?  "succeeded" : "failed");
	return failures;
}



int offline_message_queueing_test(void)
{
	int i, rc;
	MQTTClient_connectOptions opts = MQTTClient_connectOptions_initializer;
	MQTTClient aclient;
	MQTTClient bclient;

	MyLog(LOGA_INFO, "Offline message queueing test");

	tests = failures = 0;

	opts.keepAliveInterval = 20;
	opts.cleansession = 0;
	opts.username = options.username;
	opts.password = options.password;
	opts.MQTTVersion = options.MQTTVersion;

	rc = MQTTClient_create(&aclient, options.connection, options.clientid1, MQTTCLIENT_PERSISTENCE_DEFAULT, NULL);
	assert("good rc from create",  rc == MQTTCLIENT_SUCCESS, "rc was %d\n", rc);

	rc = MQTTClient_setCallbacks(aclient, NULL, NULL, messageArrived, NULL);
	assert("Good rc from setCallbacks", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);

	rc = MQTTClient_connect(aclient, &opts);
	assert("Good rc from connect", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);

	rc = MQTTClient_subscribe(aclient, wildtopics[5], 2);
	assert("Good rc from subscribe", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);

	rc = MQTTClient_disconnect(aclient, 100);
	assert("Disconnect successful", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);

	rc = MQTTClient_create(&bclient, options.connection, options.clientid2, MQTTCLIENT_PERSISTENCE_DEFAULT, NULL);
	assert("good rc from create",  rc == MQTTCLIENT_SUCCESS, "rc was %d\n", rc);

	opts.cleansession = 1;
	rc = MQTTClient_connect(bclient, &opts);
	assert("Good rc from connect", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);

	rc = MQTTClient_publish(bclient, topics[1], 5, "qos 0", 0, 0, NULL);
	assert("Good rc from publish", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);

	rc = MQTTClient_publish(bclient, topics[2], 5, "qos 1", 1, 0, NULL);
	assert("Good rc from publish", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);	

	rc = MQTTClient_publish(bclient, topics[3], 5, "qos 2", 2, 0, NULL);
	assert("Good rc from publish", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);

	msleep(2000);

	rc = MQTTClient_disconnect(bclient, 100);
	assert("Disconnect successful", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);

	MQTTClient_destroy(&bclient);

	opts.cleansession = 0;
	rc = MQTTClient_connect(aclient, &opts);
	assert("Good rc from connect", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);

    msleep(1000);  // receive the queued messages

	rc = MQTTClient_disconnect(aclient, 100);
	assert("Disconnect successful", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);

	MQTTClient_destroy(&aclient);

	assert("2 or 3 messages received", messageCount == 3 || messageCount == 2, "messageCount was %d", messageCount);

	MyLog(LOGA_INFO, "This server %s queueing QoS 0 messages for offline clients", (messageCount == 3) ? "is" : "is not");

	clearMessages();

	MyLog(LOGA_INFO, "Offline message queueing test %s", (failures == 0) ?  "succeeded" : "failed");
	return failures;
}


int retained_message_test(void)
{ 
	int i, rc;
	MQTTClient_connectOptions opts = MQTTClient_connectOptions_initializer;
	MQTTClient aclient;

	MyLog(LOGA_INFO, "Retained message test");

	tests = failures = 0;

	opts.keepAliveInterval = 20;
	opts.cleansession = 1;
	opts.username = options.username;
	opts.password = options.password;
	opts.MQTTVersion = options.MQTTVersion;

	assert("0 messages received", messageCount == 0, "messageCount was %d", messageCount);
  
    // set retained messages
	rc = MQTTClient_create(&aclient, options.connection, options.clientid1, MQTTCLIENT_PERSISTENCE_DEFAULT, NULL);
	assert("good rc from create",  rc == MQTTCLIENT_SUCCESS, "rc was %d\n", rc);

	rc = MQTTClient_setCallbacks(aclient, NULL, NULL, messageArrived, NULL);
	assert("Good rc from setCallbacks", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);

	rc = MQTTClient_connect(aclient, &opts);
	assert("Good rc from connect", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);

	rc = MQTTClient_publish(aclient, topics[1], 5, "qos 0", 0, 1, NULL);
	assert("Good rc from publish", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);

	rc = MQTTClient_publish(aclient, topics[2], 5, "qos 1", 1, 1, NULL);
	assert("Good rc from publish", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);	

	rc = MQTTClient_publish(aclient, topics[3], 5, "qos 2", 2, 1, NULL);
	assert("Good rc from publish", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);

    msleep(1000);

	rc = MQTTClient_subscribe(aclient, wildtopics[5], 2);
	assert("Good rc from subscribe", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);

    msleep(2000);

	rc = MQTTClient_disconnect(aclient, 100);
	assert("Disconnect successful", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);

	assert("3 messages received", messageCount == 3, "messageCount was %d", messageCount);

	for (i = 0; i < messageCount; ++i)
	{
		assert("messages should be retained", messagesArrived[i].m->retained, "retained was %d", 
			messagesArrived[i].m->retained);
		MQTTClient_free(messagesArrived[i].topicName);
		MQTTClient_freeMessage(&messagesArrived[i].m);
	}
	messageCount = 0;

    // clear retained messages
	rc = MQTTClient_connect(aclient, &opts);
	assert("Good rc from connect", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);

	rc = MQTTClient_publish(aclient, topics[1], 0, "", 0, 1, NULL);
	assert("Good rc from publish", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);

	rc = MQTTClient_publish(aclient, topics[2], 0, "", 1, 1, NULL);
	assert("Good rc from publish", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);	

	rc = MQTTClient_publish(aclient, topics[3], 0, "", 2, 1, NULL);
	assert("Good rc from publish", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);

    msleep(200); // wait for QoS 2 exchange to be completed
	rc = MQTTClient_subscribe(aclient, wildtopics[5], 2);
	assert("Good rc from subscribe", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);

    msleep(200);

	rc = MQTTClient_disconnect(aclient, 100);
	assert("Disconnect successful", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);

	assert("0 messages received", messageCount == 0, "messageCount was %d", messageCount);

	MQTTClient_destroy(&aclient);

	MyLog(LOGA_INFO, "Retained message test %s", (failures == 0) ?  "succeeded" : "failed");
	return failures;
}

#define SOCKET_ERROR -1

int test6_socket_error(char* aString, int sock)
{
#if defined(_WIN32)
	int errno;
#endif

#if defined(_WIN32)
	errno = WSAGetLastError();
#endif
	if (errno != EINTR && errno != EAGAIN && errno != EINPROGRESS && errno != EWOULDBLOCK)
	{
		if (strcmp(aString, "shutdown") != 0 || (errno != ENOTCONN && errno != ECONNRESET))
			printf("Socket error %d in %s for socket %d", errno, aString, sock);
	}
	return errno;
}

int test6_socket_close(int socket)
{
	int rc;

#if defined(_WIN32)
	if (shutdown(socket, SD_BOTH) == SOCKET_ERROR)
		test6_socket_error("shutdown", socket);
	if ((rc = closesocket(socket)) == SOCKET_ERROR)
		test6_socket_error("close", socket);
#else
	if (shutdown(socket, SHUT_RDWR) == SOCKET_ERROR)
		test6_socket_error("shutdown", socket);
	if ((rc = close(socket)) == SOCKET_ERROR)
		test6_socket_error("close", socket);
#endif
	return rc;
}

typedef struct
{
	int socket;
	time_t lastContact;
#if defined(OPENSSL)
	SSL* ssl;
	SSL_CTX* ctx;
#endif
} networkHandles;


typedef struct
{
	char* clientID;					/**< the string id of the client */
	char* username;					/**< MQTT v3.1 user name */
	char* password;					/**< MQTT v3.1 password */
	unsigned int cleansession : 1;	/**< MQTT clean session flag */
	unsigned int connected : 1;		/**< whether it is currently connected */
	unsigned int good : 1; 			/**< if we have an error on the socket we turn this off */
	unsigned int ping_outstanding : 1;
	int connect_state : 4;
	networkHandles net;
/* ... */
} Clients;


typedef struct
{
	char* serverURI;
	Clients* c;
	MQTTClient_connectionLost* cl;
	MQTTClient_messageArrived* ma;
	MQTTClient_deliveryComplete* dc;
	void* context;

	int connect_sem;
	int rc; /* getsockopt return code in connect */
	int connack_sem;
	int suback_sem;
	int unsuback_sem;
	void* pack;
} MQTTClients;

 
int will_message_test(void)
{
	int i, rc, count = 0;
	MQTTClient_connectOptions opts = MQTTClient_connectOptions_initializer;
	MQTTClient_willOptions wopts =  MQTTClient_willOptions_initializer;
	MQTTClient aclient, bclient;

	MyLog(LOGA_INFO, "Will message test");

	tests = failures = 0;

	opts.keepAliveInterval = 2;
	opts.cleansession = 1;
	opts.username = options.username;
	opts.password = options.password;
	opts.MQTTVersion = options.MQTTVersion;

	opts.will = &wopts;
	opts.will->message = "client not disconnected";
	opts.will->qos = 1;
	opts.will->retained = 0;
	opts.will->topicName = topics[2];

	rc = MQTTClient_create(&aclient, options.connection, options.clientid1, MQTTCLIENT_PERSISTENCE_DEFAULT, NULL);
	assert("good rc from create",  rc == MQTTCLIENT_SUCCESS, "rc was %d\n", rc);

	rc = MQTTClient_connect(aclient, &opts);
	assert("Good rc from connect", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);

	rc = MQTTClient_create(&bclient, options.connection, options.clientid2, MQTTCLIENT_PERSISTENCE_DEFAULT, NULL);
	assert("good rc from create",  rc == MQTTCLIENT_SUCCESS, "rc was %d\n", rc);

	rc = MQTTClient_setCallbacks(bclient, NULL, NULL, messageArrived, NULL);
	assert("Good rc from setCallbacks", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);

	opts.keepAliveInterval = 20;
	opts.will = NULL;
	rc = MQTTClient_connect(bclient, &opts);
	assert("Good rc from connect", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);

	rc = MQTTClient_subscribe(bclient, topics[2], 2);
	assert("Good rc from subscribe", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);

    msleep(100);

   	test6_socket_close(((MQTTClients*)aclient)->c->net.socket); 

	while (messageCount == 0 && ++count < 10)
    	msleep(1000);

	rc = MQTTClient_disconnect(bclient, 100);
	assert("Disconnect successful", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);

	MQTTClient_destroy(&bclient);

	assert("will message received", messageCount == 1, "messageCount was %d", messageCount);

	rc = MQTTClient_disconnect(aclient, 100);

	MQTTClient_destroy(&aclient);

	MyLog(LOGA_INFO, "Will message test %s", (failures == 0) ?  "succeeded" : "failed");
	return failures;
}


int overlapping_subscriptions_test(void)
{
  /* overlapping subscriptions. When there is more than one matching subscription for the same client for a topic,
   the server may send back one message with the highest QoS of any matching subscription, or one message for
   each subscription with a matching QoS. */

	int i, rc;
	MQTTClient_connectOptions opts = MQTTClient_connectOptions_initializer;
	MQTTClient aclient;
	char* topicList[] = {wildtopics[6], wildtopics[0]};
	int qosList[] = {2, 1};

	MyLog(LOGA_INFO, "Starting overlapping subscriptions test");

	clearMessages();
	tests = failures = 0;

	opts.keepAliveInterval = 20;
	opts.cleansession = 1;
	opts.username = options.username;
	opts.password = options.password;
	opts.MQTTVersion = options.MQTTVersion;

	rc = MQTTClient_create(&aclient, options.connection, options.clientid1, MQTTCLIENT_PERSISTENCE_DEFAULT, NULL);
	assert("good rc from create",  rc == MQTTCLIENT_SUCCESS, "rc was %d\n", rc);

	rc = MQTTClient_setCallbacks(aclient, NULL, NULL, messageArrived, NULL);
	assert("Good rc from setCallbacks", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);

	rc = MQTTClient_connect(aclient, &opts);
	assert("Good rc from connect", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);

	rc = MQTTClient_subscribeMany(aclient, 2, topicList, qosList);
	assert("Good rc from subscribe", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);

	rc = MQTTClient_publish(aclient, topics[3], strlen("overlapping topic filters") + 1, 
                "overlapping topic filters", 2, 0, NULL);
	assert("Good rc from publish", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);

    msleep(1000);

	assert("1 or 2 messages received", messageCount == 1 || messageCount == 2, "messageCount was %d", messageCount);

    if (messageCount == 1)
	{
		MyLog(LOGA_INFO, "This server is publishing one message for all matching overlapping subscriptions, not one for each.");
		assert("QoS should be 2", messagesArrived[0].m->qos == 2, "QoS was %d", messagesArrived[0].m->qos);
	}
    else
	{
		MyLog(LOGA_INFO, "This server is publishing one message per each matching overlapping subscription.");
		assert1("QoSs should be 1 and 2", 
			(messagesArrived[0].m->qos == 2 && messagesArrived[1].m->qos == 1) ||
      		(messagesArrived[0].m->qos == 1 && messagesArrived[1].m->qos == 2),
		"QoSs were %d %d", messagesArrived[0].m->qos, messagesArrived[1].m->qos);
	}

	rc = MQTTClient_disconnect(aclient, 100);

	MQTTClient_destroy(&aclient);

	MyLog(LOGA_INFO, "Overlapping subscription test %s", (failures == 0) ?  "succeeded" : "failed");
	return failures;
}


int keepalive_test(void)
{
	/* keepalive processing.  We should be kicked off by the server if we don't send or receive any data, and don't send
	any pings either. */

	int i, rc, count = 0;
	MQTTClient_connectOptions opts = MQTTClient_connectOptions_initializer;
	MQTTClient_willOptions wopts =  MQTTClient_willOptions_initializer;
	MQTTClient aclient, bclient;

	MyLog(LOGA_INFO, "Starting keepalive test");

	tests = failures = 0;
	clearMessages();

	opts.cleansession = 1;
	opts.username = options.username;
	opts.password = options.password;
	opts.MQTTVersion = options.MQTTVersion;

	opts.will = &wopts;
	opts.will->message = "keepalive expiry";
	opts.will->qos = 1;
	opts.will->retained = 0;
	opts.will->topicName = topics[4];

	opts.keepAliveInterval = 20;
	rc = MQTTClient_create(&bclient, options.connection, options.clientid2, MQTTCLIENT_PERSISTENCE_DEFAULT, NULL);
	assert("good rc from create",  rc == MQTTCLIENT_SUCCESS, "rc was %d\n", rc);

	rc = MQTTClient_setCallbacks(bclient, NULL, NULL, messageArrived, NULL);
	assert("Good rc from setCallbacks", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);

	rc = MQTTClient_connect(bclient, &opts);
	assert("Good rc from connect", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);

	rc = MQTTClient_subscribe(bclient, topics[4], 2);
	assert("Good rc from subscribe", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);

	opts.keepAliveInterval = 2;
	rc = MQTTClient_create(&aclient, options.connection, options.clientid1, MQTTCLIENT_PERSISTENCE_DEFAULT, NULL);
	assert("good rc from create",  rc == MQTTCLIENT_SUCCESS, "rc was %d\n", rc);

	rc = MQTTClient_connect(aclient, &opts);
	assert("Good rc from connect", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);

	while (messageCount == 0 && ++count < 20)
    	msleep(1000);

	rc = MQTTClient_disconnect(bclient, 100);
	assert("Disconnect successful", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);

	assert("Should have will message", messageCount == 1, "messageCount was %d", messageCount);

	rc = MQTTClient_disconnect(aclient, 100);

	MQTTClient_destroy(&aclient);

	MyLog(LOGA_INFO, "Keepalive test %s", (failures == 0) ?  "succeeded" : "failed");
	return failures;
}



int redelivery_on_reconnect_test(void)
{
	/* redelivery on reconnect. When a QoS 1 or 2 exchange has not been completed, the server should retry the 
	 appropriate MQTT packets */

	int i, rc, count = 0;
	MQTTClient_connectOptions opts = MQTTClient_connectOptions_initializer;
	MQTTClient aclient;

	MyLog(LOGA_INFO, "Starting redelivery on reconnect test");

	tests = failures = 0;
	clearMessages();

	opts.keepAliveInterval = 0;
	opts.cleansession = 0;
	opts.username = options.username;
	opts.password = options.password;
	opts.MQTTVersion = options.MQTTVersion;

	rc = MQTTClient_create(&aclient, options.connection, options.clientid1, MQTTCLIENT_PERSISTENCE_DEFAULT, NULL);
	assert("good rc from create",  rc == MQTTCLIENT_SUCCESS, "rc was %d\n", rc);

	rc = MQTTClient_connect(aclient, &opts);
	assert("Good rc from connect", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);

	rc = MQTTClient_subscribe(aclient, wildtopics[6], 2);
	assert("Good rc from subscribe", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);
	MQTTClient_yield();

    // no background processing because no callback has been set
	rc = MQTTClient_publish(aclient, topics[1], 6, "qos 1", 2, 0, NULL);
	assert("Good rc from publish", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);

	rc = MQTTClient_publish(aclient, topics[3], 6, "qos 2", 2, 0, NULL);
	assert("Good rc from publish", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);

	rc = MQTTClient_disconnect(aclient, 0);

	assert("No messages should have been received yet", messageCount == 0, "messageCount was %d", messageCount);

	rc = MQTTClient_setCallbacks(aclient, NULL, NULL, messageArrived, NULL);
	assert("Good rc from setCallbacks", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);

	rc = MQTTClient_connect(aclient, &opts);
	assert("Good rc from connect", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);

	while (messageCount < 2 && ++count < 5)
		msleep(1000);

	assert("Should have 2 messages", messageCount == 2, "messageCount was %d", messageCount);

	rc = MQTTClient_disconnect(aclient, 100);

	MQTTClient_destroy(&aclient);

	MyLog(LOGA_INFO, "Redelivery on reconnect test %s", (failures == 0) ?  "succeeded" : "failed");
	return failures;
}



int zero_length_clientid_test(void)
{
	int i, rc, count = 0;
	MQTTClient_connectOptions opts = MQTTClient_connectOptions_initializer;
	MQTTClient aclient;

	MyLog(LOGA_INFO, "Starting zero length clientid test");

	tests = failures = 0;
	clearMessages();

	opts.keepAliveInterval = 0;
	opts.cleansession = 0;
	opts.username = options.username;
	opts.password = options.password;
	opts.MQTTVersion = options.MQTTVersion;

	rc = MQTTClient_create(&aclient, options.connection, "", MQTTCLIENT_PERSISTENCE_DEFAULT, NULL);
	assert("good rc from create",  rc == MQTTCLIENT_SUCCESS, "rc was %d\n", rc);

	rc = MQTTClient_connect(aclient, &opts);
	assert("rc 2 from connect", rc == 2, "rc was %d", rc); // this should always fail

	opts.cleansession = 1;
	rc = MQTTClient_connect(aclient, &opts);
	assert("Connack rc should be 0 or 2", rc == MQTTCLIENT_SUCCESS || rc == 2, "rc was %d", rc);

	MyLog(LOGA_INFO, "This server %s support zero length clientids", (rc == 2) ? "does not" : "does");

	if (rc == MQTTCLIENT_SUCCESS)
		rc = MQTTClient_disconnect(aclient, 100);

	MQTTClient_destroy(&aclient);

	MyLog(LOGA_INFO, "Zero length clientid test %s", (failures == 0) ?  "succeeded" : "failed");
	return failures;
}


int dollar_topics_test(void)
{
  /* $ topics. The specification says that a topic filter which starts with a wildcard does not match topic names that
   	begin with a $.  Publishing to a topic which starts with a $ may not be allowed on some servers (which is entirely valid),
   	so this test will not work and should be omitted in that case.
	*/
	int i, rc, count = 0;
	MQTTClient_connectOptions opts = MQTTClient_connectOptions_initializer;
	MQTTClient aclient;
	char dollartopic[20];

	MyLog(LOGA_INFO, "Starting $ topics test");

	sprintf(dollartopic, "$%s", topics[1]);
  
    clearMessages();

	opts.keepAliveInterval = 5;
	opts.cleansession = 1;
	opts.username = options.username;
	opts.password = options.password;
	opts.MQTTVersion = options.MQTTVersion;

	rc = MQTTClient_create(&aclient, options.connection, options.clientid1, MQTTCLIENT_PERSISTENCE_DEFAULT, NULL);
	assert("good rc from create",  rc == MQTTCLIENT_SUCCESS, "rc was %d\n", rc);

	rc = MQTTClient_setCallbacks(aclient, NULL, NULL, messageArrived, NULL);
	assert("Good rc from setCallbacks", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);

	rc = MQTTClient_connect(aclient, &opts);
	assert("Good rc from connect", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);

	rc = MQTTClient_subscribe(aclient, wildtopics[5], 2);
	assert("Good rc from subscribe", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);

    msleep(1000); // wait for any retained messages, hopefully
    clearMessages();

	rc = MQTTClient_publish(aclient, topics[1], 20, "not sent to dollar topic", 1, 0, NULL);
	assert("Good rc from publish", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);

	rc = MQTTClient_publish(aclient, dollartopic, 20, "sent to dollar topic", 1, 0, NULL);
	assert("Good rc from publish", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);

    msleep(1000);
	assert("Should have 1 message", messageCount == 1, "messageCount was %d", messageCount);

	rc = MQTTClient_disconnect(aclient, 100);
	assert("Disconnect successful", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);

	MQTTClient_destroy(&aclient);

	MyLog(LOGA_INFO, "$ topics test %s", (failures == 0) ?  "succeeded" : "failed");
	return failures;
}


int subscribe_failure_test(void)
{
  /* Subscribe failure.  A new feature of MQTT 3.1.1 is the ability to send back negative reponses to subscribe
   requests.  One way of doing this is to subscribe to a topic which is not allowed to be subscribed to.
  */
	int i, rc, count = 0;
	MQTTClient_connectOptions opts = MQTTClient_connectOptions_initializer;
	MQTTClient aclient;
	int subqos = 2;

	MyLog(LOGA_INFO, "Starting subscribe failure test");
  
    clearMessages();

	opts.keepAliveInterval = 5;
	opts.cleansession = 1;
	opts.username = options.username;
	opts.password = options.password;
	opts.MQTTVersion = options.MQTTVersion;

	rc = MQTTClient_create(&aclient, options.connection, options.clientid1, MQTTCLIENT_PERSISTENCE_DEFAULT, NULL);
	assert("good rc from create",  rc == MQTTCLIENT_SUCCESS, "rc was %d\n", rc);

	rc = MQTTClient_setCallbacks(aclient, NULL, NULL, messageArrived, NULL);
	assert("Good rc from setCallbacks", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);

	rc = MQTTClient_connect(aclient, &opts);
	assert("Good rc from connect", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);
 
	rc = MQTTClient_subscribeMany(aclient, 1, &nosubscribe_topics[0], &subqos);
	assert("Good rc from subscribe", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);
	assert("0x80 rc from subscribe", subqos == 0x80, "subqos was %d", subqos);

	rc = MQTTClient_disconnect(aclient, 100);
	assert("Disconnect successful", rc == MQTTCLIENT_SUCCESS, "rc was %d", rc);

	MQTTClient_destroy(&aclient);

	MyLog(LOGA_INFO, "Subscribe failure test %s", (failures == 0) ?  "succeeded" : "failed");
	return failures;
}


int main(int argc, char** argv)
{
	int i;
	int all_failures = 0;

	getopts(argc, argv);

	for (i = 0; i < options.iterations; ++i)
	{
		cleanup();
		all_failures += basic_test() + 
						offline_message_queueing_test() +
		                retained_message_test() +
		                will_message_test() +
		                overlapping_subscriptions_test() +
		                keepalive_test() +
						redelivery_on_reconnect_test() + 
						zero_length_clientid_test();

		if (options.run_dollar_topics_test)
			all_failures += dollar_topics_test();
		
		if (options.run_subscribe_failure_test)
			all_failures += subscribe_failure_test();
	}

	MyLog(LOGA_INFO, "Test suite %s", (all_failures == 0) ?  "succeeded" : "failed");
}













 







