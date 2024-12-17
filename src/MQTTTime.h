/*******************************************************************************
 * Copyright (c) 2020 IBM Corp.
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

#if !defined(MQTTTIME_H)
#define MQTTTIME_H

#include <stdint.h>

#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
#if WINVER >= _WIN32_WINNT_VISTA
#define START_TIME_TYPE ULONGLONG
#define START_TIME_ZERO 0
#else
#define START_TIME_TYPE DWORD
#define START_TIME_ZERO 0
#endif
#elif defined(AIX)
#define START_TIME_TYPE struct timespec
#define START_TIME_ZERO {0, 0}
#else
#include <sys/time.h>
#define START_TIME_TYPE struct timeval
#define START_TIME_ZERO {0, 0}
#endif

#define ELAPSED_TIME_TYPE uint64_t
#define DIFF_TIME_TYPE int64_t

void MQTTTime_sleep(ELAPSED_TIME_TYPE milliseconds);
START_TIME_TYPE MQTTTime_start_clock(void);
START_TIME_TYPE MQTTTime_now(void);
ELAPSED_TIME_TYPE MQTTTime_elapsed(START_TIME_TYPE milliseconds);
DIFF_TIME_TYPE MQTTTime_difftime(START_TIME_TYPE t_new, START_TIME_TYPE t_old);

#endif
