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
  * @file ota_demo_mqtt_interface.c
  * @brief This file contains OTA MQTT interface implementation using MQTT agent APIs. OTA HTTP demo uses this implementation
  * to set the control interface for OTA agent. OTA MQTT demo uses this implementation to set both control and 
  * data interface for the OTA agent.
  * 
  * See https://freertos.org/ota/ota-mqtt-agent-demo.html
  */

#include <stdbool.h>
#include <stdint.h>

#include "FreeRTOS.h"

#include "task.h"

#include "demo_config.h"

#include "freertos_mqtt_agent.h"

#include "core_mqtt.h"

#include "ota.h"

#include "ota_event_buffer.h"

  /**
   * @brief The common prefix for all OTA topics.
   *
   * Thing name is substituted with a wildcard symbol `+`. OTA agent
   * registers with MQTT broker with the thing name in the topic. This topic
   * filter is used to match incoming packet received and route them to OTA.
   * Thing name is not needed for this matching.
   */
#define OTA_TOPIC_PREFIX                                 "$aws/things/+/"

   /**
    * @brief Wildcard topic filter for job notification.
    * The filter is used to match the constructed job notify topic filter from OTA agent and register
    * appropirate callback for it.
    */
#define OTA_JOB_NOTIFY_TOPIC_FILTER                      OTA_TOPIC_PREFIX "jobs/notify-next"

    /**
     * @brief Length of job notification topic filter.
     */
#define OTA_JOB_NOTIFY_TOPIC_FILTER_LENGTH               ( ( uint16_t ) ( sizeof( OTA_JOB_NOTIFY_TOPIC_FILTER ) - 1 ) )

     /**
      * @brief Wildcard topic filter for matching job response messages.
      * This topic filter is used to match the responses from OTA service for OTA agent job requests. THe
      * topic filter is a reserved topic which is not subscribed with MQTT broker.
      *
      */
#define OTA_JOB_ACCEPTED_RESPONSE_TOPIC_FILTER           OTA_TOPIC_PREFIX "jobs/$next/get/accepted"

      /**
       * @brief Length of job accepted response topic filter.
       */
#define OTA_JOB_ACCEPTED_RESPONSE_TOPIC_FILTER_LENGTH    ( ( uint16_t ) ( sizeof( OTA_JOB_ACCEPTED_RESPONSE_TOPIC_FILTER ) - 1 ) )


       /**
        * @brief Wildcard topic filter for matching OTA data packets.
        *  The filter is used to match the constructed data stream topic filter from OTA agent and register
        * appropirate callback for it.
        */
#define OTA_DATA_STREAM_TOPIC_FILTER           OTA_TOPIC_PREFIX  "streams/#"

        /**
         * @brief Length of data stream topic filter.
         */
#define OTA_DATA_STREAM_TOPIC_FILTER_LENGTH    ( ( uint16_t ) ( sizeof( OTA_DATA_STREAM_TOPIC_FILTER ) - 1 ) )


         /**
          * @brief Starting index of client identifier within OTA topic.
          */
#define OTA_TOPIC_CLIENT_IDENTIFIER_START_IDX    ( 12U )



          /**
           * @brief The maximum time for which OTA demo waits for an MQTT operation to be complete.
           * This involves receiving an acknowledgment for broker for SUBSCRIBE, UNSUBSCRIBE and non
           * QOS0 publishes.
           */
#define otaexampleMQTT_TIMEOUT_MS                        ( 5000U )



           /**
            * @brief Used to clear bits in a task's notification value.
            */
#define otaexampleMAX_UINT32                     ( 0xffffffff )


           /**
            * @brief Defines the structure to use as the command callback context in this
            * demo.
            */
struct CommandContext
{
    MQTTStatus_t xReturnStatus;
    TaskHandle_t xTaskToNotify;
    void* pArgs;
};


  /**
   * @brief Matches a client identifier within an OTA topic.
   * This function is used to validate that topic is valid and intended for this device thing name.
   *
   * @param[in] pTopic Pointer to the topic
   * @param[in] topicNameLength length of the topic
   * @param[in] pClientIdentifier Client identifier, should be null terminated.
   * @param[in] clientIdentifierLength Length of the client identifier.
   * @return true if client identifier is found within the topic at the right index.
   */
static bool prvMatchClientIdentifierInTopic(const char* pTopic,
    size_t topicNameLength,
    const char* pClientIdentifier,
    size_t clientIdentifierLength);

/**
 * @brief Callback invoked for firmware image chunks received from MQTT broker.
 *
 * Function gets invoked for the firmware image blocks received on OTA data stream topic.
 * The function is registered with MQTT agent's subscription manger along with the
 * topic filter for data stream. For each packet received, the
 * function fetches a free event buffer from the pool and queues the firmware image chunk for
 * OTA agent task processing.
 *
 * @param[in] pxSubscriptionContext Context which is passed unmodified from the MQTT agent.
 * @param[in] pPublishInfo Pointer to the structure containing the details of the MQTT packet.
 */
static void prvProcessIncomingData(void* pxSubscriptionContext,
    MQTTPublishInfo_t* pPublishInfo);

/**
 * @brief Callback invoked for job control messages from MQTT broker.
 *
 * Callback gets invoked for any OTA job related control messages from the MQTT broker.
 * The function is registered with MQTT agent's subscription manger along with the topic filter for
 * job stream. The function fetches a free event buffer from the pool and queues the appropirate event type
 * based on the control message received.
 *
 * @param[in] pxSubscriptionContext Context which is passed unmodified from the MQTT agent.
 * @param[in] pPublishInfo Pointer to the structure containing the details of MQTT packet.
 */
static void prvProcessIncomingJobMessage(void* pxSubscriptionContext,
    MQTTPublishInfo_t* pPublishInfo);

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
OtaMqttStatus_t vOTADemoMQTTPublish(const char* const pacTopic,
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
OtaMqttStatus_t vOTADemoMQTTSubscribe(const char* pTopicFilter,
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
OtaMqttStatus_t vOTADemoMQTTUnsubscribe(const char* pTopicFilter,
    uint16_t topicFilterLength,
    uint8_t ucQoS);

  /**
   * @brief Handler used to process MQTT messages for OTA agent.
   *
   * This handler shall be invoked from the MQTT event callback to process any incoming MQTT packets 
   * for OTA agent. The function filters only MQTT packets for OTA agent using predefined OTA topics
   * and returns false if there is no match found.
   *
   * @param[in] pvIncomingPublishCallbackContext MQTT context which stores the connection.
   * @param[in] pPublishInfo MQTT packet that stores the information of the file block.
   *
   * @return true if the message is processed by OTA.
   */
bool vOTADemoHandleMQTTPacket(void* pvIncomingPublishCallbackContext,
    MQTTPublishInfo_t* pxPublishInfo);


/**
 * @brief Static handle used for MQTT agent context.
 */
extern MQTTAgentContext_t xGlobalMqttAgentContext;


/*-----------------------------------------------------------*/

static bool prvMatchClientIdentifierInTopic(const char* pTopic,
    size_t topicNameLength,
    const char* pClientIdentifier,
    size_t clientIdentifierLength)
{
    bool isMatch = false;
    size_t idx, matchIdx = 0;

    for (idx = OTA_TOPIC_CLIENT_IDENTIFIER_START_IDX; idx < topicNameLength; idx++)
    {
        if (matchIdx == clientIdentifierLength)
        {
            if (pTopic[idx] == '/')
            {
                isMatch = true;
            }

            break;
        }
        else
        {
            if (pClientIdentifier[matchIdx] != pTopic[idx])
            {
                break;
            }
        }

        matchIdx++;
    }

    return isMatch;
}


/*-----------------------------------------------------------*/

static void prvProcessIncomingData(void* pxSubscriptionContext,
    MQTTPublishInfo_t* pPublishInfo)
{
    configASSERT(pPublishInfo != NULL);

    (void)pxSubscriptionContext;

    OtaEventData_t* pData;
    OtaEventMsg_t eventMsg = { 0 };

    LogDebug(("Received OTA image block, size %d.\n\n", pPublishInfo->payloadLength));

    configASSERT(pPublishInfo->payloadLength <= OTA_DATA_BLOCK_SIZE);

    pData = prvOTAEventBufferGet();

    if (pData != NULL)
    {
        memcpy(pData->data, pPublishInfo->pPayload, pPublishInfo->payloadLength);
        pData->dataLength = pPublishInfo->payloadLength;
        eventMsg.eventId = OtaAgentEventReceivedFileBlock;
        eventMsg.pEventData = pData;

        /* Send job document received event. */
        OTA_SignalEvent(&eventMsg);
    }
    else
    {
        LogError(("Error: No OTA data buffers available.\r\n"));
    }
}

/*-----------------------------------------------------------*/

static void prvProcessIncomingJobMessage(void* pxSubscriptionContext,
    MQTTPublishInfo_t* pPublishInfo)
{
    OtaEventData_t* pData;
    OtaEventMsg_t eventMsg = { 0 };

    (void)pxSubscriptionContext;

    configASSERT(pPublishInfo != NULL);

    LogInfo(("Received job message callback, size %d.\n\n", pPublishInfo->payloadLength));

    configASSERT(pPublishInfo->payloadLength <= OTA_DATA_BLOCK_SIZE);

    pData = prvOTAEventBufferGet();

    if (pData != NULL)
    {
        memcpy(pData->data, pPublishInfo->pPayload, pPublishInfo->payloadLength);
        pData->dataLength = pPublishInfo->payloadLength;
        eventMsg.eventId = OtaAgentEventReceivedJobDocument;
        eventMsg.pEventData = pData;

        /* Send job document received event. */
        OTA_SignalEvent(&eventMsg);
    }
    else
    {
        LogError(("Error: No OTA data buffers available.\r\n"));
    }
}


/*-----------------------------------------------------------*/

static void prvCommandCallback(CommandContext_t* pCommandContext,
    MQTTAgentReturnInfo_t* pxReturnInfo)
{
    pCommandContext->xReturnStatus = pxReturnInfo->returnCode;

    if (pCommandContext->xTaskToNotify != NULL)
    {
        xTaskNotify(pCommandContext->xTaskToNotify, (uint32_t)(pxReturnInfo->returnCode), eSetValueWithOverwrite);
    }
}
/*-----------------------------------------------------------*/

OtaMqttStatus_t vOTADemoMQTTSubscribe(const char* pTopicFilter,
    uint16_t topicFilterLength,
    uint8_t ucQoS)
{
    MQTTStatus_t mqttStatus;
    uint32_t ulNotifiedValue;
    MQTTAgentSubscribeArgs_t xSubscribeArgs = { 0 };
    MQTTSubscribeInfo_t xSubscribeInfo = { 0 };
    BaseType_t result;
    CommandInfo_t xCommandParams = { 0 };
    CommandContext_t xApplicationDefinedContext = { 0 };
    OtaMqttStatus_t otaRet = OtaMqttSuccess;

    configASSERT(pTopicFilter != NULL);
    configASSERT(topicFilterLength > 0);

    xSubscribeInfo.pTopicFilter = pTopicFilter;
    xSubscribeInfo.topicFilterLength = topicFilterLength;
    xSubscribeInfo.qos = ucQoS;
    xSubscribeArgs.pSubscribeInfo = &xSubscribeInfo;
    xSubscribeArgs.numSubscriptions = 1;

    xApplicationDefinedContext.xTaskToNotify = xTaskGetCurrentTaskHandle();

    xCommandParams.blockTimeMs = otaexampleMQTT_TIMEOUT_MS;
    xCommandParams.cmdCompleteCallback = prvCommandCallback;
    xCommandParams.pCmdCompleteCallbackContext = (void*)&xApplicationDefinedContext;

    xTaskNotifyStateClear(NULL);

    mqttStatus = MQTTAgent_Subscribe(&xGlobalMqttAgentContext,
        &xSubscribeArgs,
        &xCommandParams);

    /* Wait for command to complete so MQTTSubscribeInfo_t remains in scope for the
     * duration of the command. */
    if (mqttStatus == MQTTSuccess)
    {
        result = xTaskNotifyWait(0, otaexampleMAX_UINT32, &ulNotifiedValue, pdMS_TO_TICKS(otaexampleMQTT_TIMEOUT_MS));

        if (result == pdTRUE)
        {
            mqttStatus = xApplicationDefinedContext.xReturnStatus;
        }
        else
        {
            mqttStatus = MQTTRecvFailed;
        }
    }

    if (mqttStatus != MQTTSuccess)
    {
        LogError(("Failed to SUBSCRIBE to topic with error = %u.",
            mqttStatus));

        otaRet = OtaMqttSubscribeFailed;
    }
    else
    {
        LogInfo(("Subscribed to topic %.*s.\n\n",
            topicFilterLength,
            pTopicFilter));

        otaRet = OtaMqttSuccess;
    }

    return otaRet;
}

OtaMqttStatus_t vOTADemoMQTTPublish(const char* const pacTopic,
    uint16_t topicLen,
    const char* pMsg,
    uint32_t msgSize,
    uint8_t qos)
{
    OtaMqttStatus_t otaRet = OtaMqttSuccess;
    BaseType_t result;
    MQTTStatus_t mqttStatus = MQTTBadParameter;
    MQTTPublishInfo_t publishInfo = { 0 };
    CommandInfo_t xCommandParams = { 0 };
    CommandContext_t xCommandContext = { 0 };

    publishInfo.pTopicName = pacTopic;
    publishInfo.topicNameLength = topicLen;
    publishInfo.qos = qos;
    publishInfo.pPayload = pMsg;
    publishInfo.payloadLength = msgSize;

    xCommandContext.xTaskToNotify = xTaskGetCurrentTaskHandle();
    xTaskNotifyStateClear(NULL);

    xCommandParams.blockTimeMs = otaexampleMQTT_TIMEOUT_MS;
    xCommandParams.cmdCompleteCallback = prvCommandCallback;
    xCommandParams.pCmdCompleteCallbackContext = (void*)&xCommandContext;

    mqttStatus = MQTTAgent_Publish(&xGlobalMqttAgentContext,
        &publishInfo,
        &xCommandParams);

    /* Wait for command to complete so MQTTSubscribeInfo_t remains in scope for the
     * duration of the command. */
    if (mqttStatus == MQTTSuccess)
    {
        result = xTaskNotifyWait(0, otaexampleMAX_UINT32, NULL, pdMS_TO_TICKS(otaexampleMQTT_TIMEOUT_MS));

        if (result != pdTRUE)
        {
            mqttStatus = MQTTSendFailed;
        }
        else
        {
            mqttStatus = xCommandContext.xReturnStatus;
        }
    }

    if (mqttStatus != MQTTSuccess)
    {
        LogError(("Failed to send PUBLISH packet to broker with error = %u.", mqttStatus));
        otaRet = OtaMqttPublishFailed;
    }
    else
    {
        LogInfo(("Sent PUBLISH packet to broker %.*s to broker.\n\n",
            topicLen,
            pacTopic));

        otaRet = OtaMqttSuccess;
    }

    return otaRet;
}

 OtaMqttStatus_t vOTADemoMQTTUnsubscribe(const char* pTopicFilter,
    uint16_t topicFilterLength,
    uint8_t ucQoS)
{
    MQTTStatus_t mqttStatus;
    uint32_t ulNotifiedValue;
    MQTTAgentSubscribeArgs_t xSubscribeArgs = { 0 };
    MQTTSubscribeInfo_t xSubscribeInfo = { 0 };
    BaseType_t result;
    CommandInfo_t xCommandParams = { 0 };
    CommandContext_t xApplicationDefinedContext = { 0 };
    OtaMqttStatus_t otaRet = OtaMqttSuccess;

    configASSERT(pTopicFilter != NULL);
    configASSERT(topicFilterLength > 0);

    xSubscribeInfo.pTopicFilter = pTopicFilter;
    xSubscribeInfo.topicFilterLength = topicFilterLength;
    xSubscribeInfo.qos = ucQoS;
    xSubscribeArgs.pSubscribeInfo = &xSubscribeInfo;
    xSubscribeArgs.numSubscriptions = 1;


    xApplicationDefinedContext.xTaskToNotify = xTaskGetCurrentTaskHandle();

    xCommandParams.blockTimeMs = otaexampleMQTT_TIMEOUT_MS;
    xCommandParams.cmdCompleteCallback = prvCommandCallback;
    xCommandParams.pCmdCompleteCallbackContext = (void*)&xApplicationDefinedContext;

    LogInfo((" Unsubscribing to topic filter: %s", pTopicFilter));
    xTaskNotifyStateClear(NULL);


    mqttStatus = MQTTAgent_Unsubscribe(&xGlobalMqttAgentContext,
        &xSubscribeArgs,
        &xCommandParams);

    /* Wait for command to complete so MQTTSubscribeInfo_t remains in scope for the
     * duration of the command. */
    if (mqttStatus == MQTTSuccess)
    {
        result = xTaskNotifyWait(0, otaexampleMAX_UINT32, &ulNotifiedValue, pdMS_TO_TICKS(otaexampleMQTT_TIMEOUT_MS));

        if (result == pdTRUE)
        {
            mqttStatus = xApplicationDefinedContext.xReturnStatus;
        }
        else
        {
            mqttStatus = MQTTRecvFailed;
        }
    }

    if (mqttStatus != MQTTSuccess)
    {
        LogError(("Failed to UNSUBSCRIBE from topic %.*s with error = %u.",
            topicFilterLength,
            pTopicFilter,
            mqttStatus));

        otaRet = OtaMqttUnsubscribeFailed;
    }
    else
    {
        LogInfo(("UNSUBSCRIBED from topic %.*s.\n\n",
            topicFilterLength,
            pTopicFilter));

        otaRet = OtaMqttSuccess;
    }

    return otaRet;
}

/*-----------------------------------------------------------*/

bool vOTADemoHandleMQTTPacket(void* pvIncomingPublishCallbackContext,
    MQTTPublishInfo_t* pxPublishInfo) {
    bool isMatch = false;

    (void)MQTT_MatchTopic(pxPublishInfo->pTopicName,
        pxPublishInfo->topicNameLength,
        OTA_JOB_ACCEPTED_RESPONSE_TOPIC_FILTER,
        OTA_JOB_ACCEPTED_RESPONSE_TOPIC_FILTER_LENGTH,
        &isMatch);

    if (isMatch == true)
    {
        /* validate thing name */

        isMatch = prvMatchClientIdentifierInTopic(pxPublishInfo->pTopicName,
            pxPublishInfo->topicNameLength,
            democonfigCLIENT_IDENTIFIER,
            strlen(democonfigCLIENT_IDENTIFIER));

        if (isMatch == true)
        {
            prvProcessIncomingJobMessage(pvIncomingPublishCallbackContext, pxPublishInfo);
        }
    }

    if (isMatch == false)
    {
        (void)MQTT_MatchTopic(pxPublishInfo->pTopicName,
            pxPublishInfo->topicNameLength,
            OTA_JOB_NOTIFY_TOPIC_FILTER,
            OTA_JOB_NOTIFY_TOPIC_FILTER_LENGTH,
            &isMatch);

        if (isMatch == true)
        {
            prvProcessIncomingJobMessage(pvIncomingPublishCallbackContext, pxPublishInfo);
        }
    }

    if (isMatch == false)
    {
        (void)MQTT_MatchTopic(pxPublishInfo->pTopicName,
            pxPublishInfo->topicNameLength,
            OTA_DATA_STREAM_TOPIC_FILTER,
            OTA_DATA_STREAM_TOPIC_FILTER_LENGTH,
            &isMatch);

        if (isMatch == true)
        {
            prvProcessIncomingData(pvIncomingPublishCallbackContext, pxPublishInfo);
        }
    }

    return isMatch;
}





