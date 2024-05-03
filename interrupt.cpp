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
	*interrupt = context;

Exit:
	return status;
}

void
IgbInterruptInitialize(
	_In_ IGB_ADAPTER* adapter)
{
	// TODO: Handle 82575/82576 models

	if (adapter->MsiInterrupts > 1)
	{
		UINT miscInterrupt = adapter->MsiInterrupts - 1;

		// Enable MSI-X mode
		E1000_WRITE_REG(&adapter->Hw, E1000_GPIE, E1000_GPIE_NSICR | E1000_GPIE_MSIX_MODE | E1000_GPIE_EIAME | E1000_GPIE_PBA);

		// Map link & misc interrupts
		E1000_WRITE_REG(&adapter->Hw, E1000_IVAR_MISC, ((miscInterrupt | E1000_IVAR_VALID) << 8));

		// Map the TX queue to the misc interrupt
		u32 ivar = E1000_READ_REG_ARRAY(&adapter->Hw, E1000_IVAR0, 0);
		E1000_WRITE_REG_ARRAY(&adapter->Hw, E1000_IVAR0, 0, (ivar & 0xFFFF00FF) | ((miscInterrupt | E1000_IVAR_VALID) << 8));

		// Automatically clear interrupt causes
		E1000_WRITE_REG(&adapter->Hw, E1000_EIAC, (1 << adapter->MsiInterrupts) - 1);
		// Automatically mask RX queues
		E1000_WRITE_REG(&adapter->Hw, E1000_EIAM, (1 << (adapter->MsiInterrupts - 1)) - 1);
		// Enable misc interrupt
		E1000_WRITE_REG(&adapter->Hw, E1000_EIMS, (1 << miscInterrupt));

		adapter->MiscInterrupt = adapter->Interrupts[miscInterrupt];
	}
	else
	{
		E1000_WRITE_REG(&adapter->Hw, E1000_GPIE, adapter->MsiInterrupts == 1 ? E1000_GPIE_NSICR : 0);
		adapter->MiscInterrupt = adapter->Interrupts[0];
	}

	// Map RX queues to EICR bits
	for (int i = 0; i < IGB_MAX_RX_QUEUES; i++)
	{
		u32 index = i >> 1;
		u32 ivar = E1000_READ_REG_ARRAY(&adapter->Hw, E1000_IVAR0, index);
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
		E1000_WRITE_REG_ARRAY(&adapter->Hw, E1000_IVAR0, index, ivar);
	}

	E1000_WRITE_FLUSH(&adapter->Hw);
}

_Use_decl_annotations_
BOOLEAN
EvtInterruptIsr(
	_In_ WDFINTERRUPT wdfInterrupt,
	_In_ ULONG messageId)
{
	IGB_INTERRUPT* interrupt = IgbGetInterruptContext(wdfInterrupt);
	IGB_ADAPTER* adapter = interrupt->Adapter;
	struct e1000_hw* hw = &adapter->Hw;

	if (adapter->MsiInterrupts > 1)
	{
		if (interrupt == adapter->MiscInterrupt)
		{
			// We use MSI-X for RX queues and we are forbidden to read EICR
			u32 icr = E1000_READ_REG(hw, E1000_ICR);
			
			// Disarm the TX interrupts for queue where a transfer finished
			if ((icr & (E1000_ICR_TXDW | E1000_ICR_TXQE)) != 0)
			{
				E1000_WRITE_REG(&interrupt->Adapter->Hw, E1000_IMC, E1000_IMS_TXDW | E1000_ICR_TXQE);
				E1000_WRITE_FLUSH(&interrupt->Adapter->Hw);
			}

			InterlockedOr(&interrupt->ICR, icr);
			InterlockedOr(&interrupt->EICR, E1000_EICR_OTHER);
		}
		else
		{
			// Mark the RX queue
			InterlockedOr(&interrupt->EICR, 1 << messageId);
		}

		WdfInterruptQueueDpcForIsr(wdfInterrupt);
		return TRUE;
	}
	else
	{
		u32 eicr = E1000_READ_REG(hw, E1000_EICR);

		// Hot eject
		if (eicr == 0xffffffff)
		{
			return TRUE;
		}

		if (eicr != 0)
		{
			// Disarm the RX interrupts for queues where we received packets
			u32 eicrQueueMask = ((1 << IGB_MAX_RX_QUEUES) - 1);
			if ((eicr & eicrQueueMask) != 0)
			{
				E1000_WRITE_REG(&interrupt->Adapter->Hw, E1000_EIMC, eicr & eicrQueueMask);
				E1000_WRITE_FLUSH(&interrupt->Adapter->Hw);
			}

			if ((eicr & E1000_EICR_OTHER) != 0)
			{
				u32 icr = E1000_READ_REG(hw, E1000_ICR);

				// Disarm the TX interrupts for queue where a transfer finished
				if ((icr & (E1000_ICR_TXDW | E1000_ICR_TXQE)) != 0)
				{
					E1000_WRITE_REG(&interrupt->Adapter->Hw, E1000_IMC, E1000_IMS_TXDW | E1000_ICR_TXQE);
					E1000_WRITE_FLUSH(&interrupt->Adapter->Hw);
				}

				InterlockedOr(&interrupt->ICR, icr);
			}

			InterlockedOr(&interrupt->EICR, eicr);

			WdfInterruptQueueDpcForIsr(wdfInterrupt);
			return TRUE;
		}

		return FALSE;
	}
}

_Use_decl_annotations_
VOID
EvtInterruptDpc(
	_In_ WDFINTERRUPT wdfInterrupt,
	_In_ WDFOBJECT associatedObject)
{
	UNREFERENCED_PARAMETER(associatedObject);

	IGB_INTERRUPT* interrupt = IgbGetInterruptContext(wdfInterrupt);
	IGB_ADAPTER* adapter = interrupt->Adapter;

	LONG eicr = InterlockedExchange(&interrupt->EICR, 0);

	if ((eicr & ((1 << IGB_MAX_RX_QUEUES) - 1)) != 0)
	{
		for (int i = 0; i < IGB_MAX_RX_QUEUES; i++)
		{
			if ((eicr & (1 << i)) != 0)
			{
				NetRxQueueNotifyMoreReceivedPacketsAvailable(adapter->RxQueues[i]);
			}
		}
	}

	if ((eicr & E1000_EICR_OTHER) != 0)
	{
		LONG icr = InterlockedExchange(&interrupt->ICR, 0);

		if ((icr & (E1000_ICR_TXDW | E1000_ICR_TXQE)) != 0)
		{
			NetTxQueueNotifyMoreCompletedPacketsAvailable(adapter->TxQueues[0]);
		}

		if ((icr & E1000_ICR_LSC) != 0)
		{
			DBGPRINT("Link Interrupt!\n");
			adapter->Hw.mac.get_link_status = 1;
			IgbCheckLinkStatus(adapter);
		}
	}
}
