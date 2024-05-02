#include "precomp.h"
#include "trace.h"
#include "adapter.h"
#include "interrupt.h"
#include "link.h"

NTSTATUS
IgbInterruptCreate(
	_In_ WDFDEVICE wdfDevice,
	_In_ IGB_ADAPTER* adapter,
	_In_ PCM_PARTIAL_RESOURCE_DESCRIPTOR rawDescriptor,
	_In_ PCM_PARTIAL_RESOURCE_DESCRIPTOR translatedDescriptor,
	_Out_ IGB_INTERRUPT** interrupt)
{
	DBGPRINT("IntelInterruptCreate");

	*interrupt = nullptr;

	WDF_OBJECT_ATTRIBUTES attributes;
	WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, IGB_INTERRUPT);

	WDF_INTERRUPT_CONFIG config;
	WDF_INTERRUPT_CONFIG_INIT(&config, EvtInterruptIsr, EvtInterruptDpc);

	config.InterruptRaw = rawDescriptor;
	config.InterruptTranslated = translatedDescriptor;

	NTSTATUS status = STATUS_SUCCESS;

	WDFINTERRUPT wdfInterrupt;
	GOTO_IF_NOT_NT_SUCCESS(Exit, status,
		WdfInterruptCreate(wdfDevice, &config, &attributes, &wdfInterrupt));

	IGB_INTERRUPT* context = IgbGetInterruptContext(wdfInterrupt);
	context->Adapter = adapter;
	context->Handle = wdfInterrupt;
	context->IsMsi = translatedDescriptor->Flags & CM_RESOURCE_INTERRUPT_MESSAGE ? TRUE : FALSE;
	*interrupt = context;

Exit:
	return status;
}

void
IgbInterruptInitialize(
	_In_ IGB_INTERRUPT* interrupt)
{
	// Map RX queues to EICR bits
	// TODO: Handle 82575/82576 models
	for (int i = 0; i < IGB_MAX_RX_QUEUES; i++)
	{
		u32 index = i >> 1;
		u32 ivar = E1000_READ_REG_ARRAY(&interrupt->Adapter->Hw, E1000_IVAR0, index);
		if (i & 1)
		{
			ivar &= 0xFF00FFFF;
			ivar |= (i | E1000_IVAR_VALID) << 16;
		}
		else
		{
			ivar &= 0xFFFFFF00;
			ivar |= i | E1000_IVAR_VALID;
		}
		E1000_WRITE_REG_ARRAY(&interrupt->Adapter->Hw, E1000_IVAR0, index, ivar);
	}

	// TODO: MSI-X
	if (interrupt->IsMsi)
	{
		E1000_WRITE_REG(&interrupt->Adapter->Hw, E1000_GPIE, E1000_GPIE_NSICR);
	}
	else
	{
		E1000_WRITE_REG(&interrupt->Adapter->Hw, E1000_GPIE, 0);
	}
}

_Use_decl_annotations_
BOOLEAN
EvtInterruptIsr(
	_In_ WDFINTERRUPT wdfInterrupt,
	ULONG MessageID)
{
	UNREFERENCED_PARAMETER((MessageID));

	IGB_INTERRUPT* interrupt = IgbGetInterruptContext(wdfInterrupt);
	IGB_ADAPTER* adapter = interrupt->Adapter;
	struct e1000_hw* hw = &adapter->Hw;
	u32 icr, eicr;
	BOOLEAN queueDPC = FALSE;

	eicr = E1000_READ_REG(hw, E1000_EICR);

	// Hot eject
	if (eicr == 0xffffffff)
	{
		return TRUE;
	}

	if (eicr != 0)
	{
		InterlockedOr(&interrupt->EICR, eicr);

		if ((eicr & E1000_EICR_OTHER) != 0)
		{
			icr = E1000_READ_REG(hw, E1000_ICR);
			InterlockedOr(&interrupt->ICR, icr);
		}

		WdfInterruptQueueDpcForIsr(wdfInterrupt);
		return TRUE;
	}

	return interrupt->IsMsi;
}

_Use_decl_annotations_
VOID
EvtInterruptDpc(
	_In_ WDFINTERRUPT Interrupt,
	_In_ WDFOBJECT AssociatedObject)
{
	UNREFERENCED_PARAMETER(AssociatedObject);

	IGB_INTERRUPT* interrupt = IgbGetInterruptContext(Interrupt);
	IGB_ADAPTER* adapter = interrupt->Adapter;
	struct e1000_hw* hw = &adapter->Hw;
	u32 icr = 0, eicr;

	eicr = InterlockedExchange(&interrupt->EICR, 0);

	for (int i = 0; i < IGB_MAX_RX_QUEUES; i++)
	{
		if ((eicr & (1 << i)) != 0)
		{
			if (InterlockedExchange(&interrupt->RxNotifyArmed[i], FALSE))
				NetRxQueueNotifyMoreReceivedPacketsAvailable(adapter->RxQueues[i]);
		}
	}

	if ((eicr & E1000_EICR_OTHER) != 0)
	{
		icr = InterlockedExchange(&interrupt->ICR, 0);

		if ((icr & (E1000_ICR_TXDW | E1000_ICR_TXQE)) != 0 &&
			InterlockedExchange(&interrupt->TxNotifyArmed[0], FALSE))
		{
			NetTxQueueNotifyMoreCompletedPacketsAvailable(adapter->TxQueues[0]);
		}

		if ((icr & E1000_ICR_LSC) != 0)
		{
			DBGPRINT("Link Interrupt!\n");
			hw->mac.get_link_status = 1;
			IgbCheckLinkStatus(adapter);
		}
	}
}