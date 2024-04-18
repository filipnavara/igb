#pragma once

extern "C"
{
#include "e1000_api.h"
}

#define IGB_MAX_TX_QUEUES 4
#define IGB_MAX_RX_QUEUES 4
#define IGB_MAX_MCAST_LIST 32

#define IGB_BUF_SIZE 2048

typedef struct _IGB_ADAPTER
{
	WDFDEVICE WdfDevice;
	NETADAPTER NetAdapter;
	NETCONFIGURATION NetConfiguration;

	WDFSPINLOCK Lock;
	WDFDMAENABLER DmaEnabler;

	NETPACKETQUEUE TxQueues[IGB_MAX_TX_QUEUES];
	NETPACKETQUEUE RxQueues[IGB_MAX_RX_QUEUES];

	PVOID MMIOAddress;
	SIZE_T MMIOSize;
	BUS_INTERFACE_STANDARD PciConfig;

	IGB_INTERRUPT* Interrupt;

	NET_PACKET_FILTER_FLAGS PacketFilterFlags;
	UINT MCAddressLength;
	UCHAR MCAddressList[E1000_RAR_ENTRIES * ETH_LENGTH_OF_ADDRESS];

	// Configuration
	//REG_SPEED_SETTING SpeedDuplex;
	NET_ADAPTER_LINK_LAYER_ADDRESS PermanentAddress;
	NET_ADAPTER_LINK_LAYER_ADDRESS CurrentAddress;
	//BOOLEAN OverrideAddress;
	//FLOW_CTRL FlowControl;
	//UINT16 VlanID;
	//ULONG64 MaxSpeed;
	//BOOLEAN TxIpHwChkSum;
	//BOOLEAN TxTcpHwChkSum;
	//BOOLEAN TxUdpHwChkSum;
	//BOOLEAN RxIpHwChkSum;
	//BOOLEAN RxTcpHwChkSum;
	//BOOLEAN RxUdpHwChkSum;
	//BOOLEAN LSOv4;
	//BOOLEAN LSOv6;

	struct e1000_hw Hw;
} IGB_ADAPTER, *PIGB_ADAPTER;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(IGB_ADAPTER, IgbGetAdapterContext);

EVT_NET_ADAPTER_CREATE_TXQUEUE EvtAdapterCreateTxQueue;
EVT_NET_ADAPTER_CREATE_RXQUEUE EvtAdapterCreateRxQueue;

NTSTATUS
IgbInitializeAdapterContext(
	_In_ PIGB_ADAPTER adapter,
	_In_ WDFDEVICE device,
	_In_ NETADAPTER netAdapter);

NTSTATUS
IgbAdapterStart(
	_In_ IGB_ADAPTER* adapter);
