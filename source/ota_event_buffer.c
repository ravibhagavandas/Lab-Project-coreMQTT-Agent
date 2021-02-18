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
  * @file ota_event_buffer.c
  * @brief Contains functions to manage event buffer pool for OTA demo.
  *
  * The implementation creates a static array of event buffers for OTA. The event buffers
  * are used to copy the incoming control or data messages received over HTTP or MQTT protocol
  * for the OTA agent to consume.
  *
  * See https://freertos.org/ota/ota-mqtt-agent-demo.html
  */



/* Kernel includes. */
#include "FreeRTOS.h"
#include "semphr.h"

#include "ota_config.h"
#include "ota_event_buffer.h"


/**
 * @brief A statically allocated array of event buffers used by the OTA agent.
 * Maximum number of buffers are determined by how many chunks are requested
 * by OTA agent at a time along with an extra buffer to handle control message.
 * The size of each buffer is determined by the maximum size of firmware image
 * chunk, and other metadata send along with the chunk.
 */
static OtaEventData_t eventBuffer[otaconfigMAX_NUM_OTA_DATA_BUFFERS] = { 0 };

/*
 * @brief Mutex used to manage thread safe access of OTA event buffers.
 */
static SemaphoreHandle_t bufferMutex;

void vOTADemoEventBufferPoolInit(void)
{
	bufferMutex = xSemaphoreCreateMutex();
}

OtaEventData_t* vOTADemoEventBufferGet(size_t bufferSize)
{
    size_t index = 0;
    OtaEventData_t* pFreeBuffer = NULL;

    configASSERT(bufferSize <= OTA_DATA_BLOCK_SIZE);

    /* Block indefinitely to grab the mutex. */
    (void)xSemaphoreTake(bufferMutex, portMAX_DELAY);

        for (index = 0; index < otaconfigMAX_NUM_OTA_DATA_BUFFERS; index++)
        {
            if (eventBuffer[index].bufferUsed == false)
            {
                eventBuffer[index].bufferUsed = true;
                pFreeBuffer = &eventBuffer[index];
                break;
            }
        }

    (void)xSemaphoreGive(bufferMutex);

    return pFreeBuffer;

}

void vOTADemoEventBufferFree(OtaEventData_t* const pxEventBuffer)
{
     /* Block indefinitely to grab the mutex. */
    ( void )xSempahoreTake(bufferMutex, portMAX_DELAY);
    
    pxEventBuffer->bufferUsed = false;
    
    (void)xSemaphoreGive(bufferMutex);
    
}
