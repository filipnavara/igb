#pragma once

typedef struct _IGB_RXQUEUE {
	IGB_ADAPTER* Adapter;
	IGB_INTERRUPT* Interrupt;

	NET_RING_COLLECTION const* Rings;

	// descriptor information
	WDFCOMMONBUFFER RxdArray;
	union e1000_adv_rx_desc* RxdBase;
	size_t RxdSize;

	USHORT NumRxDesc;

	NET_EXTENSION LogicalAddressExtension;
	NET_EXTENSION Ieee8021qExtension;
	/*NET_EXTENSION ChecksumExtension;
	NET_EXTENSION VirtualAddressExtension;
	NET_EXTENSION HashValueExtension;
	*/

	ULONG QueueId;
} IGB_RXQUEUE, *PIGB_RXQUEUE;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(IGB_RXQUEUE, IgbGetRxQueueContext);

NTSTATUS IgbRxQueueInitialize(_In_ NETPACKETQUEUE rxQueue, _In_ IGB_ADAPTER* adapter);
EVT_WDF_OBJECT_CONTEXT_DESTROY EvtRxQueueDestroy;
EVT_PACKET_QUEUE_SET_NOTIFICATION_ENABLED EvtRxQueueSetNotificationEnabled;
EVT_PACKET_QUEUE_ADVANCE EvtRxQueueAdvance;
EVT_PACKET_QUEUE_CANCEL EvtRxQueueCancel;
EVT_PACKET_QUEUE_START EvtRxQueueStart;
EVT_PACKET_QUEUE_STOP EvtRxQueueStop;