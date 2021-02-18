/*
 * FreeRTOS V202011.00
 * Copyright (C) 2020 Amazon.com, Inc. or its affiliates.  All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
 * the Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * http://aws.amazon.com/freertos
 * http://www.FreeRTOS.org
 */

 /**
  * @file ota_event_buffer.h
  * @brief OTA event buffer APIs.
  */


#include "ota.h"

  /**
   * @brief Initialize OTA event buffer pool.
   *
   * Demo uses a simple statically allocated array of fixed size event buffers. The
   * number of event buffers is configured by the param otaconfigMAX_NUM_OTA_DATA_BUFFERS
   * within ota_config.h.
   *
   */
void vOTADemoEventBufferPoolInit(void);

  /**
   * @brief Fetch an unused OTA event buffer from the pool.
   *
   * This function is used to fetch a free buffer from the pool for processing
   * by the OTA agent task. It uses a mutex for thread safe access to the pool.
   * 
   * @param[in] bufferSize The size of buffer requested.
   * @return A pointer to an unusued buffer. NULL if there are no buffers available.
   */
OtaEventData_t* vOTADemoEventBufferGet(size_t bufferSize)

/**
 * @brief Free an event buffer back to pool
 *
 * OTA demo uses a statically allocated array of fixed size event buffers . The
 * number of event buffers is configured by the param otaconfigMAX_NUM_OTA_DATA_BUFFERS
 * within ota_config.h. The function is used by the OTA application callback to free a buffer,
 * after OTA agent has completed processing with the event. The access to the pool is made thread safe
 * using a mutex.
 *
 * @param[in] pxBuffer Pointer to the buffer to be freed.
 */
void vOTADemoEventBufferFree(OtaEventData_t* const pxEventBuffer);