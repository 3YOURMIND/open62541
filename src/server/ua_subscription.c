/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. 
 *
 *    Copyright 2015-2017 (c) Fraunhofer IOSB (Author: Julius Pfrommer)
 *    Copyright 2015 (c) Chris Iatrou
 *    Copyright 2015-2016 (c) Sten Grüner
 *    Copyright 2017-2018 (c) Thomas Stalder, Blue Time Concept SA
 *    Copyright 2015 (c) Joakim L. Gilje
 *    Copyright 2016-2017 (c) Florian Palm
 *    Copyright 2015-2016 (c) Oleksiy Vasylyev
 *    Copyright 2017 (c) frax2222
 *    Copyright 2017 (c) Stefan Profanter, fortiss GmbH
 *    Copyright 2017 (c) Ari Breitkreuz, fortiss GmbH
 *    Copyright 2017 (c) Mattias Bornhager
 */

#include "ua_server_internal.h"
#include "ua_subscription.h"

#ifdef UA_ENABLE_SUBSCRIPTIONS /* conditional compilation */

void UA_Notification_delete(UA_Notification *n) {
    if(n->mon->monitoredItemType == UA_MONITOREDITEMTYPE_CHANGENOTIFY) {
        UA_DataValue_deleteMembers(&n->data.value);
    } else if (n->mon->monitoredItemType == UA_MONITOREDITEMTYPE_EVENTNOTIFY) {
        UA_Array_delete(n->data.event.fields.eventFields, n->data.event.fields.eventFieldsSize,
                        &UA_TYPES[UA_TYPES_VARIANT]);
        /* EventFilterResult currently isn't being used
         * UA_EventFilterResult_delete(notification->data.event->result);
         */
    }
    UA_free(n);
}

UA_Subscription *
UA_Subscription_new(UA_Session *session, UA_UInt32 subscriptionId) {
    /* Allocate the memory */
    UA_Subscription *newSub =
        (UA_Subscription*)UA_calloc(1, sizeof(UA_Subscription));
    if(!newSub)
        return NULL;

    /* Remaining members are covered by calloc zeroing out the memory */
    newSub->session = session;
    newSub->subscriptionId = subscriptionId;
    newSub->state = UA_SUBSCRIPTIONSTATE_NORMAL; /* The first publish response is sent immediately */
    TAILQ_INIT(&newSub->retransmissionQueue);
    TAILQ_INIT(&newSub->notificationQueue);
    return newSub;
}

void
UA_Subscription_deleteMembers(UA_Server *server, UA_Subscription *sub) {
    UA_LOG_DEBUG_SESSION(server->config.logger, sub->session, "Subscription %u | "
                             "Delete the subscription", sub->subscriptionId);

    Subscription_unregisterPublishCallback(server, sub);

    /* Delete monitored Items */
    UA_MonitoredItem *mon, *tmp_mon;
    LIST_FOREACH_SAFE(mon, &sub->monitoredItems, listEntry, tmp_mon) {
        LIST_REMOVE(mon, listEntry);
        UA_MonitoredItem_delete(server, mon);
    }
    sub->monitoredItemsSize = 0;

    /* Delete Retransmission Queue */
    UA_NotificationMessageEntry *nme, *nme_tmp;
    TAILQ_FOREACH_SAFE(nme, &sub->retransmissionQueue, listEntry, nme_tmp) {
        TAILQ_REMOVE(&sub->retransmissionQueue, nme, listEntry);
        UA_NotificationMessage_deleteMembers(&nme->message);
        UA_free(nme);
    }
    sub->retransmissionQueueSize = 0;
}

UA_MonitoredItem *
UA_Subscription_getMonitoredItem(UA_Subscription *sub, UA_UInt32 monitoredItemId) {
    UA_MonitoredItem *mon;
    LIST_FOREACH(mon, &sub->monitoredItems, listEntry) {
        if(mon->monitoredItemId == monitoredItemId)
            break;
    }
    return mon;
}

UA_StatusCode
UA_Subscription_deleteMonitoredItem(UA_Server *server, UA_Subscription *sub,
                                    UA_UInt32 monitoredItemId) {
    /* Find the MonitoredItem */
    UA_MonitoredItem *mon;
    LIST_FOREACH(mon, &sub->monitoredItems, listEntry) {
        if(mon->monitoredItemId == monitoredItemId)
            break;
    }
    if(!mon)
        return UA_STATUSCODE_BADMONITOREDITEMIDINVALID;

    /* Remove the MonitoredItem */
    LIST_REMOVE(mon, listEntry);
    sub->monitoredItemsSize--;

    /* Remove content and delayed free */
    UA_MonitoredItem_delete(server, mon);

    return UA_STATUSCODE_GOOD;
}

void
UA_Subscription_addMonitoredItem(UA_Subscription *sub, UA_MonitoredItem *newMon) {
    sub->monitoredItemsSize++;
    LIST_INSERT_HEAD(&sub->monitoredItems, newMon, listEntry);
}

static void
UA_Subscription_addRetransmissionMessage(UA_Server *server, UA_Subscription *sub,
                                         UA_NotificationMessageEntry *entry) {
    /* Release the oldest entry if there is not enough space */
    if(server->config.maxRetransmissionQueueSize > 0 &&
       sub->retransmissionQueueSize >= server->config.maxRetransmissionQueueSize) {
        UA_NotificationMessageEntry *lastentry =
            TAILQ_LAST(&sub->retransmissionQueue, ListOfNotificationMessages);
        TAILQ_REMOVE(&sub->retransmissionQueue, lastentry, listEntry);
        --sub->retransmissionQueueSize;
        UA_NotificationMessage_deleteMembers(&lastentry->message);
        UA_free(lastentry);
    }

    /* Add entry */
    TAILQ_INSERT_HEAD(&sub->retransmissionQueue, entry, listEntry);
    ++sub->retransmissionQueueSize;
}

UA_StatusCode
UA_Subscription_removeRetransmissionMessage(UA_Subscription *sub, UA_UInt32 sequenceNumber) {
    /* Find the retransmission message */
    UA_NotificationMessageEntry *entry;
    TAILQ_FOREACH(entry, &sub->retransmissionQueue, listEntry) {
        if(entry->message.sequenceNumber == sequenceNumber)
            break;
    }
    if(!entry)
        return UA_STATUSCODE_BADSEQUENCENUMBERUNKNOWN;

    /* Remove the retransmission message */
    TAILQ_REMOVE(&sub->retransmissionQueue, entry, listEntry);
    --sub->retransmissionQueueSize;
    UA_NotificationMessage_deleteMembers(&entry->message);
    UA_free(entry);
    return UA_STATUSCODE_GOOD;
}

#ifdef UA_ENABLE_SUBSCRIPTIONS_EVENTS
/* EventChange: Iterate over the monitoredItems of the subscription, starting at mon, and
 *              move notifications into the response. */
static void
Events_moveNotificationsFromMonitoredItems(UA_Server *server, UA_Subscription *sub, UA_EventFieldList *efls,
                                           size_t eflsSize) {
    UA_StatusCode retval;
    size_t pos = 0;
    UA_Notification *notification, *notification_tmp;
    TAILQ_FOREACH_SAFE(notification, &sub->notificationQueue, globalEntry, notification_tmp) {
        if (pos >= eflsSize) {
            return;
        }
        UA_MonitoredItem *mon = notification->mon;

        /* Remove the notification from the queues */
        TAILQ_REMOVE(&sub->notificationQueue, notification, globalEntry);
        TAILQ_REMOVE(&mon->queue, notification, listEntry);

        /* removing an overflowEvent should not reduce the queueSize */
        UA_NodeId overflowId = UA_NODEID_NUMERIC(0, UA_NS0ID_SIMPLEOVERFLOWEVENTTYPE);
        if (!(notification->data.event.fields.eventFieldsSize == 1
                && notification->data.event.fields.eventFields->type == &UA_TYPES[UA_TYPES_NODEID]
                && UA_NodeId_equal((UA_NodeId *)notification->data.event.fields.eventFields->data, &overflowId))) {
            --mon->queueSize;
            --sub->notificationQueueSize;
        }

        /* Move the content to the response */
        UA_EventFieldList *efl = &efls[pos];
        efl->clientHandle = mon->clientHandle;
        efl->eventFieldsSize = notification->data.event.fields.eventFieldsSize;
        retval = UA_Array_copy(notification->data.event.fields.eventFields,
                               notification->data.event.fields.eventFieldsSize,
                               (void **) &efl->eventFields, &UA_TYPES[UA_TYPES_VARIANT]);
        if (retval != UA_STATUSCODE_GOOD) {
            return;
        }

        /* EventFilterResult currently isn't being used
        UA_EventFilterResult_deleteMembers(&notification->data.event.result); */
        UA_EventFieldList_deleteMembers(&notification->data.event.fields);
        UA_free(notification);
    }
}
#endif

/* DataChange: Iterate over the monitoreditems of the subscription, starting at mon, and
 *             move notifications into the response. */
static void
DataChange_moveNotificationsFromMonitoredItems(UA_Subscription *sub, UA_MonitoredItemNotification *mins,
                                               size_t minsSize) {
    size_t pos = 0;
    UA_Notification *notification, *notification_tmp;
    TAILQ_FOREACH_SAFE(notification, &sub->notificationQueue, globalEntry, notification_tmp) {
        if(pos >= minsSize)
            return;

        UA_MonitoredItem *mon = notification->mon;

        /* Remove the notification from the queues */
        TAILQ_REMOVE(&sub->notificationQueue, notification, globalEntry);
        TAILQ_REMOVE(&mon->queue, notification, listEntry);
        --mon->queueSize;
        --sub->notificationQueueSize;

        /* Move the content to the response */
        UA_MonitoredItemNotification *min = &mins[pos];
        min->clientHandle = mon->clientHandle;
        min->value = notification->data.value;
        UA_free(notification);
        ++pos;
    }
}

static UA_StatusCode
prepareNotificationMessage(UA_Server *server, UA_Subscription *sub, UA_NotificationMessage *message,
                           size_t notifications) {
    /* Array of ExtensionObject to hold different kinds of notifications
     * (currently only DataChangeNotifications) */
    message->notificationData = UA_ExtensionObject_new();
    if(!message->notificationData)
        return UA_STATUSCODE_BADOUTOFMEMORY;
    message->notificationDataSize = 1;

    UA_ExtensionObject *data = message->notificationData;
    data->encoding = UA_EXTENSIONOBJECT_DECODED;
    /* TODO: basing type of notificationtype off of first monitoredItem in subscription which isnt very good */
    /* Allocate Notification */
    if (LIST_FIRST(&sub->monitoredItems)->monitoredItemType == UA_MONITOREDITEMTYPE_CHANGENOTIFY) {
        UA_DataChangeNotification *dcn = UA_DataChangeNotification_new();
        if (!dcn) {
            UA_NotificationMessage_deleteMembers(message);
            return UA_STATUSCODE_BADOUTOFMEMORY;
        }
        data->content.decoded.data = dcn;
        data->content.decoded.type = &UA_TYPES[UA_TYPES_DATACHANGENOTIFICATION];

        /* Allocate array of notifications */
        dcn->monitoredItems = (UA_MonitoredItemNotification *)
                UA_Array_new(notifications,
                             &UA_TYPES[UA_TYPES_MONITOREDITEMNOTIFICATION]);
        if(!dcn->monitoredItems) {
            UA_NotificationMessage_deleteMembers(message);
            return UA_STATUSCODE_BADOUTOFMEMORY;
        }
        dcn->monitoredItemsSize = notifications;

        /* Move notifications into the response .. the point of no return */
        DataChange_moveNotificationsFromMonitoredItems(sub, dcn->monitoredItems, notifications);
    }
#ifdef UA_ENABLE_SUBSCRIPTIONS_EVENTS
    else if (LIST_FIRST(&sub->monitoredItems)->monitoredItemType == UA_MONITOREDITEMTYPE_EVENTNOTIFY) {

        UA_EventNotificationList *enl = UA_EventNotificationList_new();
        if (!enl) {
            UA_NotificationMessage_deleteMembers(message);
            return UA_STATUSCODE_BADOUTOFMEMORY;
        }
        UA_EventNotificationList_init(enl);

        data->content.decoded.data = enl;
        data->content.decoded.type = &UA_TYPES[UA_TYPES_EVENTNOTIFICATIONLIST];

        /* Allocate array of notifications */
        enl->events = (UA_EventFieldList *) UA_Array_new(notifications, &UA_TYPES[UA_TYPES_EVENTFIELDLIST]);
        if (!enl->events) {
            UA_NotificationMessage_deleteMembers(message);
            return UA_STATUSCODE_BADOUTOFMEMORY;
        }
        enl->eventsSize = notifications;

        /* Move the list into the response .. the point of no return */
        Events_moveNotificationsFromMonitoredItems(server, sub, enl->events, notifications);
    }
#endif
    else {
        return UA_STATUSCODE_BADNOTIMPLEMENTED;
    }
    return UA_STATUSCODE_GOOD;
}

/* According to OPC Unified Architecture, Part 4 5.13.1.1 i) The value 0 is
 * never used for the sequence number */
static UA_UInt32
UA_Subscription_nextSequenceNumber(UA_UInt32 sequenceNumber) {
    UA_UInt32 nextSequenceNumber = sequenceNumber + 1;
    if(nextSequenceNumber == 0)
        nextSequenceNumber = 1;
    return nextSequenceNumber;
}

static void
publishCallback(UA_Server *server, UA_Subscription *sub) {
    sub->readyNotifications = sub->notificationQueueSize;
    UA_Subscription_publish(server, sub);
}

void
UA_Subscription_publish(UA_Server *server, UA_Subscription *sub) {
    UA_LOG_DEBUG_SESSION(server->config.logger, sub->session, "Subscription %u | "
                         "Publish Callback", sub->subscriptionId);
    /* Dequeue a response */
    UA_PublishResponseEntry *pre = UA_Session_dequeuePublishReq(sub->session);
    if(pre) {
        sub->currentLifetimeCount = 0; /* Reset the LifetimeCounter */
    } else {
        UA_LOG_DEBUG_SESSION(server->config.logger, sub->session,
                             "Subscription %u | The publish queue is empty",
                             sub->subscriptionId);
        ++sub->currentLifetimeCount;

        if(sub->currentLifetimeCount > sub->lifeTimeCount) {
            UA_LOG_DEBUG_SESSION(server->config.logger, sub->session,
                                 "Subscription %u | End of lifetime "
                                 "for subscription", sub->subscriptionId);
            UA_Session_deleteSubscription(server, sub->session, sub->subscriptionId);
            /* TODO: send a StatusChangeNotification with Bad_Timeout */
            return;
        }
    }

    if (sub->readyNotifications > sub->notificationQueueSize)
        sub->readyNotifications = sub->notificationQueueSize;

    /* Count the available notifications */
    UA_UInt32 notifications = sub->readyNotifications;
    if(!sub->publishingEnabled)
        notifications = 0;

    UA_Boolean moreNotifications = false;
    if(notifications > sub->notificationsPerPublish) {
        notifications = sub->notificationsPerPublish;
        moreNotifications = true;
    }

    /* Return if no notifications and no keepalive */
    if(notifications == 0) {
        ++sub->currentKeepAliveCount;
        if(sub->currentKeepAliveCount < sub->maxKeepAliveCount) {
            if(pre)
                UA_Session_queuePublishReq(sub->session, pre, true); /* Re-enqueue */
            return;
        }
        UA_LOG_DEBUG_SESSION(server->config.logger, sub->session,
                             "Subscription %u | Sending a KeepAlive",
                             sub->subscriptionId);
    }

    /* We want to send a response. Is it possible? */
    UA_SecureChannel *channel = sub->session->header.channel;
    if(!channel || !pre) {
        UA_LOG_DEBUG_SESSION(server->config.logger, sub->session,
                             "Subscription %u | Want to send a publish response but can't. "
                             "The subscription is late.", sub->subscriptionId);
        sub->state = UA_SUBSCRIPTIONSTATE_LATE;
        if(pre)
            UA_Session_queuePublishReq(sub->session, pre, true); /* Re-enqueue */
        return;
    }

    /* Prepare the response */
    UA_PublishResponse *response = &pre->response;
    UA_NotificationMessage *message = &response->notificationMessage;
    UA_NotificationMessageEntry *retransmission = NULL;
    if(notifications > 0) {
        /* Allocate the retransmission entry */
        retransmission = (UA_NotificationMessageEntry*)UA_malloc(sizeof(UA_NotificationMessageEntry));
        if(!retransmission) {
            UA_LOG_WARNING_SESSION(server->config.logger, sub->session,
                                   "Subscription %u | Could not allocate memory for retransmission. "
                                   "The subscription is late.", sub->subscriptionId);
            sub->state = UA_SUBSCRIPTIONSTATE_LATE;
            UA_Session_queuePublishReq(sub->session, pre, true); /* Re-enqueue */
            return;
        }

        /* Prepare the response */
        UA_StatusCode retval = prepareNotificationMessage(server, sub, message, notifications);
        if(retval != UA_STATUSCODE_GOOD) {
            UA_LOG_WARNING_SESSION(server->config.logger, sub->session,
                                   "Subscription %u | Could not prepare the notification message. "
                                   "The subscription is late.", sub->subscriptionId);
            UA_free(retransmission);
            sub->state = UA_SUBSCRIPTIONSTATE_LATE;
            UA_Session_queuePublishReq(sub->session, pre, true); /* Re-enqueue */
            return;
        }
    }

    /* <-- The point of no return --> */

    /* Adjust the number of ready notifications */
    UA_assert(sub->readyNotifications >= notifications);
    sub->readyNotifications -= notifications;

    /* Set up the response */
    response->responseHeader.timestamp = UA_DateTime_now();
    response->subscriptionId = sub->subscriptionId;
    response->moreNotifications = moreNotifications;
    message->publishTime = response->responseHeader.timestamp;

    /* Set the sequence number. The sequence number will be reused if there are
     * no notifications (and this is a keepalive message). */
    message->sequenceNumber = UA_Subscription_nextSequenceNumber(sub->sequenceNumber);

    if(notifications != 0) {
        /* There are notifications. So we can't reuse the sequence number. */
        sub->sequenceNumber = message->sequenceNumber;

        /* Put the notification message into the retransmission queue. This
         * needs to be done here, so that the message itself is included in the
         * available sequence numbers for acknowledgement. */
        retransmission->message = response->notificationMessage;
        UA_Subscription_addRetransmissionMessage(server, sub, retransmission);
    }

    /* Get the available sequence numbers from the retransmission queue */
    size_t available = sub->retransmissionQueueSize;
    UA_STACKARRAY(UA_UInt32, seqNumbers, available);
    if(available > 0) {
        response->availableSequenceNumbers = seqNumbers;
        response->availableSequenceNumbersSize = available;
        size_t i = 0;
        UA_NotificationMessageEntry *nme;
        TAILQ_FOREACH(nme, &sub->retransmissionQueue, listEntry) {
            response->availableSequenceNumbers[i] = nme->message.sequenceNumber;
            ++i;
        }
    }

    /* Send the response */
    UA_LOG_DEBUG_SESSION(server->config.logger, sub->session,
                         "Subscription %u | Sending out a publish response "
                         "with %u notifications", sub->subscriptionId,
                         (UA_UInt32)notifications);
    UA_SecureChannel_sendSymmetricMessage(sub->session->header.channel, pre->requestId,
                                          UA_MESSAGETYPE_MSG, response,
                                          &UA_TYPES[UA_TYPES_PUBLISHRESPONSE]);

    /* Reset subscription state to normal */
    sub->state = UA_SUBSCRIPTIONSTATE_NORMAL;
    sub->currentKeepAliveCount = 0;

    /* Free the response */
    UA_Array_delete(response->results, response->resultsSize, &UA_TYPES[UA_TYPES_UINT32]);
    UA_free(pre); /* No need for UA_PublishResponse_deleteMembers */

    /* Repeat sending responses if there are more notifications to send */
    if(moreNotifications)
        UA_Subscription_publish(server, sub);
}

UA_Boolean
UA_Subscription_reachedPublishReqLimit(UA_Server *server,  UA_Session *session) {
    UA_LOG_DEBUG_SESSION(server->config.logger, session, "Reached number of publish request limit");

    /* Dequeue a response */
    UA_PublishResponseEntry *pre = UA_Session_dequeuePublishReq(session);

    /* Cannot publish without a response */
    if(!pre) {
        UA_LOG_FATAL_SESSION(server->config.logger, session, "No publish requests available");
        return false;
    }

    /* <-- The point of no return --> */

    UA_PublishResponse *response = &pre->response;
    UA_NotificationMessage *message = &response->notificationMessage;

    /* Set up the response. Note that this response has no related subscription id */
    response->responseHeader.timestamp = UA_DateTime_now();
    response->responseHeader.serviceResult = UA_STATUSCODE_BADTOOMANYPUBLISHREQUESTS;
    response->subscriptionId = 0;
    response->moreNotifications = false;
    message->publishTime = response->responseHeader.timestamp;
    message->sequenceNumber = 0;
    response->availableSequenceNumbersSize = 0;

    /* Send the response */
    UA_LOG_DEBUG_SESSION(server->config.logger, session,
                         "Sending out a publish response triggered by too many publish requests");
    UA_SecureChannel_sendSymmetricMessage(session->header.channel, pre->requestId,
                     UA_MESSAGETYPE_MSG, response, &UA_TYPES[UA_TYPES_PUBLISHRESPONSE]);

    /* Free the response */
    UA_Array_delete(response->results, response->resultsSize, &UA_TYPES[UA_TYPES_UINT32]);
    UA_free(pre); /* no need for UA_PublishResponse_deleteMembers */

    return true;
}

UA_StatusCode
Subscription_registerPublishCallback(UA_Server *server, UA_Subscription *sub) {
    UA_LOG_DEBUG_SESSION(server->config.logger, sub->session,
                         "Subscription %u | Register subscription "
                         "publishing callback", sub->subscriptionId);

    if(sub->publishCallbackIsRegistered)
        return UA_STATUSCODE_GOOD;

    UA_StatusCode retval =
        UA_Server_addRepeatedCallback(server, (UA_ServerCallback)publishCallback,
                                      sub, (UA_UInt32)sub->publishingInterval, &sub->publishCallbackId);
    if(retval != UA_STATUSCODE_GOOD)
        return retval;

    sub->publishCallbackIsRegistered = true;
    return UA_STATUSCODE_GOOD;
}

UA_StatusCode
Subscription_unregisterPublishCallback(UA_Server *server, UA_Subscription *sub) {
    UA_LOG_DEBUG_SESSION(server->config.logger, sub->session, "Subscription %u | "
                         "Unregister subscription publishing callback", sub->subscriptionId);

    if(!sub->publishCallbackIsRegistered)
        return UA_STATUSCODE_GOOD;

    UA_StatusCode retval = UA_Server_removeRepeatedCallback(server, sub->publishCallbackId);
    if(retval != UA_STATUSCODE_GOOD)
        return retval;

    sub->publishCallbackIsRegistered = false;
    return UA_STATUSCODE_GOOD;
}

/* When the session has publish requests stored but the last subscription is
 * deleted... Send out empty responses */
void
UA_Subscription_answerPublishRequestsNoSubscription(UA_Server *server, UA_Session *session) {
    /* No session or there are remaining subscriptions */
    if(!session || LIST_FIRST(&session->serverSubscriptions))
        return;

    /* Send a response for every queued request */
    UA_PublishResponseEntry *pre;
    while((pre = UA_Session_dequeuePublishReq(session))) {
        UA_PublishResponse *response = &pre->response;
        response->responseHeader.serviceResult = UA_STATUSCODE_BADNOSUBSCRIPTION;
        response->responseHeader.timestamp = UA_DateTime_now();
        UA_SecureChannel_sendSymmetricMessage(session->header.channel, pre->requestId, UA_MESSAGETYPE_MSG,
                                              response, &UA_TYPES[UA_TYPES_PUBLISHRESPONSE]);
        UA_PublishResponse_deleteMembers(response);
        UA_free(pre);
    }
}

#endif /* UA_ENABLE_SUBSCRIPTIONS */
