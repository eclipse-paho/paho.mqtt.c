/*******************************************************************************
 * Copyright (c) 2026 Contributors to the Eclipse Foundation
 * SPDX-License-Identifier: EPL-2.0 OR BSD-3-Clause
 *******************************************************************************/
#include "SocketConnect.h"
#include "Log.h"
#include <stdlib.h>
#include <limits.h>
#include <string.h>
#include "Heap.h"

#define CONNECT_DELAY_MS 200
#define CONNECT_LAUNCH_LIMIT 8

typedef struct
{
    struct addrinfo* next;
    unsigned int remaining;
    int family;
    SOCKET socket;
    uint64_t registration_id;
    ELAPSED_TIME_TYPE deadline;
} ConnectSlot;

struct SocketConnect
{
    struct SocketConnect* next;
    struct addrinfo* addresses;
    ConnectSlot slots[2];
    START_TIME_TYPE start;
    ELAPSED_TIME_TYPE timeout;
    ELAPSED_TIME_TYPE second_start;
    uint64_t id;
    int second_enabled;
    int done;
    int error;
    SOCKET winner;
};

static SocketConnect* attempts;
static SocketConnect* cursor;
static uint64_t next_id;

static void closeSlot(ConnectSlot* slot)
{
    if (slot->socket != INVALID_SOCKET)
        Socket_close(slot->socket);
    slot->socket = INVALID_SOCKET;
    slot->registration_id = 0;
}

static void finish(SocketConnect* attempt, int error, int winner)
{
    int i;
    if (attempt->done)
        return;
    attempt->done = 1;
    attempt->error = error;
    if (winner >= 0)
    {
        attempt->winner = attempt->slots[winner].socket;
        attempt->slots[winner].socket = INVALID_SOCKET;
        Socket_connectComplete(attempt->winner);
    }
    for (i = 0; i < 2; ++i)
        closeSlot(&attempt->slots[i]);
    Log(TRACE_MINIMUM, -1, "TCP attempt %llu complete: rc %d, family %d, elapsed %llu ms",
        (unsigned long long)attempt->id, error, winner < 0 ? 0 : attempt->slots[winner].family,
        (unsigned long long)MQTTTime_elapsed(attempt->start));
}

/* At most one socket is started per visit, so a long list of immediate
 * failures cannot prevent other attempts or established clients making progress.
 */
static int advance(SocketConnect* attempt)
{
    int i;
    ELAPSED_TIME_TYPE elapsed = MQTTTime_elapsed(attempt->start);
    if (attempt->done)
        return 0;
    if (elapsed >= attempt->timeout)
    {
        finish(attempt, ETIMEDOUT, -1);
        return 0;
    }
    for (i = 0; i < 2; ++i)
    {
        ConnectSlot* slot = &attempt->slots[i];
        if (slot->socket != INVALID_SOCKET && elapsed >= slot->deadline)
        {
            Log(TRACE_MINIMUM, -1, "TCP attempt %llu family %d candidate timeout",
                (unsigned long long)attempt->id, slot->family);
            closeSlot(slot);
            attempt->error = ETIMEDOUT;
        }
    }
    if (elapsed >= attempt->second_start ||
        (attempt->slots[0].socket == INVALID_SOCKET && attempt->slots[0].remaining == 0))
        attempt->second_enabled = 1;

    /* When due, the second family takes priority over another quick failure
     * in the preferred family. Its launch deadline is never restarted.
     */
    for (i = 1; i >= 0; --i)
    {
        ConnectSlot* slot = &attempt->slots[i];
        struct addrinfo* address;
        ELAPSED_TIME_TYPE budget;
        int rc;
        if ((i == 1 && !attempt->second_enabled) || slot->socket != INVALID_SOCKET ||
            slot->remaining == 0)
            continue;
        address = slot->next;
        while (address && address->ai_family != slot->family)
            address = address->ai_next;
        slot->next = address->ai_next;
        --slot->remaining;
        budget = (attempt->timeout - elapsed) / ((ELAPSED_TIME_TYPE)slot->remaining + 1);
        if (budget == 0)
            budget = 1;
        slot->deadline = elapsed + budget;
        Log(TRACE_MINIMUM, -1, "TCP attempt %llu start family %d, elapsed %llu ms, budget %llu ms",
            (unsigned long long)attempt->id, slot->family, (unsigned long long)elapsed,
            (unsigned long long)budget);
        rc = Socket_connectAddress(address, &slot->socket, &slot->registration_id);
        if (rc == 0)
            finish(attempt, 0, i);
        else if (rc != EINPROGRESS && rc != EWOULDBLOCK)
        {
            closeSlot(slot);
            attempt->error = rc;
            if (!attempt->slots[0].remaining && !attempt->slots[1].remaining &&
                attempt->slots[0].socket == INVALID_SOCKET &&
                attempt->slots[1].socket == INVALID_SOCKET)
                finish(attempt, rc, -1);
        }
        return 1;
    }
    if (attempt->slots[0].remaining == 0 && attempt->slots[1].remaining == 0 &&
        attempt->slots[0].socket == INVALID_SOCKET && attempt->slots[1].socket == INVALID_SOCKET)
        finish(attempt, attempt->error, -1);
    return 0;
}

int SocketConnect_start(SocketConnect** output, const char* host, size_t length, int port,
                        START_TIME_TYPE start, ELAPSED_TIME_TYPE timeout)
{
    SocketConnect* attempt;
    struct addrinfo* address;
    int rc, first = 0;
    SocketConnect_cancel(output);
    attempt = malloc(sizeof(*attempt));
    if (!attempt)
        return SOCKET_ERROR;
    memset(attempt, 0, sizeof(*attempt));
    attempt->winner = INVALID_SOCKET;
    attempt->slots[0].socket = attempt->slots[1].socket = INVALID_SOCKET;
    attempt->start = start;
    attempt->timeout = timeout;
    attempt->error = SOCKET_ERROR;
    attempt->id = ++next_id;
    rc = Socket_resolve(host, length, port, &attempt->addresses);
    for (address = attempt->addresses; address; address = address->ai_next)
    {
        int index;
        if (address->ai_family != AF_INET && address->ai_family != AF_INET6)
            continue;
        if (first == 0)
            first = address->ai_family;
        index = address->ai_family == first ? 0 : 1;
        attempt->slots[index].family = address->ai_family;
        if (!attempt->slots[index].next)
            attempt->slots[index].next = address;
        ++attempt->slots[index].remaining;
    }
    attempt->second_start = MQTTTime_elapsed(start) + CONNECT_DELAY_MS;
    attempt->next = attempts;
    attempts = attempt;
    *output = attempt;
    if (rc != 0 || first == 0)
        finish(attempt, SOCKET_ERROR, -1);
    else
        advance(attempt);
    Socket_interrupt();
    /* Even immediate success is consumed by the event loop, after the caller
     * has installed its connect command and callbacks.
     */
    return EINPROGRESS;
}

void SocketConnect_process(SOCKET ready, uint64_t registration_id)
{
    SocketConnect* attempt;
    unsigned int visits = 0, count = 0, launches = 0;
    for (attempt = attempts; attempt; attempt = attempt->next)
    {
        int i;
        ++count;
        if (attempt->done)
            continue;
        for (i = 0; i < 2; ++i)
        {
            ConnectSlot* slot = &attempt->slots[i];
            if (ready > 0 && slot->socket == ready && slot->registration_id == registration_id)
            {
                int rc = Socket_connectResult(ready);
                if (rc == 0 && MQTTTime_elapsed(attempt->start) < attempt->timeout)
                    finish(attempt, 0, i);
                else if (rc != EINPROGRESS && rc != EWOULDBLOCK)
                {
                    attempt->error = rc == 0 ? ETIMEDOUT : rc;
                    closeSlot(slot);
                }
                break;
            }
        }
    }
    if (!cursor)
        cursor = attempts;
    while (cursor && launches < CONNECT_LAUNCH_LIMIT && visits < count)
    {
        int launched;
        attempt = cursor;
        cursor = cursor->next ? cursor->next : attempts;
        launched = advance(attempt);
        launches += launched;
        visits = launched ? 0 : visits + 1;
    }
}

int SocketConnect_timeout(int timeout)
{
    SocketConnect* attempt;
    ELAPSED_TIME_TYPE delay = timeout < 0 ? INT_MAX : (ELAPSED_TIME_TYPE)timeout;
    for (attempt = attempts; attempt; attempt = attempt->next)
    {
        ELAPSED_TIME_TYPE elapsed = MQTTTime_elapsed(attempt->start);
        ELAPSED_TIME_TYPE due = attempt->timeout;
        int i;
        if (attempt->done)
            return 0;
        for (i = 0; i < 2; ++i)
        {
            ConnectSlot* slot = &attempt->slots[i];
            if (slot->socket != INVALID_SOCKET)
            {
                if (slot->deadline < due)
                    due = slot->deadline;
            }
            else if (slot->remaining)
            {
                ELAPSED_TIME_TYPE launch = i == 0 || attempt->second_enabled ||
                                                   (attempt->slots[0].socket == INVALID_SOCKET &&
                                                    !attempt->slots[0].remaining)
                                               ? elapsed
                                               : attempt->second_start;
                if (launch < due)
                    due = launch;
            }
        }
        if (due <= elapsed)
            return 0;
        if (due - elapsed < delay)
            delay = due - elapsed;
    }
    return (int)delay;
}

void SocketConnect_cancel(SocketConnect** output)
{
    SocketConnect* attempt = *output;
    SocketConnect** link = &attempts;
    if (!attempt)
        return;
    *output = NULL;
    while (*link && *link != attempt)
        link = &(*link)->next;
    if (*link)
        *link = attempt->next;
    if (cursor == attempt)
        cursor = attempt->next ? attempt->next : attempts;
    closeSlot(&attempt->slots[0]);
    closeSlot(&attempt->slots[1]);
    if (attempt->winner != INVALID_SOCKET)
        Socket_close(attempt->winner);
    if (attempt->addresses)
        Socket_freeAddresses(attempt->addresses);
    free(attempt);
    Socket_interrupt();
}

int SocketConnect_takeResult(SocketConnect** output, SOCKET* winner, int* error)
{
    SocketConnect* attempt = *output;
    if (!attempt || !attempt->done)
        return 0;
    *winner = attempt->winner;
    *error = attempt->error;
    attempt->winner = INVALID_SOCKET;
    SocketConnect_cancel(output);
    return 1;
}
