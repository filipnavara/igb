#include "precomp.h"

#include <initguid.h>
#include <wdmguid.h>
#include "trace.h"
#include "device.h"
#include "adapter.h"
//#include "configuration.h"
#include "link.h"

NTSTATUS
IgbGetResources(
	_In_ IGB_ADAPTER* adapter,
	_In_ WDFCMRESLIST resourcesRaw,
	_In_ WDFCMRESLIST resourcesTranslated)
{
	DBGPRINT("IntelGetResources\n");

	NTSTATUS status = STATUS_SUCCESS;

	ULONG errorCode = 0;
	ULONG memRegCnt = 0;

	// According to https://msdn.microsoft.com/windows/hardware/drivers/wdf/raw-and-translated-resources
	// "Both versions represent the same set of hardware resources, in the same order."
	ULONG rawCount = WdfCmResourceListGetCount(resourcesRaw);
	NT_ASSERT(rawCount == WdfCmResourceListGetCount(resourcesTranslated));

	for (ULONG i = 0; i < rawCount; i++)
	{
		PCM_PARTIAL_RESOURCE_DESCRIPTOR rawDescriptor = WdfCmResourceListGetDescriptor(resourcesRaw, i);
		PCM_PARTIAL_RESOURCE_DESCRIPTOR translatedDescriptor = WdfCmResourceListGetDescriptor(resourcesTranslated, i);

		if (rawDescriptor->Type == CmResourceTypeMemory)
		{
			if (memRegCnt == 0)
			{
				adapter->MMIOAddress = MmMapIoSpaceEx(
					translatedDescriptor->u.Memory.Start,
					translatedDescriptor->u.Memory.Length,
					PAGE_READWRITE | PAGE_NOCACHE);
				adapter->MMIOSize = translatedDescriptor->u.Memory.Length;
			}

			memRegCnt++;
		}
	}

	if (!adapter->MMIOAddress)
	{
		status = STATUS_RESOURCE_TYPE_NOT_FOUND;
		errorCode = NDIS_ERROR_CODE_RESOURCE_CONFLICT;
		GOTO_IF_NOT_NT_SUCCESS(Exit, status, STATUS_NDIS_RESOURCE_CONFLICT);
	}

	status = WdfFdoQueryForInterface(adapter->WdfDevice,
		&GUID_BUS_INTERFACE_STANDARD,
		(PINTERFACE)&adapter->PciConfig,
		sizeof(BUS_INTERFACE_STANDARD),
		1, // Version
		NULL); //InterfaceSpecificData
	if (!NT_SUCCESS(status))
		goto Exit;

Exit:
	DBGPRINT("IntelGetResources - %x\n", status);
	return status;
}

NTSTATUS
IgbRegisterScatterGatherDma(
	_In_ IGB_ADAPTER* adapter)
{
	//TraceEntryRtAdapter(adapter);
	DBGPRINT("IntelRegisterScatterGatherDma\n");

	WDF_DMA_ENABLER_CONFIG dmaEnablerConfig;
	WDF_DMA_ENABLER_CONFIG_INIT(&dmaEnablerConfig, WdfDmaProfileScatterGather64, /*adapter->bsdData.max_jumbo_frame_size*/IGB_BUF_SIZE);
	dmaEnablerConfig.Flags |= WDF_DMA_ENABLER_CONFIG_REQUIRE_SINGLE_TRANSFER;
	dmaEnablerConfig.WdmDmaVersionOverride = 3;

	NTSTATUS status = STATUS_SUCCESS;
	GOTO_IF_NOT_NT_SUCCESS(Exit, status,
		WdfDmaEnablerCreate(
			adapter->WdfDevice,
			&dmaEnablerConfig,
			WDF_NO_OBJECT_ATTRIBUTES,
			&adapter->DmaEnabler),
		TraceLoggingRtAdapter(adapter));

Exit:
	DBGPRINT("IntelRegisterScatterGatherDma - %x\n", status);
	return status;
}

void
IgbReleaseHardware(
	_In_ IGB_ADAPTER* adapter)
{
	if (adapter->MMIOAddress)
	{
		MmUnmapIoSpace(
			adapter->MMIOAddress,
			adapter->MMIOSize);
		adapter->MMIOAddress = NULL;
		adapter->MMIOSize = 0;
	}
}

NTSTATUS
IgbInitializeHardware(
	_In_ IGB_ADAPTER* adapter,
	_In_ WDFCMRESLIST resourcesRaw,
	_In_ WDFCMRESLIST resourcesTranslated)
{
	DBGPRINT("IntelInitializeHardware\n");

	NTSTATUS status = STATUS_SUCCESS;

	struct e1000_hw* hw = &adapter->Hw;

	GOTO_IF_NOT_NT_SUCCESS(Exit, status,
		NetAdapterOpenConfiguration(adapter->NetAdapter, WDF_NO_OBJECT_ATTRIBUTES, &adapter->NetConfiguration));

	GOTO_IF_NOT_NT_SUCCESS(Exit, status,
		IgbGetResources(adapter, resourcesRaw, resourcesTranslated));

	RtlZeroMemory(&adapter->Hw, sizeof(adapter->Hw));
	hw->hw_addr = (u8 *)adapter->MMIOAddress;
	hw->back = adapter->MMIOAddress;

	DECLSPEC_ALIGN(MEMORY_ALLOCATION_ALIGNMENT) PCI_COMMON_CONFIG pci_config;
	RtlZeroMemory(&pci_config, sizeof(pci_config));
	adapter->PciConfig.GetBusData(
		adapter->PciConfig.Context,
		PCI_WHICHSPACE_CONFIG, //READ
		&pci_config,
		FIELD_OFFSET(PCI_COMMON_CONFIG, VendorID),
		sizeof(PCI_COMMON_CONFIG));

	hw->bus.pci_cmd_word = pci_config.Command;
	hw->vendor_id = pci_config.VendorID;
	hw->device_id = pci_config.DeviceID;
	hw->revision_id = pci_config.RevisionID;
	hw->subsystem_vendor_id = pci_config.u.type0.SubVendorID;
	hw->subsystem_device_id = pci_config.u.type0.SubSystemID;

	DBGPRINT("Vendor: %x\n", hw->vendor_id);
	DBGPRINT("Device: %x\n", hw->device_id);

	// Do Shared Code initialization
	if (e1000_setup_init_funcs(hw, TRUE))
	{
		DBGPRINT("e1000_setup_init_funcs failed\n");
		GOTO_IF_NOT_NT_SUCCESS(Exit, status, NDIS_STATUS_ADAPTER_NOT_FOUND);
	}

	e1000_get_bus_info(hw);

	hw->mac.autoneg = 1;
	hw->phy.autoneg_wait_to_complete = FALSE;
	hw->phy.autoneg_advertised = ADVERTISE_10_HALF | ADVERTISE_10_FULL | ADVERTISE_100_HALF | ADVERTISE_100_FULL | ADVERTISE_1000_FULL;

	// Copper options
	if (hw->phy.media_type == e1000_media_type_copper)
	{
		hw->phy.mdix = 0;
		hw->phy.disable_polarity_correction = FALSE;
		hw->phy.ms_type = e1000_ms_hw_default;
	}

	GOTO_IF_NOT_NT_SUCCESS(Exit, status,
		IgbRegisterScatterGatherDma(adapter),
		TraceLoggingRtAdapter(adapter));

	e1000_reset_hw(hw);

	if (e1000_read_mac_addr(hw) < 0) {
		DBGPRINT("EEPROM read error while reading MAC address\n");
		GOTO_IF_NOT_NT_SUCCESS(Exit, status, NDIS_STATUS_INVALID_ADDRESS);
	}

	adapter->PermanentAddress.Length = ETH_LENGTH_OF_ADDRESS;
	memcpy(adapter->PermanentAddress.Address, hw->mac.perm_addr, ETH_LENGTH_OF_ADDRESS);

	DBGPRINT("Permanent Address = %02x-%02x-%02x-%02x-%02x-%02x\n",
		adapter->PermanentAddress.Address[0], adapter->PermanentAddress.Address[1],
		adapter->PermanentAddress.Address[2], adapter->PermanentAddress.Address[3],
		adapter->PermanentAddress.Address[4], adapter->PermanentAddress.Address[5]);

	status = NetConfigurationQueryLinkLayerAddress(adapter->NetConfiguration, &adapter->CurrentAddress);
	if (!NT_SUCCESS(status) ||
		adapter->CurrentAddress.Length != ETH_LENGTH_OF_ADDRESS ||
		ETH_IS_MULTICAST(adapter->CurrentAddress.Address) ||
		ETH_IS_BROADCAST(adapter->CurrentAddress.Address))
	{
		RtlCopyMemory(&adapter->CurrentAddress, &adapter->PermanentAddress, sizeof(adapter->PermanentAddress));
	}

	DBGPRINT("Current Address = %02x-%02x-%02x-%02x-%02x-%02x\n",
		adapter->CurrentAddress.Address[0], adapter->CurrentAddress.Address[1],
		adapter->CurrentAddress.Address[2], adapter->CurrentAddress.Address[3],
		adapter->CurrentAddress.Address[4], adapter->CurrentAddress.Address[5]);

	NET_ADAPTER_LINK_STATE linkState;
	NET_ADAPTER_LINK_STATE_INIT_DISCONNECTED(&linkState);
	NetAdapterSetLinkState(adapter->NetAdapter, &linkState);

	GOTO_IF_NOT_NT_SUCCESS(Exit, status,
		IgbAdapterStart(adapter));

Exit:
	DBGPRINT("IntelInitializeHardware - %x\n", status);
	return status;
}

_Use_decl_annotations_
NTSTATUS
EvtDevicePrepareHardware(
	_In_ WDFDEVICE device,
	_In_ WDFCMRESLIST resourcesRaw,
	_In_ WDFCMRESLIST resourcesTranslated)
{
	IGB_ADAPTER* adapter = IgbGetDeviceContext(device)->Adapter;

	DBGPRINT("EvtDevicePrepareHardware\n");

	NTSTATUS status = STATUS_SUCCESS;
	GOTO_IF_NOT_NT_SUCCESS(Exit, status,
		IgbInitializeHardware(adapter, resourcesRaw, resourcesTranslated));

Exit:
	DBGPRINT("EvtDevicePrepareHardware - %x\n", status);
	return status;
}

_Use_decl_annotations_
NTSTATUS
EvtDeviceReleaseHardware(
	_In_ WDFDEVICE device,
	_In_ WDFCMRESLIST resourcesTranslated)
{
	UNREFERENCED_PARAMETER(resourcesTranslated);
	IGB_ADAPTER* adapter = IgbGetDeviceContext(device)->Adapter;

	DBGPRINT("EvtDeviceReleaseHardware\n");

	IgbReleaseHardware(adapter);

	NTSTATUS status = STATUS_SUCCESS;
	return status;
}