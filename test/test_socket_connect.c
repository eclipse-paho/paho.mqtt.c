/* Copyright (c) 2026 Contributors to the Eclipse Foundation
 * SPDX-License-Identifier: EPL-2.0 OR BSD-3-Clause
 * Socket/clock substitutes drive the real connection scheduler.
 */
#include "SocketConnect.h"
#include "Socket.h"
#include "Log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static ELAPSED_TIME_TYPE now_ms;
static int failures;
static int scenario;
static int launch_count, close_count, complete_count, interrupt_count;
static SOCKET launched[64];
static uint64_t registration_ids[64];
static int launch_family[64];
static ELAPSED_TIME_TYPE launch_time[64];
static int launch_owner[64], live[64], resolve_count;
static int ready_rc[64];
static SOCKET next_socket = 100;
static uint64_t next_registration_id;

#define CHECK(x)                                                                                   \
    do                                                                                             \
    {                                                                                              \
        if (!(x))                                                                                  \
        {                                                                                          \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x);                                    \
            ++failures;                                                                            \
            return 0;                                                                              \
        }                                                                                          \
    } while (0)

START_TIME_TYPE MQTTTime_start_clock(void)
{
    START_TIME_TYPE t = START_TIME_ZERO;
    return t;
}
START_TIME_TYPE MQTTTime_now(void) { return MQTTTime_start_clock(); }
ELAPSED_TIME_TYPE MQTTTime_elapsed(START_TIME_TYPE ignored)
{
    (void)ignored;
    return now_ms;
}
DIFF_TIME_TYPE MQTTTime_difftime(START_TIME_TYPE a, START_TIME_TYPE b)
{
    (void)a;
    (void)b;
    return (DIFF_TIME_TYPE)now_ms;
}
void MQTTTime_sleep(ELAPSED_TIME_TYPE ms) { now_ms += ms; }
void Log(enum LOG_LEVELS l, int line, const char* f, ...)
{
    (void)l;
    (void)line;
    (void)f;
}
trace_settings_type trace_settings;

static struct addrinfo* node(int family)
{
    struct addrinfo* a = (struct addrinfo*)calloc(1, sizeof(*a));
    a->ai_family = family;
    return a;
}
static void add(struct addrinfo** head, int family)
{
    struct addrinfo *a = node(family), *p;
    if (!*head)
    {
        *head = a;
        return;
    }
    for (p = *head; p->ai_next; p = p->ai_next)
    {
    }
    p->ai_next = a;
}

int Socket_resolve(const char* host, size_t length, int port, struct addrinfo** out)
{
    size_t i;
    struct addrinfo* a;
    (void)length;
    (void)port;
    *out = NULL;
    ++resolve_count;
    if (scenario == 9)
    {
        now_ms += 30;
        return EAI_AGAIN;
    }
    if (scenario == 10)
        now_ms += 100;
    if (strcmp(host, "nine") == 0)
        for (i = 0; i < 9; ++i)
            add(out, AF_INET);
    else if (strcmp(host, "mixed") == 0)
    {
        add(out, AF_INET6);
        add(out, AF_INET);
        add(out, AF_INET6);
        add(out, AF_INET);
    }
    else if (strcmp(host, "v4") == 0)
        for (i = 0; i < 10; ++i)
            add(out, AF_INET);
    else if (strcmp(host, "v6") == 0)
        for (i = 0; i < 2; ++i)
            add(out, AF_INET6);
    else if (strcmp(host, "v4v6") == 0)
    {
        add(out, AF_INET);
        add(out, AF_INET6);
        add(out, AF_INET);
    }
    else if (strcmp(host, "v6v4") == 0)
    {
        add(out, AF_INET6);
        add(out, AF_INET);
        add(out, AF_INET6);
    }
    else if (strcmp(host, "pair") == 0)
    {
        add(out, AF_INET);
        add(out, AF_INET6);
    }
    else
        add(out, AF_INET);
    for (a = *out; a; a = a->ai_next)
        a->ai_flags = resolve_count;
    return 0;
}
void Socket_freeAddresses(struct addrinfo* a)
{
    while (a)
    {
        struct addrinfo* n = a->ai_next;
        free(a);
        a = n;
    }
}
int Socket_connectAddress(const struct addrinfo* a, SOCKET* s, uint64_t* registration_id)
{
    int i = launch_count++;
    next_socket = 100;
    while (live[next_socket - 100])
        ++next_socket;
    *s = next_socket;
    live[*s - 100] = 1;
    *registration_id = ++next_registration_id;
    launched[i] = *s;
    registration_ids[i] = *registration_id;
    launch_family[i] = a->ai_family;
    launch_time[i] = now_ms;
    launch_owner[i] = a->ai_flags;
    if (scenario == 4)
        return ECONNREFUSED;
    if (scenario == 5 && i == 0)
        return 0;
    return scenario == 4 ? ECONNREFUSED : EINPROGRESS;
}
int Socket_connectResult(SOCKET s)
{
    int i;
    for (i = launch_count - 1; i >= 0; --i)
        if (launched[i] == s)
            return ready_rc[i];
    return ECONNREFUSED;
}
void Socket_connectComplete(SOCKET s)
{
    (void)s;
    ++complete_count;
}
int Socket_close(SOCKET s)
{
    if (s < 100 || s >= 164 || !live[s - 100])
    {
        fprintf(stderr, "double/invalid close\n");
        abort();
    }
    live[s - 100] = 0;
    ++close_count;
    return 0;
}
int Socket_interrupt(void)
{
    ++interrupt_count;
    return 0;
}

static void reset(void)
{
    int i;
    for (i = 0; i < 64; ++i)
        if (live[i])
        {
            fprintf(stderr, "socket leak\n");
            abort();
        }
    resolve_count = 0;
    now_ms = 0;
    scenario = 0;
    launch_count = close_count = complete_count = interrupt_count = 0;
    next_socket = 100;
    memset(launched, 0, sizeof(launched));
    memset(registration_ids, 0, sizeof(registration_ids));
    memset(launch_time, 0, sizeof(launch_time));
    memset(launch_owner, 0, sizeof(launch_owner));
    memset(ready_rc, 0, sizeof(ready_rc));
}
static SocketConnect* start(const char* host, int timeout)
{
    SocketConnect* a = NULL;
    START_TIME_TYPE t = START_TIME_ZERO;
    CHECK(SocketConnect_start(&a, host, strlen(host), 1883, t, (ELAPSED_TIME_TYPE)timeout) ==
          EINPROGRESS);
    return a;
}
static int test_delay_order(void)
{
    SocketConnect* a;
    reset();
    a = start("v4v6", 1000);
    CHECK(a && launch_count == 1 && launch_family[0] == AF_INET);
    now_ms = 199;
    SocketConnect_process(0, 0);
    CHECK(launch_count == 1);
    now_ms = 200;
    SocketConnect_process(0, 0);
    CHECK(launch_count == 2 && launch_family[1] == AF_INET6);
    SocketConnect_cancel(&a);
    return 1;
}
static int test_symmetric_delay(void)
{
    SocketConnect* a;
    reset();
    a = start("v6v4", 1000);
    CHECK(launch_count == 1 && launch_family[0] == AF_INET6);
    now_ms = 200;
    SocketConnect_process(0, 0);
    CHECK(launch_count == 2 && launch_family[1] == AF_INET);
    SocketConnect_cancel(&a);
    return 1;
}
static int test_slow_preferred_wins(void)
{
    SocketConnect* a;
    SOCKET w;
    int e;
    reset();
    a = start("pair", 1000);
    CHECK(launch_count == 1);
    now_ms = 200;
    SocketConnect_process(0, 0);
    CHECK(launch_count == 2);
    SocketConnect_process(launched[0], registration_ids[0]);
    CHECK(SocketConnect_takeResult(&a, &w, &e) && e == 0 && w == launched[0] &&
          complete_count == 1);
    Socket_close(w);
    return 1;
}
static int test_blackhole_rotation(void)
{
    SocketConnect* a;
    int before;
    reset();
    a = start("v4", 1000);
    CHECK(launch_count == 1);
    now_ms = 500;
    SocketConnect_process(0, 0);
    CHECK(launch_count == 2 && launch_family[1] == AF_INET);
    before = close_count;
    SocketConnect_cancel(&a);
    CHECK(close_count == before + 1);
    SocketConnect_cancel(&a);
    CHECK(close_count == before + 1);
    return 1;
}
static int test_immediate_limit_and_chain(void)
{
    SocketConnect* a;
    reset();
    scenario = 4;
    a = start("v4", 1000);
    CHECK(launch_count == 1);
    SocketConnect_process(0, 0);
    CHECK(launch_count == 9);
    SocketConnect_process(0, 0);
    CHECK(launch_count == 10);
    CHECK(SocketConnect_timeout(1000) == 0);
    SocketConnect_cancel(&a);
    return 1;
}
static int test_timeout_and_dns(void)
{
    SocketConnect* a;
    SOCKET w;
    int e;
    reset();
    a = start("v4", 100);
    now_ms = 100;
    SocketConnect_process(0, 0);
    CHECK(SocketConnect_takeResult(&a, &w, &e) && e == ETIMEDOUT);
    CHECK(!SocketConnect_takeResult(&a, &w, &e));
    reset();
    scenario = 9;
    a = start("dns", 100);
    CHECK(SocketConnect_takeResult(&a, &w, &e) && e == SOCKET_ERROR);
    return 1;
}
static int test_single_winner_and_cancel_reuse(void)
{
    SocketConnect *a, *b;
    SOCKET w;
    int e;
    reset();
    a = start("pair", 1000);
    now_ms = 200;
    SocketConnect_process(0, 0);
    SocketConnect_process(launched[0], registration_ids[0]);
    CHECK(SocketConnect_takeResult(&a, &w, &e));
    SocketConnect_process(launched[1], registration_ids[1]);
    CHECK(complete_count == 1);
    Socket_close(w);
    reset();
    a = start("pair", 1000);
    {
        SOCKET old = launched[0];
        uint64_t registration_id = registration_ids[0];
        SocketConnect_cancel(&a);
        CHECK(close_count == 1);
        b = start("pair", 1000);
        CHECK(launch_count == 2);
        CHECK(launched[1] == old && registration_ids[1] != registration_id);
        SocketConnect_process(old, registration_id);
        CHECK(!SocketConnect_takeResult(&b, &w, &e));
        SocketConnect_cancel(&b);
    }
    return 1;
}
static int test_fairness(void)
{
    SocketConnect *a, *b;
    int i, counts[3] = {0};
    reset();
    scenario = 4;
    a = start("v4", 1000);
    b = start("v4", 1000);
    CHECK(launch_count == 2);
    SocketConnect_process(0, 0);
    CHECK(launch_count == 10);
    for (i = 2; i < launch_count; ++i)
        ++counts[launch_owner[i]];
    CHECK(counts[1] == 4 && counts[2] == 4);
    SocketConnect_cancel(&a);
    SocketConnect_cancel(&b);
    return 1;
}
static int test_budget_boundaries(void)
{
    SocketConnect* a;
    SOCKET winner;
    int error;
    reset();
    a = start("mixed", 1000);
    CHECK(launch_count == 1 && SocketConnect_timeout(1000) == 200);
    now_ms = 200;
    SocketConnect_process(0, 0);
    CHECK(launch_count == 2 && close_count == 0);
    now_ms = 499;
    SocketConnect_process(0, 0);
    CHECK(launch_count == 2 && SocketConnect_timeout(1000) == 1);
    now_ms = 500;
    SocketConnect_process(0, 0);
    CHECK(launch_count == 3 && launch_family[2] == AF_INET6 && launch_time[2] == 500);
    now_ms = 599;
    SocketConnect_process(0, 0);
    CHECK(launch_count == 3);
    now_ms = 600;
    SocketConnect_process(0, 0);
    CHECK(launch_count == 4 && launch_family[3] == AF_INET && launch_time[3] == 600);
    CHECK(close_count == 2);
    now_ms = 1000;
    SocketConnect_process(0, 0);
    CHECK(SocketConnect_takeResult(&a, &winner, &error) && error == ETIMEDOUT && close_count == 4);
    reset();
    scenario = 10;
    a = start("pair", 100);
    CHECK(launch_count == 0);
    CHECK(SocketConnect_takeResult(&a, &winner, &error) && error == ETIMEDOUT);
    return 1;
}
static int test_exhaustion_at_dispatch_limit(void)
{
    SocketConnect* a;
    SOCKET winner;
    int error;
    reset();
    scenario = 4;
    a = start("nine", 1000);
    SocketConnect_process(0, 0);
    CHECK(launch_count == 9 && close_count == 9);
    CHECK(SocketConnect_takeResult(&a, &winner, &error) && error == ECONNREFUSED);
    return 1;
}
static int test_immediate_and_delayed_failure(void)
{
    SocketConnect* a;
    SOCKET winner;
    int error;
    reset();
    scenario = 5;
    a = start("pair", 1000);
    CHECK(SocketConnect_takeResult(&a, &winner, &error) && !error && launch_count == 1);
    Socket_close(winner);
    reset();
    a = start("pair", 1000);
    now_ms = 50;
    ready_rc[0] = ECONNREFUSED;
    SocketConnect_process(launched[0], registration_ids[0]);
    CHECK(launch_count == 2 && launch_family[1] == AF_INET6 && launch_time[1] == 50);
    SocketConnect_process(launched[1], registration_ids[1]);
    CHECK(SocketConnect_takeResult(&a, &winner, &error) && !error);
    Socket_close(winner);
    return 1;
}
int main(void)
{
    int passed = 0;
    passed += test_delay_order();
    passed += test_symmetric_delay();
    passed += test_slow_preferred_wins();
    passed += test_blackhole_rotation();
    passed += test_immediate_limit_and_chain();
    passed += test_timeout_and_dns();
    passed += test_single_winner_and_cancel_reuse();
    passed += test_fairness();
    passed += test_budget_boundaries();
    passed += test_exhaustion_at_dispatch_limit();
    passed += test_immediate_and_delayed_failure();
    reset();
    printf("socket_connect tests: %d passed, %d failed\n", passed, failures);
    return failures ? 1 : 0;
}
