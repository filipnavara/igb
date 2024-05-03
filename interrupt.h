#pragma once

typedef struct _IGB_INTERRUPT
{
	IGB_ADAPTER* Adapter;
	WDFINTERRUPT Handle;
	LONG EICR;
	LONG ICR;
} IGB_INTERRUPT, *PIGB_INTERRUPT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(IGB_INTERRUPT, IgbGetInterruptContext);

NTSTATUS
IgbInterruptCreate(
	_In_ WDFDEVICE wdfDevice,
	_In_ IGB_ADAPTER* adapter,
	_In_ PCM_PARTIAL_RESOURCE_DESCRIPTOR rawDescriptor,
	_In_ PCM_PARTIAL_RESOURCE_DESCRIPTOR translatedDescriptor,
	_Out_ IGB_INTERRUPT** interrupt);

void
IgbInterruptInitialize(
	_In_ IGB_ADAPTER* adapter);

EVT_WDF_INTERRUPT_ISR EvtInterruptIsr;
EVT_WDF_INTERRUPT_DPC EvtInterruptDpc;
EVT_WDF_INTERRUPT_ISR EvtQueueInterruptIsr;
EVT_WDF_INTERRUPT_DPC EvtQueueInterruptDpc;
