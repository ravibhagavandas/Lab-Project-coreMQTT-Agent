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
  * @file ota_over_mqtt_demo.c
  * @brief Over The Air Update demo using coreMQTT Agent.
  *
  * The file demonstrates how to perform Over The Air update using OTA agent and coreMQTT
  * library. It creates an OTA agent task which manages the OTA firmware update
  * for the device. The example also provides implementations to subscribe, publish,
  * and receive data from an MQTT broker. The implementation uses coreMQTT agent which manages
  * thread safety of the MQTT operations and allows OTA agent to share the same MQTT
  * broker connection with other tasks. OTA agent invokes the callback implementations to
  * publish job related control information, as well as receive chunks
  * of pre-signed firmware image from the MQTT broker.
  *
  * See https://freertos.org/mqtt/mqtt-agent-demo.html
  * See https://freertos.org/ota/ota-mqtt-agent-demo.html
  */

  /* Standard includes. */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

/* Kernel includes. */
#include "FreeRTOS.h"
#include "task.h"

#include "ota_config.h"
#include "demo_config.h"

/* OTA Library include. */
#include "ota.h"

/* OTA Library Interface include. */
#include "ota_os_freertos.h"
#include "ota_platform_interface.h"
#include "ota_mqtt_interface.h"
#include "ota_http_interface.h"

/* Include firmware version struct definition. */
#include "ota_appversion32.h"

/* Include platform abstraction header. */
#include "ota_pal.h"

/* HTTP library include. */
#include "core_http_client.h"

/* Transport interface include for HTTPS. */
#if defined( democonfigUSE_TLS ) && ( democonfigUSE_TLS == 1 )
 #include "using_mbedtls.h"
#else
#error "OTA over HTTPS demo requires a secure connection using TLS."
#endif

/*------------- Demo configurations -------------------------*/

#ifndef democonfigCLIENT_IDENTIFIER
#error "Please define the democonfigCLIENT_IDENTIFIER with the thing name for which OTA is performed"
#endif

/**
 * @brief The maximum size of the file paths used in the demo.
 */
#define otaexampleMAX_FILE_PATH_SIZE                     ( 260 )

 /**
  * @brief The maximum size of the stream name required for downloading update file
  * from streaming service.
  */
#define otaexampleMAX_STREAM_NAME_SIZE                   ( 128 )

  /**
   * @brief The delay used in the OTA demo task to periodically output the OTA
   * statistics like number of packets received, dropped, processed and queued per connection.
   */
#define otaexampleTASK_DELAY_MS                          ( 1000U )

             /**
              * @brief Task priority of OTA agent.
              */
#define otaexampleAGENT_TASK_PRIORITY            ( tskIDLE_PRIORITY + 1 )

              /**
               * @brief Maximum stack size of OTA agent task.
               */
#define otaexampleAGENT_TASK_STACK_SIZE          ( 4096 )


               /**
* @brief The maximum size of the HTTP header.
* /
#define HTTP_HEADER_SIZE_MAX             ( 1024U )

/* HTTP buffers used for http request and response. */
#define HTTP_USER_BUFFER_LENGTH          ( otaconfigFILE_BLOCK_SIZE + HTTP_HEADER_SIZE_MAX )

               /**
                * @brief The version for the firmware which is running. OTA agent uses this
                * version number to perform anti-rollback validation. The firmware version for the
                * download image should be higher than the current version, otherwise the new image is
                * rejected in self test phase.
                */
#define APP_VERSION_MAJOR                        0
#define APP_VERSION_MINOR                        9
#define APP_VERSION_BUILD                        2

/**
 * @brief Initialize OTA Http interface.
 *
 * @param[in] pUrl Pointer to the pre-signed url for downloading update file.
 * @return OtaHttpStatus_t OtaHttpSuccess if success ,
 *                         OtaHttpInitFailed on failure.
 */
static OtaHttpStatus_t httpInit(char* pUrl);

/**
 * @brief Request file block over HTTP.
 *
 * @param[in] rangeStart  Starting index of the file data
 * @param[in] rangeEnd    Last index of the file data
 * @return OtaHttpStatus_t OtaHttpSuccess if success ,
 *                         other errors on failure.
 */
static OtaHttpStatus_t httpRequest(uint32_t rangeStart,
    uint32_t rangeEnd);

/**
 * @brief Deinitialize and cleanup of the HTTP connection.
 *
 * @return OtaHttpStatus_t  OtaHttpSuccess if success ,
 *                          OtaHttpRequestFailed on failure.
 */
static OtaHttpStatus_t httpDeinit(void);


/**
 * @brief The function which runs the OTA agent task.
 *
 * The function runs the OTA Agent Event processing loop, which waits for
 * any events for OTA agent and process them. The loop never returns until the OTA agent
 * is shutdown. The tasks exits gracefully by freeing up all resources in the event of an
 *  OTA agent shutdown.
 *
 * @param[in] pvParam Any parameters to be passed to OTA agent task.
 */
static void prvOTAAgentTask(void* pvParam);

/**
 * @brief The function which runs the OTA demo task.
 *
 * The demo task initializes the OTA agent an loops until OTA agent is shutdown.
 * It reports OTA update statistics (which includes number of blocks received, processed and dropped),
 * at regular intervals.
 *
 * @param[in] pvParam Any parameters to be passed to OTA demo task.
 */
static void prvOTADemoTask(void* pvParam);


/**
 * @brief Function used by OTA agent to publish control messages to the MQTT broker.
 *
 * The implementation uses MQTT agent to queue a publish request. It then waits
 * for the request complete notification from the agent. The notification along with result of the
 * operation is sent back to the caller task using xTaksNotify API. For publishes involving QOS 1 and
 * QOS2 the operation is complete once an acknwoledgment (PUBACK) is received. OTA agent uses this function
 * to fetch new job, provide status update and send other control related messges to the MQTT broker.
 *
 * @param[in] pacTopic Topic to publish the control packet to.
 * @param[in] topicLen Length of the topic string.
 * @param[in] pMsg Message to publish.
 * @param[in] msgSize Size of the message to publish.
 * @param[in] qos Qos for the publish.
 * @return OtaMqttSuccess if successful. Appropirate error code otherwise.
 */
extern OtaMqttStatus_t vOTADemoMQTTPublish(const char* const pacTopic,
    uint16_t topicLen,
    const char* pMsg,
    uint32_t msgSize,
    uint8_t qos);

/**
 * @brief Function used by OTA agent to subscribe for a control or data packet from the MQTT broker.
 *
 * The implementation queues a SUBSCRIBE request for the topic filter with the MQTT agent. It then waits for
 * a notification of the request completion. Notification will be sent back to caller task,
 * using xTaskNotify APIs. MQTT agent also stores a callback provided by this function with
 * the associated topic filter. The callback will be used to
 * route any data received on the matching topic to the OTA agent. OTA agent uses this function
 * to subscribe to all topic filters necessary for receiving job related control messages as
 * well as firmware image chunks from MQTT broker.
 *
 * @param[in] pTopicFilter The topic filter used to subscribe for packets.
 * @param[in] topicFilterLength Length of the topic filter string.
 * @param[in] ucQoS Intended qos value for the messages received on this topic.
 * @return OtaMqttSuccess if successful. Appropirate error code otherwise.
 */
extern OtaMqttStatus_t vOTADemoMQTTSubscribe(const char* pTopicFilter,
    uint16_t topicFilterLength,
    uint8_t ucQoS);

/**
 * @brief Function is used by OTA agent to unsubscribe a topicfilter from MQTT broker.
 *
 * The implementation queues an UNSUBSCRIBE request for the topic filter with the MQTT agent. It then waits
 * for a successful completion of the request from the agent. Notification along with results of
 * operation is sent using xTaskNotify API to the caller task. MQTT agent also removes the topic filter
 * subscription from its memory so any future
 * packets on this topic will not be routed to the OTA agent.
 *
 * @param[in] pTopicFilter Topic filter to be unsubscibed.
 * @param[in] topicFilterLength Length of the topic filter.
 * @param[in] ucQos Qos value for the topic.
 * @return OtaMqttSuccess if successful. Appropirate error code otherwise.
 *
 */
extern OtaMqttStatus_t vOTADemoMQTTUnsubscribe(const char* pTopicFilter,
    uint16_t topicFilterLength,
    uint8_t ucQoS);


/**
 * @brief Buffer used to store the firmware image file path.
 * Buffer is passed to the OTA agent during initialization.
 */
static uint8_t updateFilePath[otaexampleMAX_FILE_PATH_SIZE];

/**
 * @brief Buffer used to store the code signing certificate file path.
 * Buffer is passed to the OTA agent during initialization.
 */
static uint8_t certFilePath[otaexampleMAX_FILE_PATH_SIZE];

/**
 * @brief Buffer used to store the name of the data stream.
 * Buffer is passed to the OTA agent during initialization.
 */
static uint8_t streamName[otaexampleMAX_STREAM_NAME_SIZE];

/**
 * @brief Buffer used decode the CBOR message from the MQTT payload.
 * Buffer is passed to the OTA agent during initialization.
 */
static uint8_t decodeMem[(1U << otaconfigLOG2_FILE_BLOCK_SIZE)];

/**
 * @brief Application buffer used to store the bitmap for requesting firmware image
 * chunks from MQTT broker. Buffer is passed to the OTA agent during initialization.
 */
static uint8_t bitmap[OTA_MAX_BLOCK_BITMAP_SIZE];

/**
 * @brief The host address string extracted from the pre-signed URL.
 *
 * @note S3_PRESIGNED_GET_URL_LENGTH is set as the array length here as the
 * length of the host name string cannot exceed this value.
 */
static char serverHost[256];

/**
 * @brief A buffer used in the demo for storing HTTP request headers and
 * HTTP response headers and body.
 *
 * @note This demo shows how the same buffer can be re-used for storing the HTTP
 * response after the HTTP request is sent out. However, the user can also
 * decide to use separate buffers for storing the HTTP request and response.
 */
static uint8_t httpUserBuffer[HTTP_USER_BUFFER_LENGTH];

/**
 * @brief Network connection context used for HTTP connection.
 */
static NetworkContext_t networkContextHttp;

/* The transport layer interface used by the HTTP Client library. */
static TransportInterface_t transportInterfaceHttp;

/**
 * @brief The location of the path within the pre-signed URL.
 */
static const char* pPath;

/**
 * @brief The length of the host address found in the pre-signed URL.
 */
static size_t serverHostLength;

/**
 * @brief The buffer passed to the OTA Agent from application while initializing.
 */
static OtaAppBuffer_t otaBuffer =
{
    .pUpdateFilePath = updateFilePath,
    .updateFilePathsize = OTA_MAX_FILE_PATH_SIZE,
    .pCertFilePath = certFilePath,
    .certFilePathSize = OTA_MAX_FILE_PATH_SIZE,
    .pDecodeMemory = decodeMem,
    .decodeMemorySize = otaconfigFILE_BLOCK_SIZE,
    .pFileBitmap = bitmap,
    .fileBitmapSize = OTA_MAX_BLOCK_BITMAP_SIZE,
    .pUrl = updateUrl,
    .urlSize = OTA_MAX_URL_SIZE,
    .pAuthScheme = authScheme,
    .authSchemeSize = OTA_MAX_AUTH_SCHEME_SIZE
};


/**
 * @brief Structure used for encoding firmware version.
 */
const AppVersion32_t appFirmwareVersion =
{
    .u.x.major = APP_VERSION_MAJOR,
    .u.x.minor = APP_VERSION_MINOR,
    .u.x.build = APP_VERSION_BUILD,
};

/*-----------------------------------------------------------*/
static void prvOTAAgentTask(void* pvParam)
{
    OTA_EventProcessingTask(pvParam);
    vTaskDelete(NULL);
}

/*-----------------------------------------------------------*/

/**
 * @brief The OTA agent has completed the update job or it is in
 * self test mode. If it was accepted, we want to activate the new image.
 * This typically means we should reset the device to run the new firmware.
 * If now is not a good time to reset the device, it may be activated later
 * by your user code. If the update was rejected, just return without doing
 * anything and we will wait for another job. If it reported that we should
 * start test mode, normally we would perform some kind of system checks to
 * make sure our new firmware does the basic things we think it should do
 * but we will just go ahead and set the image as accepted for demo purposes.
 * The accept function varies depending on your platform. Refer to the OTA
 * PAL implementation for your platform in aws_ota_pal.c to see what it
 * does for you.
 *
 * @param[in] event Specify if this demo is running with the AWS IoT
 * MQTT server. Set this to `false` if using another MQTT server.
 * @param[in] pData Data associated with the event.
 * @return None.
 */
static void otaAppCallback(OtaJobEvent_t event,
    const void* pData)
{
    OtaErr_t err = OtaErrUninitialized;

    switch (event)
    {
    case OtaJobEventActivate:
        LogInfo(("Received OtaJobEventActivate callback from OTA Agent."));

        /**
         * Activate the new firmware image immediately. Applications can choose to postpone
         * the activation to a later stage if needed.
         */
        err = OTA_ActivateNewImage();

        /**
         * Activation of the new image failed. This indicates an error that requires a follow
         * up through manual activation by resetting the device. The demo reports the error
         * and shuts down the OTA agent.
         */
        LogError(("New image activation failed."));

        /* Shutdown OTA Agent, if it is required that the unsubscribe operations are not
         * performed while shutting down please set the second parameter to 0 instead of 1. */
        OTA_Shutdown(0, 1);


        break;

    case OtaJobEventFail:

        /**
         * No user action is needed here. OTA agent handles the job failure event.
         */
        LogInfo(("Received an OtaJobEventFail notification from OTA Agent."));

        break;

    case OtaJobEventStartTest:

        /* This demo just accepts the image since it was a good OTA update and networking
         * and services are all working (or we would not have made it this far). If this
         * were some custom device that wants to test other things before validating new
         * image, this would be the place to kick off those tests before calling
         * OTA_SetImageState() with the final result of either accepted or rejected. */

        LogInfo(("Received OtaJobEventStartTest callback from OTA Agent."));

        err = OTA_SetImageState(OtaImageStateAccepted);

        if (err == OtaErrNone)
        {
            LogInfo(("New image validation succeeded in self test mode."));
        }
        else
        {
            LogError(("Failed to set image state as accepted with error %d.", err));
        }

        break;

    case OtaJobEventProcessed:

        LogDebug(("OTA Event processing completed. Freeing the event buffer to pool."));
        configASSERT(pData != NULL);
        vOTADemoEventBufferFree((OtaEventData_t*)pData);

        break;

    case OtaJobEventSelfTestFailed:
        LogDebug(("Received OtaJobEventSelfTestFailed callback from OTA Agent."));

        /* Requires manual activation of previous image as self-test for
         * new image downloaded failed.*/
        LogError(("OTA Self-test failed for new image. shutting down OTA Agent."));

        /* Shutdown OTA Agent, if it is required that the unsubscribe operations are not
         * performed while shutting down please set the second parameter to 0 instead of 1. */
        OTA_Shutdown(0, 1);

        break;

    default:
        LogWarn(("Received an unhandled callback event from OTA Agent, event = %d", event));

        break;
    }
}

static OtaHttpStatus_t httpInit(char* pUrl)
{
    /* OTA lib return error code. */
    OtaHttpStatus_t ret = OtaHttpSuccess;

    /* HTTPS Client library return status. */
    HTTPStatus_t httpStatus = HTTPSuccess;

    /* Return value from libraries. */
    int32_t returnStatus = EXIT_SUCCESS;

    /* The length of the path within the pre-signed URL. This variable is
     * defined in order to store the length returned from parsing the URL, but
     * it is unused. The path used for the requests in this demo needs all the
     * query information following the location of the object, to the end of the
     * S3 presigned URL. */
    size_t pathLen = 0;

    /* Establish HTTPs connection */
    LogInfo(("Performing TLS handshake on top of the TCP connection."));

    /* Attempt to connect to the HTTPs server. If connection fails, retry after
     * a timeout. Timeout value will be exponentially increased till the maximum
     * attempts are reached or maximum timeout value is reached. The function
     * returns EXIT_FAILURE if the TCP connection cannot be established to
     * broker after configured number of attempts. */
    returnStatus = connectToS3Server(&networkContextHttp, pUrl);

    if (returnStatus == EXIT_SUCCESS)
    {
        /* Define the transport interface. */
        (void)memset(&transportInterfaceHttp, 0, sizeof(transportInterfaceHttp));
        transportInterfaceHttp.recv = TLS_FreeRTOS_recv;
        transportInterfaceHttp.send = TLS_FreeRTOS_send;
        transportInterfaceHttp.pNetworkContext = &networkContextHttp;

        /* Retrieve the path location from url. This
         * function returns the length of the path without the query into
         * pathLen, which is left unused in this demo. */
        httpStatus = getUrlPath(pUrl,
            strlen(pUrl),
            &pPath,
            &pathLen);

        ret = (httpStatus == HTTPSuccess) ? OtaHttpSuccess : OtaHttpInitFailed;
    }
    else
    {
        /* Log an error to indicate connection failure after all
         * reconnect attempts are over. */
        LogError(("Failed to connect to HTTP server %s.",
            serverHost));

        ret = OtaHttpInitFailed;
    }

    return ret;
}

static OtaHttpStatus_t httpRequest(uint32_t rangeStart,
    uint32_t rangeEnd)
{
    /* OTA lib return error code. */
    OtaHttpStatus_t ret = OtaHttpSuccess;

    /* Configurations of the initial request headers that are passed to
     * #HTTPClient_InitializeRequestHeaders. */
    HTTPRequestInfo_t requestInfo;
    /* Represents a response returned from an HTTP server. */
    HTTPResponse_t response;
    /* Represents header data that will be sent in an HTTP request. */
    HTTPRequestHeaders_t requestHeaders;

    /* Return value of all methods from the HTTP Client library API. */
    HTTPStatus_t httpStatus = HTTPSuccess;

    /* Reconnection required flag. */
    bool reconnectRequired = false;

    /* Initialize all HTTP Client library API structs to 0. */
    (void)memset(&requestInfo, 0, sizeof(requestInfo));
    (void)memset(&response, 0, sizeof(response));
    (void)memset(&requestHeaders, 0, sizeof(requestHeaders));

    /* Initialize the request object. */
    requestInfo.pHost = serverHost;
    requestInfo.hostLen = serverHostLength;
    requestInfo.pMethod = HTTP_METHOD_GET;
    requestInfo.methodLen = sizeof(HTTP_METHOD_GET) - 1;
    requestInfo.pPath = pPath;
    requestInfo.pathLen = strlen(pPath);

    /* Set "Connection" HTTP header to "keep-alive" so that multiple requests
     * can be sent over the same established TCP connection. */
    requestInfo.reqFlags = HTTP_REQUEST_KEEP_ALIVE_FLAG;

    /* Set the buffer used for storing request headers. */
    requestHeaders.pBuffer = httpUserBuffer;
    requestHeaders.bufferLen = HTTP_USER_BUFFER_LENGTH;

    httpStatus = HTTPClient_InitializeRequestHeaders(&requestHeaders,
        &requestInfo);

    HTTPClient_AddRangeHeader(&requestHeaders, rangeStart, rangeEnd);

    if (httpStatus == HTTPSuccess)
    {
        /* Initialize the response object. The same buffer used for storing
         * request headers is reused here. */
        response.pBuffer = httpUserBuffer;
        response.bufferLen = HTTP_USER_BUFFER_LENGTH;

        /* Send the request and receive the response. */
        httpStatus = HTTPClient_Send(&transportInterfaceHttp,
            &requestHeaders,
            NULL,
            0,
            &response,
            0);
    }
    else
    {
        LogError(("Failed to initialize HTTP request headers: Error=%s.",
            HTTPClient_strerror(httpStatus)));
    }

    if (httpStatus != HTTPSuccess)
    {
        if ((httpStatus == HTTPNoResponse) || (httpStatus == HTTPNetworkError))
        {
            reconnectRequired = true;
        }
        else
        {
            LogError(("HTTPClient_Send failed: Error=%s.",
                HTTPClient_strerror(httpStatus)));

            ret = OtaHttpRequestFailed;
        }
    }
    else
    {
        /* Check if reconnection required. */
        if (response.respFlags & HTTP_RESPONSE_CONNECTION_CLOSE_FLAG)
        {
            reconnectRequired = true;
        }

        /* Handle the http response received. */
        ret = handleHttpResponse(&response);
    }

    if (reconnectRequired == true)
    {
        /* End TLS session, then close TCP connection. */
        (void)SecureSocketsTransport_Disconnect(&networkContextHttp);

        /* Try establishing connection to S3 server again. */
        if (connectToS3Server(&networkContextHttp, NULL) == EXIT_SUCCESS)
        {
            ret = HTTPSuccess;
        }
        else
        {
            /* Log an error to indicate connection failure after all
             * reconnect attempts are over. */
            LogError(("Failed to connect to HTTP server %s.",
                serverHost));

            ret = OtaHttpRequestFailed;
        }
    }

    return ret;
}

/*-----------------------------------------------------------*/

static OtaHttpStatus_t httpDeinit(void)
{
    OtaHttpStatus_t ret = OtaHttpSuccess;

    /* Nothing special to do here .*/

    return ret;
}

/*-----------------------------------------------------------*/

static OtaHttpStatus_t handleHttpResponse(const HTTPResponse_t* pResponse)
{
    /* Return error code. */
    OtaHttpStatus_t ret = OtaHttpRequestFailed;

    OtaEventData_t* pData;
    OtaEventMsg_t eventMsg = { 0 };

    switch (pResponse->statusCode)
    {
    case HTTP_RESPONSE_PARTIAL_CONTENT:
        /* Get buffer to send event & data. */
        pData = vOTADemoEventBufferGet(pResponse->bodyLen);

        if (pData != NULL)
        {
            /* Get the data from response buffer. */
            memcpy(pData->data, pResponse->pBody, pResponse->bodyLen);
            pData->dataLength = pResponse->bodyLen;

            /* Send job document received event. */
            eventMsg.eventId = OtaAgentEventReceivedFileBlock;
            eventMsg.pEventData = pData;
            OTA_SignalEvent(&eventMsg);

            ret = OtaHttpSuccess;
        }
        else
        {
            LogError(("Error: No OTA data buffers available."));

            ret = OtaHttpRequestFailed;
        }

        break;

    case HTTP_RESPONSE_BAD_REQUEST:
    case HTTP_RESPONSE_FORBIDDEN:
    case HTTP_RESPONSE_NOT_FOUND:
        /* Request the job document to get new url. */
        eventMsg.eventId = OtaAgentEventRequestJobDocument;
        eventMsg.pEventData = NULL;
        OTA_SignalEvent(&eventMsg);

        ret = OtaHttpSuccess;
        break;

    default:
        LogError(("Unhandled http response code: =%d.",
            pResponse->statusCode));

        ret = OtaHttpRequestFailed;
    }

    return ret;
}


/*-----------------------------------------------------------*/

static void setOtaInterfaces(OtaInterfaces_t* pOtaInterfaces)
{
    configASSERT(pOtaInterfaces != NULL);

    /* Initialize OTA library OS Interface. */
    pOtaInterfaces->os.event.init = OtaInitEvent_FreeRTOS;
    pOtaInterfaces->os.event.send = OtaSendEvent_FreeRTOS;
    pOtaInterfaces->os.event.recv = OtaReceiveEvent_FreeRTOS;
    pOtaInterfaces->os.event.deinit = OtaDeinitEvent_FreeRTOS;
    pOtaInterfaces->os.timer.start = OtaStartTimer_FreeRTOS;
    pOtaInterfaces->os.timer.stop = OtaStopTimer_FreeRTOS;
    pOtaInterfaces->os.timer.delete = OtaDeleteTimer_FreeRTOS;
    pOtaInterfaces->os.mem.malloc = Malloc_FreeRTOS;
    pOtaInterfaces->os.mem.free = Free_FreeRTOS;

    /* Initialize the OTA library MQTT Interface.*/
    pOtaInterfaces->mqtt.subscribe = prvMQTTSubscribe;
    pOtaInterfaces->mqtt.publish = prvMQTTPublish;
    pOtaInterfaces->mqtt.unsubscribe = prvMQTTUnsubscribe;

    /* Initialize the OTA library PAL Interface.*/
    pOtaInterfaces->pal.getPlatformImageState = otaPal_GetPlatformImageState;
    pOtaInterfaces->pal.setPlatformImageState = otaPal_SetPlatformImageState;
    pOtaInterfaces->pal.writeBlock = otaPal_WriteBlock;
    pOtaInterfaces->pal.activate = otaPal_ActivateNewImage;
    pOtaInterfaces->pal.closeFile = otaPal_CloseFile;
    pOtaInterfaces->pal.reset = otaPal_ResetDevice;
    pOtaInterfaces->pal.abort = otaPal_Abort;
    pOtaInterfaces->pal.createFile = otaPal_CreateFileForRx;
}

static void prvOTAHttpDemoTask(void* pvParam)
{
    (void)pvParam;
    /* FreeRTOS APIs return status. */
    BaseType_t xResult = pdPASS;

    /* OTA library return status. */
    OtaErr_t otaRet = OtaErrNone;

    /* OTA event message used for sending event to OTA Agent.*/
    OtaEventMsg_t eventMsg = { 0 };

    /* OTA interface context required for library interface functions.*/
    OtaInterfaces_t otaInterfaces;

    /* OTA library packet statistics per job.*/
    OtaAgentStatistics_t otaStatistics = { 0 };

    /* OTA Agent state returned from calling OTA_GetAgentState.*/
    OtaState_t state = OtaAgentStateStopped;

    /* Set OTA Library interfaces.*/
    setOtaInterfaces(&otaInterfaces);

    LogInfo(("OTA over HTTP demo, Application version %u.%u.%u",
        appFirmwareVersion.u.x.major,
        appFirmwareVersion.u.x.minor,
        appFirmwareVersion.u.x.build));
    /****************************** Init OTA Library. ******************************/


    if (xResult == pdPASS)
    {
        if ((otaRet = OTA_Init(&otaBuffer,
            &otaInterfaces,
            (const uint8_t*)(democonfigCLIENT_IDENTIFIER),
            otaAppCallback)) != OtaErrNone)
        {
            LogError(("Failed to initialize OTA Agent, exiting = %u.",
                otaRet));
            xResult = pdFAIL;
        }
    }

    if (xResult == pdPASS)
    {
        if ((xResult = xTaskCreate(prvOTAAgentTask,
            "OTAAgentTask",
            otaexampleAGENT_TASK_STACK_SIZE,
            NULL,
            otaexampleAGENT_TASK_PRIORITY,
            NULL)) != pdPASS)
        {
            LogError(("Failed to start OTA Agent task: "
                ",errno=%d",
                xResult));
        }
    }

    /***************************Start OTA demo loop. ******************************/

    if (xResult == pdPASS)
    {
        /* Start the OTA Agent.*/
        eventMsg.eventId = OtaAgentEventStart;
        OTA_SignalEvent(&eventMsg);

        while (((state = OTA_GetState()) != OtaAgentStateStopped))
        {
            /* Get OTA statistics for currently executing job. */
            OTA_GetStatistics(&otaStatistics);
            LogInfo((" Received: %u   Queued: %u   Processed: %u   Dropped: %u",
                otaStatistics.otaPacketsReceived,
                otaStatistics.otaPacketsQueued,
                otaStatistics.otaPacketsProcessed,
                otaStatistics.otaPacketsDropped));

            vTaskDelay(pdMS_TO_TICKS(otaexampleTASK_DELAY_MS));
        }
    }

    LogInfo(("OTA agent task stopped. Exiting OTA demo."));

    vTaskDelete(NULL);
}

void vStartOTAHTTPCodeSigningTask(configSTACK_DEPTH_TYPE uxStackSize,
    UBaseType_t uxPriority)
{
    BaseType_t xResult;

    if ((xResult = xTaskCreate(prvOTAHttpDemoTask,
        "OTAHTTPDemoTask",
        uxStackSize,
        NULL,
        uxPriority,
        NULL)) != pdPASS)
    {
        LogError(("Failed to start OTA task: "
            ",errno=%d",
            xResult));
    }

    configASSERT(xResult == pdPASS);
}