/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * Device Redirection Virtual Channel Extension
 *
 * Copyright 2014 Dell Software <Mike.McDonald@software.dell.com>
 * Copyright 2013 Marc-Andre Moreau <marcandre.moreau@gmail.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <winpr/crt.h>
#include <winpr/nt.h>
#include <winpr/print.h>
#include <winpr/stream.h>

#include <freerdp/channels/log.h>
#include "rdpdr_main.h"

#define TAG "rdpdr.server"

static UINT32 g_ClientId = 0;

static RDPDR_IRP* rdpdr_server_irp_new()
{
	RDPDR_IRP* irp;

	irp = (RDPDR_IRP*) calloc(1, sizeof(RDPDR_IRP));

	return irp;
}

static void rdpdr_server_irp_free(RDPDR_IRP* irp)
{
	free(irp);
}

static void rdpdr_server_enqueue_irp(RdpdrServerContext* context, RDPDR_IRP* irp)
{
	ListDictionary_Add(context->priv->IrpList, (void *) irp->CompletionId, irp);
}

static RDPDR_IRP* rdpdr_server_dequeue_irp(RdpdrServerContext* context, UINT32 completionId)
{
	RDPDR_IRP* irp;

	irp = (RDPDR_IRP*) ListDictionary_Remove(context->priv->IrpList, (void *) completionId);

	return irp;
}

static int rdpdr_server_send_announce_request(RdpdrServerContext* context)
{
	wStream* s;
	BOOL status;
	RDPDR_HEADER header;
	ULONG written;

	WLog_DBG(TAG, "RdpdrServerSendAnnounceRequest");

	header.Component = RDPDR_CTYP_CORE;
	header.PacketId = PAKID_CORE_SERVER_ANNOUNCE;
	s = Stream_New(NULL, RDPDR_HEADER_LENGTH + 8);
	Stream_Write_UINT16(s, header.Component); /* Component (2 bytes) */
	Stream_Write_UINT16(s, header.PacketId); /* PacketId (2 bytes) */
	Stream_Write_UINT16(s, context->priv->VersionMajor); /* VersionMajor (2 bytes) */
	Stream_Write_UINT16(s, context->priv->VersionMinor); /* VersionMinor (2 bytes) */
	Stream_Write_UINT32(s, context->priv->ClientId); /* ClientId (4 bytes) */

	Stream_SealLength(s);

	winpr_HexDump(TAG, WLOG_DEBUG, Stream_Buffer(s), Stream_Length(s));

	status = WTSVirtualChannelWrite(context->priv->ChannelHandle, (PCHAR) Stream_Buffer(s), Stream_Length(s), &written);

	Stream_Free(s, TRUE);

	return 0;
}

static int rdpdr_server_receive_announce_response(RdpdrServerContext* context, wStream* s, RDPDR_HEADER* header)
{
	UINT32 ClientId;
	UINT16 VersionMajor;
	UINT16 VersionMinor;

	Stream_Read_UINT16(s, VersionMajor); /* VersionMajor (2 bytes) */
	Stream_Read_UINT16(s, VersionMinor); /* VersionMinor (2 bytes) */
	Stream_Read_UINT32(s, ClientId); /* ClientId (4 bytes) */

	WLog_DBG(TAG, "Client Announce Response: VersionMajor: 0x%04X VersionMinor: 0x%04X ClientId: 0x%04X",
			 VersionMajor, VersionMinor, ClientId);

	context->priv->ClientId = ClientId;

	return 0;
}

static int rdpdr_server_receive_client_name_request(RdpdrServerContext* context, wStream* s, RDPDR_HEADER* header)
{
	UINT32 UnicodeFlag;
	UINT32 ComputerNameLen;

	Stream_Read_UINT32(s, UnicodeFlag); /* UnicodeFlag (4 bytes) */
	Stream_Seek_UINT32(s); /* CodePage (4 bytes), MUST be set to zero */
	Stream_Read_UINT32(s, ComputerNameLen); /* ComputerNameLen (4 bytes) */

	/**
	 * Caution: ComputerNameLen is given *bytes*,
	 * not in characters, including the NULL terminator!
	 */

	if (context->priv->ClientComputerName)
	{
		free(context->priv->ClientComputerName);
		context->priv->ClientComputerName = NULL;
	}

	if (UnicodeFlag)
	{
		ConvertFromUnicode(CP_UTF8, 0, (WCHAR*) Stream_Pointer(s),
						   -1, &(context->priv->ClientComputerName), 0, NULL, NULL);
	}
	else
	{
		context->priv->ClientComputerName = _strdup((char*) Stream_Pointer(s));
	}

	Stream_Seek(s, ComputerNameLen);

	WLog_DBG(TAG, "ClientComputerName: %s", context->priv->ClientComputerName);

	fflush(stdout);

	return 0;
}

static int rdpdr_server_read_capability_set_header(wStream* s, RDPDR_CAPABILITY_HEADER* header)
{
	Stream_Read_UINT16(s, header->CapabilityType); /* CapabilityType (2 bytes) */
	Stream_Read_UINT16(s, header->CapabilityLength); /* CapabilityLength (2 bytes) */
	Stream_Read_UINT32(s, header->Version); /* Version (4 bytes) */

	return 0;
}

static int rdpdr_server_write_capability_set_header(wStream* s, RDPDR_CAPABILITY_HEADER* header)
{
	Stream_Write_UINT16(s, header->CapabilityType); /* CapabilityType (2 bytes) */
	Stream_Write_UINT16(s, header->CapabilityLength); /* CapabilityLength (2 bytes) */
	Stream_Write_UINT32(s, header->Version); /* Version (4 bytes) */

	return 0;
}

static int rdpdr_server_read_general_capability_set(RdpdrServerContext* context, wStream* s, RDPDR_CAPABILITY_HEADER* header)
{
	UINT32 ioCode1;
	UINT32 extraFlags1;
	UINT32 extendedPdu;
	UINT16 VersionMajor;
	UINT16 VersionMinor;
	UINT32 SpecialTypeDeviceCap;

	Stream_Seek_UINT32(s); /* osType (4 bytes), ignored on receipt */
	Stream_Seek_UINT32(s); /* osVersion (4 bytes), unused and must be set to zero */
	Stream_Read_UINT16(s, VersionMajor); /* protocolMajorVersion (2 bytes) */
	Stream_Read_UINT16(s, VersionMinor); /* protocolMinorVersion (2 bytes) */
	Stream_Read_UINT32(s, ioCode1); /* ioCode1 (4 bytes) */
	Stream_Seek_UINT32(s); /* ioCode2 (4 bytes), must be set to zero, reserved for future use */
	Stream_Read_UINT32(s, extendedPdu); /* extendedPdu (4 bytes) */
	Stream_Read_UINT32(s, extraFlags1); /* extraFlags1 (4 bytes) */
	Stream_Seek_UINT32(s); /* extraFlags2 (4 bytes), must be set to zero, reserved for future use */
	if (header->Version == GENERAL_CAPABILITY_VERSION_02)
	{
		Stream_Read_UINT32(s, SpecialTypeDeviceCap); /* SpecialTypeDeviceCap (4 bytes) */
	}

	context->priv->UserLoggedOnPdu = (extendedPdu & RDPDR_USER_LOGGEDON_PDU) ? TRUE : FALSE;

	return 0;
}

static int rdpdr_server_write_general_capability_set(RdpdrServerContext* context, wStream* s)
{
	UINT32 ioCode1;
	UINT32 extendedPdu;
	UINT32 extraFlags1;
	UINT32 SpecialTypeDeviceCap;
	RDPDR_CAPABILITY_HEADER header;
    
	header.CapabilityType = CAP_GENERAL_TYPE;
	header.CapabilityLength = RDPDR_CAPABILITY_HEADER_LENGTH + 36;
	if (!Stream_EnsureRemainingCapacity(s, header.CapabilityLength))
		return -1;

	header.CapabilityType = CAP_GENERAL_TYPE;
	header.Version = GENERAL_CAPABILITY_VERSION_02;
	ioCode1 = 0;
	ioCode1 |= RDPDR_IRP_MJ_CREATE; /* always set */
	ioCode1 |= RDPDR_IRP_MJ_CLEANUP; /* always set */
	ioCode1 |= RDPDR_IRP_MJ_CLOSE; /* always set */
	ioCode1 |= RDPDR_IRP_MJ_READ; /* always set */
	ioCode1 |= RDPDR_IRP_MJ_WRITE; /* always set */
	ioCode1 |= RDPDR_IRP_MJ_FLUSH_BUFFERS; /* always set */
	ioCode1 |= RDPDR_IRP_MJ_SHUTDOWN; /* always set */
	ioCode1 |= RDPDR_IRP_MJ_DEVICE_CONTROL; /* always set */
	ioCode1 |= RDPDR_IRP_MJ_QUERY_VOLUME_INFORMATION; /* always set */
	ioCode1 |= RDPDR_IRP_MJ_SET_VOLUME_INFORMATION; /* always set */
	ioCode1 |= RDPDR_IRP_MJ_QUERY_INFORMATION; /* always set */
	ioCode1 |= RDPDR_IRP_MJ_SET_INFORMATION; /* always set */
	ioCode1 |= RDPDR_IRP_MJ_DIRECTORY_CONTROL; /* always set */
	ioCode1 |= RDPDR_IRP_MJ_LOCK_CONTROL; /* always set */
	ioCode1 |= RDPDR_IRP_MJ_QUERY_SECURITY; /* optional */
	ioCode1 |= RDPDR_IRP_MJ_SET_SECURITY; /* optional */
	extendedPdu = 0;
	extendedPdu |= RDPDR_CLIENT_DISPLAY_NAME_PDU; /* always set */
	extendedPdu |= RDPDR_DEVICE_REMOVE_PDUS; /* optional */

	if (context->priv->UserLoggedOnPdu)
		extendedPdu |= RDPDR_USER_LOGGEDON_PDU; /* optional */

	extraFlags1 = 0;
	extraFlags1 |= ENABLE_ASYNCIO; /* optional */
	SpecialTypeDeviceCap = 0;

	Stream_EnsureRemainingCapacity(s, header.CapabilityLength);

	rdpdr_server_write_capability_set_header(s, &header);

	Stream_Write_UINT32(s, 0); /* osType (4 bytes), ignored on receipt */
	Stream_Write_UINT32(s, 0); /* osVersion (4 bytes), unused and must be set to zero */
	Stream_Write_UINT16(s, context->priv->VersionMajor); /* protocolMajorVersion (2 bytes) */
	Stream_Write_UINT16(s, context->priv->VersionMinor); /* protocolMinorVersion (2 bytes) */
	Stream_Write_UINT32(s, ioCode1); /* ioCode1 (4 bytes) */
	Stream_Write_UINT32(s, 0); /* ioCode2 (4 bytes), must be set to zero, reserved for future use */
	Stream_Write_UINT32(s, extendedPdu); /* extendedPdu (4 bytes) */
	Stream_Write_UINT32(s, extraFlags1); /* extraFlags1 (4 bytes) */
	Stream_Write_UINT32(s, 0); /* extraFlags2 (4 bytes), must be set to zero, reserved for future use */
	Stream_Write_UINT32(s, SpecialTypeDeviceCap); /* SpecialTypeDeviceCap (4 bytes) */

	return 0;
}

static int rdpdr_server_read_printer_capability_set(RdpdrServerContext* context, wStream* s, RDPDR_CAPABILITY_HEADER* header)
{
	return 0;
}

static int rdpdr_server_write_printer_capability_set(RdpdrServerContext* context, wStream* s)
{
	RDPDR_CAPABILITY_HEADER header;

	header.CapabilityType = CAP_PRINTER_TYPE;
	header.CapabilityLength = RDPDR_CAPABILITY_HEADER_LENGTH;
	header.Version = PRINT_CAPABILITY_VERSION_01;

    if (!Stream_EnsureRemainingCapacity(s, header.CapabilityLength))
		return -1;

	rdpdr_server_write_capability_set_header(s, &header);

	return 0;
}

static int rdpdr_server_read_port_capability_set(RdpdrServerContext* context, wStream* s, RDPDR_CAPABILITY_HEADER* header)
{
	return 0;
}

static int rdpdr_server_write_port_capability_set(RdpdrServerContext* context, wStream* s)
{
	RDPDR_CAPABILITY_HEADER header;

	header.CapabilityType = CAP_PORT_TYPE;
	header.CapabilityLength = RDPDR_CAPABILITY_HEADER_LENGTH;
	header.Version = PORT_CAPABILITY_VERSION_01;

    if (!Stream_EnsureRemainingCapacity(s, header.CapabilityLength))
		return -1;

	rdpdr_server_write_capability_set_header(s, &header);

	return 0;
}

static int rdpdr_server_read_drive_capability_set(RdpdrServerContext* context, wStream* s, RDPDR_CAPABILITY_HEADER* header)
{
	return 0;
}

static int rdpdr_server_write_drive_capability_set(RdpdrServerContext* context, wStream* s)
{
	RDPDR_CAPABILITY_HEADER header;

	header.CapabilityType = CAP_DRIVE_TYPE;
	header.CapabilityLength = RDPDR_CAPABILITY_HEADER_LENGTH;
	header.Version = DRIVE_CAPABILITY_VERSION_02;

    if (!Stream_EnsureRemainingCapacity(s, header.CapabilityLength))
		return -1;

	rdpdr_server_write_capability_set_header(s, &header);

	return 0;
}

static int rdpdr_server_read_smartcard_capability_set(RdpdrServerContext* context, wStream* s, RDPDR_CAPABILITY_HEADER* header)
{
	return 0;
}

static int rdpdr_server_write_smartcard_capability_set(RdpdrServerContext* context, wStream* s)
{
	RDPDR_CAPABILITY_HEADER header;

	header.CapabilityType = CAP_SMARTCARD_TYPE;
	header.CapabilityLength = RDPDR_CAPABILITY_HEADER_LENGTH;
	header.Version = SMARTCARD_CAPABILITY_VERSION_01;

    if (!Stream_EnsureRemainingCapacity(s, header.CapabilityLength))
		return -1;

	rdpdr_server_write_capability_set_header(s, &header);

	return 0;
}

static int rdpdr_server_send_core_capability_request(RdpdrServerContext* context)
{
	wStream* s;
	BOOL status = FALSE;
	RDPDR_HEADER header;
	UINT16 numCapabilities;
	ULONG written;

	WLog_DBG(TAG, "RdpdrServerSendCoreCapabilityRequest"); fflush(stdout);

	header.Component = RDPDR_CTYP_CORE;
	header.PacketId = PAKID_CORE_SERVER_CAPABILITY;

	numCapabilities = 1;

	if (context->supportsDrives)
		numCapabilities++;
	if (context->supportsPorts)
		numCapabilities++;
	if (context->supportsPrinters)
		numCapabilities++;
	if (context->supportsSmartcards)
		numCapabilities++;

	s = Stream_New(NULL, RDPDR_HEADER_LENGTH + 512);

	if (!s)
		return -1;

	Stream_Write_UINT16(s, header.Component); /* Component (2 bytes) */
	Stream_Write_UINT16(s, header.PacketId); /* PacketId (2 bytes) */
	Stream_Write_UINT16(s, numCapabilities); /* numCapabilities (2 bytes) */
	Stream_Write_UINT16(s, 0); /* Padding (2 bytes) */

	rdpdr_server_write_general_capability_set(context, s);
	if (context->supportsDrives)
	{
		rdpdr_server_write_drive_capability_set(context, s);
	}
	if (context->supportsPorts)
	{
		rdpdr_server_write_port_capability_set(context, s);
	}
	if (context->supportsPrinters)
	{
		rdpdr_server_write_printer_capability_set(context, s);
	}
	if (context->supportsSmartcards)
	{
		rdpdr_server_write_smartcard_capability_set(context, s);
	}

	Stream_SealLength(s);

	winpr_HexDump(TAG, WLOG_DEBUG, Stream_Buffer(s), Stream_Length(s));

	status = WTSVirtualChannelWrite(context->priv->ChannelHandle, (PCHAR) Stream_Buffer(s), Stream_Length(s), &written);

	Stream_Free(s, TRUE);

	return 0;
}

static int rdpdr_server_receive_core_capability_response(RdpdrServerContext* context, wStream* s, RDPDR_HEADER* header)
{
	int i;
	UINT16 numCapabilities;
	RDPDR_CAPABILITY_HEADER capabilityHeader;

	Stream_Read_UINT16(s, numCapabilities); /* numCapabilities (2 bytes) */
	Stream_Seek_UINT16(s); /* Padding (2 bytes) */

	for (i = 0; i < numCapabilities; i++)
	{
		rdpdr_server_read_capability_set_header(s, &capabilityHeader);

		switch (capabilityHeader.CapabilityType)
		{
			case CAP_GENERAL_TYPE:
				rdpdr_server_read_general_capability_set(context, s, &capabilityHeader);
				break;

			case CAP_PRINTER_TYPE:
				rdpdr_server_read_printer_capability_set(context, s, &capabilityHeader);
				break;

			case CAP_PORT_TYPE:
				rdpdr_server_read_port_capability_set(context, s, &capabilityHeader);
				break;

			case CAP_DRIVE_TYPE:
				rdpdr_server_read_drive_capability_set(context, s, &capabilityHeader);
				break;

			case CAP_SMARTCARD_TYPE:
				rdpdr_server_read_smartcard_capability_set(context, s, &capabilityHeader);
				break;

			default:
				WLog_DBG(TAG, "Unknown capabilityType %d", capabilityHeader.CapabilityType);
				Stream_Seek(s, capabilityHeader.CapabilityLength - RDPDR_CAPABILITY_HEADER_LENGTH);
				break;
		}
	}

	return 0;
}

static int rdpdr_server_send_client_id_confirm(RdpdrServerContext* context)
{
	wStream* s;
	BOOL status;
	RDPDR_HEADER header;
	ULONG written;

	WLog_DBG(TAG, "RdpdrServerSendClientIdConfirm"); fflush(stdout);

	header.Component = RDPDR_CTYP_CORE;
	header.PacketId = PAKID_CORE_CLIENTID_CONFIRM;

	s = Stream_New(NULL, RDPDR_HEADER_LENGTH + 8);

	Stream_Write_UINT16(s, header.Component); /* Component (2 bytes) */
	Stream_Write_UINT16(s, header.PacketId); /* PacketId (2 bytes) */
	Stream_Write_UINT16(s, context->priv->VersionMajor); /* VersionMajor (2 bytes) */
	Stream_Write_UINT16(s, context->priv->VersionMinor); /* VersionMinor (2 bytes) */
	Stream_Write_UINT32(s, context->priv->ClientId); /* ClientId (4 bytes) */

	Stream_SealLength(s);

	winpr_HexDump(TAG, WLOG_DEBUG, Stream_Buffer(s), Stream_Length(s));

	status = WTSVirtualChannelWrite(context->priv->ChannelHandle, (PCHAR) Stream_Buffer(s), Stream_Length(s), &written);

	Stream_Free(s, TRUE);

	return 0;
}

static int rdpdr_server_receive_device_list_announce_request(RdpdrServerContext* context, wStream* s, RDPDR_HEADER* header)
{
	int i;
	UINT32 DeviceCount;
	UINT32 DeviceType;
	UINT32 DeviceId;
	char PreferredDosName[9];
	UINT32 DeviceDataLength;
	BYTE* DeviceData;

	Stream_Read_UINT32(s, DeviceCount); /* DeviceCount (4 bytes) */

	WLog_DBG(TAG, "DeviceCount: %d", DeviceCount);

	for (i = 0; i < DeviceCount; i++)
	{
		ZeroMemory(PreferredDosName, sizeof(PreferredDosName));

		Stream_Read_UINT32(s, DeviceType); /* DeviceType (4 bytes) */
		Stream_Read_UINT32(s, DeviceId); /* DeviceId (4 bytes) */
		Stream_Read(s, PreferredDosName, 8); /* PreferredDosName (8 bytes) */
		Stream_Read_UINT32(s, DeviceDataLength); /* DeviceDataLength (4 bytes) */

		DeviceData = Stream_Pointer(s);

		WLog_DBG(TAG, "Device %d Name: %s Id: 0x%04X DataLength: %d",
				 i, PreferredDosName, DeviceId, DeviceDataLength);

		switch (DeviceType)
		{
			case RDPDR_DTYP_FILESYSTEM:
				if (context->supportsDrives)
				{
					IFCALL(context->OnDriveCreate, context, DeviceId, PreferredDosName);
				}
				break;

			case RDPDR_DTYP_PRINT:
				if (context->supportsPrinters)
				{
					IFCALL(context->OnPrinterCreate, context, DeviceId, PreferredDosName);
				}
				break;

			case RDPDR_DTYP_SERIAL:
			case RDPDR_DTYP_PARALLEL:
				if (context->supportsPorts)
				{
					IFCALL(context->OnPortCreate, context, DeviceId, PreferredDosName);
				}
				break;

			case RDPDR_DTYP_SMARTCARD:
				if (context->supportsSmartcards)
				{
					IFCALL(context->OnSmartcardCreate, context, DeviceId, PreferredDosName);
				}
				break;

			default:
				break;
		}

		Stream_Seek(s, DeviceDataLength);
	}

	fflush(stdout);

	return 0;
}

static int rdpdr_server_receive_device_list_remove_request(RdpdrServerContext* context, wStream* s, RDPDR_HEADER* header)
{
	int i;
	UINT32 DeviceCount;
	UINT32 DeviceType;
	UINT32 DeviceId;

	Stream_Read_UINT32(s, DeviceCount); /* DeviceCount (4 bytes) */

	WLog_DBG(TAG, "DeviceCount: %d", DeviceCount);

	for (i = 0; i < DeviceCount; i++)
	{
		Stream_Read_UINT32(s, DeviceId); /* DeviceId (4 bytes) */

		WLog_DBG(TAG, "Device %d Id: 0x%04X", i, DeviceId);

		DeviceType = 0; /* TODO: Save the device type on the announce request. */

		switch (DeviceType)
		{
			case RDPDR_DTYP_FILESYSTEM:
				if (context->supportsDrives)
				{
					IFCALL(context->OnDriveDelete, context, DeviceId);
				}
				break;

			case RDPDR_DTYP_PRINT:
				if (context->supportsPrinters)
				{
					IFCALL(context->OnPrinterDelete, context, DeviceId);
				}
				break;

			case RDPDR_DTYP_SERIAL:
			case RDPDR_DTYP_PARALLEL:
				if (context->supportsPorts)
				{
					IFCALL(context->OnPortDelete, context, DeviceId);
				}
				break;

			case RDPDR_DTYP_SMARTCARD:
				if (context->supportsSmartcards)
				{
					IFCALL(context->OnSmartcardDelete, context, DeviceId);
				}
				break;

			default:
				break;
		}
	}

	return 0;
}

static int rdpdr_server_receive_device_io_completion(RdpdrServerContext* context, wStream* s, RDPDR_HEADER* header)
{
	UINT32 deviceId;
	UINT32 completionId;
	UINT32 ioStatus;
	RDPDR_IRP* irp;

	Stream_Read_UINT32(s, deviceId);
	Stream_Read_UINT32(s, completionId);
	Stream_Read_UINT32(s, ioStatus);

	WLog_DBG(TAG, "deviceId=%d, completionId=0x%x, ioStatus=0x%x", deviceId, completionId, ioStatus);

	irp = rdpdr_server_dequeue_irp(context, completionId);
	if (irp == NULL)
	{
		WLog_ERR(TAG, "IRP not found for completionId=0x%x", completionId);
		return -1;
	}

	/* Invoke the callback. */
	if (irp->Callback)
	{
		(*irp->Callback)(context, s, irp, deviceId, completionId, ioStatus);
	}

	return 0;
}

static int rdpdr_server_send_user_logged_on(RdpdrServerContext* context)
{
	wStream* s;
	BOOL status;
	RDPDR_HEADER header;
	ULONG written;

	WLog_DBG(TAG, "%s", __FUNCTION__);
	fflush(stdout);

	header.Component = RDPDR_CTYP_CORE;
	header.PacketId = PAKID_CORE_USER_LOGGEDON;

	s = Stream_New(NULL, RDPDR_HEADER_LENGTH);

	Stream_Write_UINT16(s, header.Component); /* Component (2 bytes) */
	Stream_Write_UINT16(s, header.PacketId); /* PacketId (2 bytes) */

	Stream_SealLength(s);

	winpr_HexDump(TAG, WLOG_DEBUG, Stream_Buffer(s), Stream_Length(s));

	status = WTSVirtualChannelWrite(context->priv->ChannelHandle, (PCHAR) Stream_Buffer(s), Stream_Length(s), &written);

	Stream_Free(s, TRUE);

	return 0;
}

static int rdpdr_server_receive_pdu(RdpdrServerContext* context, wStream* s, RDPDR_HEADER* header)
{
	WLog_DBG(TAG, "RdpdrServerReceivePdu: Component: 0x%04X PacketId: 0x%04X",
			 header->Component, header->PacketId);
	winpr_HexDump(TAG, WLOG_DEBUG, Stream_Buffer(s), Stream_Length(s));
	fflush(stdout);

	if (header->Component == RDPDR_CTYP_CORE)
	{
		switch (header->PacketId)
		{
			case PAKID_CORE_CLIENTID_CONFIRM:
				rdpdr_server_receive_announce_response(context, s, header);
				break;

			case PAKID_CORE_CLIENT_NAME:
				rdpdr_server_receive_client_name_request(context, s, header);
				rdpdr_server_send_core_capability_request(context);
				rdpdr_server_send_client_id_confirm(context);
				break;

			case PAKID_CORE_CLIENT_CAPABILITY:
				rdpdr_server_receive_core_capability_response(context, s, header);

				if (context->priv->UserLoggedOnPdu)
					rdpdr_server_send_user_logged_on(context);

				break;

			case PAKID_CORE_DEVICELIST_ANNOUNCE:
				rdpdr_server_receive_device_list_announce_request(context, s, header);
				break;

			case PAKID_CORE_DEVICE_REPLY:
				break;

			case PAKID_CORE_DEVICE_IOREQUEST:
				break;

			case PAKID_CORE_DEVICE_IOCOMPLETION:
				rdpdr_server_receive_device_io_completion(context, s, header);
				break;

			case PAKID_CORE_DEVICELIST_REMOVE:
				rdpdr_server_receive_device_list_remove_request(context, s, header);
				break;

			default:
				break;
		}
	}
	else if (header->Component == RDPDR_CTYP_PRN)
	{
		switch (header->PacketId)
		{
			case PAKID_PRN_CACHE_DATA:
				break;

			case PAKID_PRN_USING_XPS:
				break;

			default:
				break;
		}
	}
	else
	{
		WLog_WARN(TAG, "Unknown RDPDR_HEADER.Component: 0x%04X", header->Component);
		return -1;
	}

	return 0;
}

static void* rdpdr_server_thread(void* arg)
{
	wStream* s;
	DWORD status;
	DWORD nCount;
	void* buffer;
	int position;
	HANDLE events[8];
	RDPDR_HEADER header;
	HANDLE ChannelEvent;
	DWORD BytesReturned;
	RdpdrServerContext* context;

	context = (RdpdrServerContext*) arg;
	buffer = NULL;
	BytesReturned = 0;
	ChannelEvent = NULL;

	s = Stream_New(NULL, 4096);

	if (WTSVirtualChannelQuery(context->priv->ChannelHandle, WTSVirtualEventHandle, &buffer, &BytesReturned) == TRUE)
	{
		if (BytesReturned == sizeof(HANDLE))
			CopyMemory(&ChannelEvent, buffer, sizeof(HANDLE));

		WTSFreeMemory(buffer);
	}

	nCount = 0;
	events[nCount++] = ChannelEvent;
	events[nCount++] = context->priv->StopEvent;

	rdpdr_server_send_announce_request(context);

	while (1)
	{
		BytesReturned = 0;
		status = WaitForMultipleObjects(nCount, events, FALSE, INFINITE);

		if (WaitForSingleObject(context->priv->StopEvent, 0) == WAIT_OBJECT_0)
		{
			break;
		}

#if 0
		WTSVirtualChannelRead(context->priv->ChannelHandle, 0, NULL, 0, &BytesReturned);

		if (BytesReturned < 1)
			continue;

		Stream_EnsureRemainingCapacity(s, BytesReturned);
#endif

		if (!WTSVirtualChannelRead(context->priv->ChannelHandle, 0,
								   (PCHAR) Stream_Buffer(s), Stream_Capacity(s), &BytesReturned))
		{
			break;
		}

		if (BytesReturned >= RDPDR_HEADER_LENGTH)
		{
			position = Stream_GetPosition(s);
			Stream_SetPosition(s, 0);
			Stream_SetLength(s, BytesReturned);
			while (Stream_GetRemainingLength(s) >= RDPDR_HEADER_LENGTH)
			{
				Stream_Read_UINT16(s, header.Component); /* Component (2 bytes) */
				Stream_Read_UINT16(s, header.PacketId); /* PacketId (2 bytes) */
				rdpdr_server_receive_pdu(context, s, &header);
			}
		}
	}

	Stream_Free(s, TRUE);
	return NULL;
}

static int rdpdr_server_start(RdpdrServerContext* context)
{
	context->priv->ChannelHandle = WTSVirtualChannelOpen(context->vcm, WTS_CURRENT_SESSION, "rdpdr");

	if (!context->priv->ChannelHandle)
		return -1;

	context->priv->StopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
	context->priv->Thread = CreateThread(NULL, 0,
										 (LPTHREAD_START_ROUTINE) rdpdr_server_thread, (void*) context, 0, NULL);
	return 0;
}

static int rdpdr_server_stop(RdpdrServerContext* context)
{
	SetEvent(context->priv->StopEvent);
	WaitForSingleObject(context->priv->Thread, INFINITE);
	CloseHandle(context->priv->Thread);
	return 0;
}

static void rdpdr_server_write_device_iorequest(
	wStream* s,
	UINT32 deviceId,
	UINT32 fileId,
	UINT32 completionId,
	UINT32 majorFunction,
	UINT32 minorFunction
)
{
	Stream_Write_UINT16(s, RDPDR_CTYP_CORE); /* Component (2 bytes) */
	Stream_Write_UINT16(s, PAKID_CORE_DEVICE_IOREQUEST); /* PacketId (2 bytes) */
	Stream_Write_UINT32(s, deviceId); /* DeviceId (4 bytes) */
	Stream_Write_UINT32(s, fileId); /* FileId (4 bytes) */
	Stream_Write_UINT32(s, completionId); /* CompletionId (4 bytes) */
	Stream_Write_UINT32(s, majorFunction); /* MajorFunction (4 bytes) */
	Stream_Write_UINT32(s, minorFunction); /* MinorFunction (4 bytes) */
}

static BOOL rdpdr_server_read_file_directory_information(wStream* s, FILE_DIRECTORY_INFORMATION* fdi)
{
	UINT32 fileNameLength;

	ZeroMemory(fdi, sizeof(FILE_DIRECTORY_INFORMATION));

	Stream_Read_UINT32(s, fdi->NextEntryOffset); /* NextEntryOffset (4 bytes) */
	Stream_Read_UINT32(s, fdi->FileIndex); /* FileIndex (4 bytes) */
	Stream_Read_UINT64(s, fdi->CreationTime); /* CreationTime (8 bytes) */
	Stream_Read_UINT64(s, fdi->LastAccessTime); /* LastAccessTime (8 bytes) */
	Stream_Read_UINT64(s, fdi->LastWriteTime); /* LastWriteTime (8 bytes) */
	Stream_Read_UINT64(s, fdi->ChangeTime); /* ChangeTime (8 bytes) */
	Stream_Read_UINT64(s, fdi->EndOfFile); /* EndOfFile (8 bytes) */
	Stream_Read_UINT64(s, fdi->AllocationSize); /* AllocationSize (8 bytes) */
	Stream_Read_UINT32(s, fdi->FileAttributes); /* FileAttributes (4 bytes) */
	Stream_Read_UINT32(s, fileNameLength); /* FileNameLength (4 bytes) */

	WideCharToMultiByte(CP_ACP, 0, (LPCWSTR) Stream_Pointer(s), fileNameLength / 2, fdi->FileName, sizeof(fdi->FileName), NULL, NULL);
	Stream_Seek(s, fileNameLength);

	return TRUE;
}

static BOOL rdpdr_server_send_device_create_request(
	RdpdrServerContext* context,
	UINT32 deviceId,
	UINT32 completionId,
	const char* path,
	UINT32 desiredAccess,
	UINT32 createOptions,
	UINT32 createDisposition
)
{
	int pathLength;
	ULONG written;
	BOOL status;
	wStream* s;

	WLog_DBG(TAG, "%s: deviceId=%d, path=%s, desiredAccess=0x%x createOptions=0x%x createDisposition=0x%x",
		__FUNCTION__, deviceId, path, desiredAccess, createOptions, createDisposition);

	/* Compute the required Unicode size. */
	pathLength = (strlen(path) + 1) * sizeof(WCHAR);

	s = Stream_New(NULL, 256 + pathLength);

	rdpdr_server_write_device_iorequest(s, deviceId, 0, completionId, IRP_MJ_CREATE, 0);

	Stream_Write_UINT32(s, desiredAccess); /* DesiredAccess (4 bytes) */
    Stream_Write_UINT32(s, 0); /* AllocationSize (8 bytes) */
    Stream_Write_UINT32(s, 0);
    Stream_Write_UINT32(s, 0); /* FileAttributes (4 bytes) */
    Stream_Write_UINT32(s, 3); /* SharedAccess (4 bytes) */
    Stream_Write_UINT32(s, createDisposition); /* CreateDisposition (4 bytes) */
    Stream_Write_UINT32(s, createOptions); /* CreateOptions (4 bytes) */
    Stream_Write_UINT32(s, pathLength); /* PathLength (4 bytes) */

	/* Convert the path to Unicode. */
	MultiByteToWideChar(CP_ACP, 0, path, -1, (LPWSTR) Stream_Pointer(s), pathLength);
	Stream_Seek(s, pathLength);

    Stream_SealLength(s);

	status = WTSVirtualChannelWrite(context->priv->ChannelHandle, (PCHAR) Stream_Buffer(s), Stream_Length(s), &written);

    Stream_Free(s, TRUE);

    return status;
}

static BOOL rdpdr_server_send_device_close_request(
	RdpdrServerContext* context,
	UINT32 deviceId,
	UINT32 fileId,
	UINT32 completionId
)
{
	ULONG written;
	BOOL status;
	wStream* s;

	WLog_DBG(TAG, "%s: deviceId=%d, fileId=%d", __FUNCTION__, deviceId, fileId);

	s = Stream_New(NULL, 128);

	rdpdr_server_write_device_iorequest(s, deviceId, fileId, completionId, IRP_MJ_CLOSE, 0);

	Stream_Zero(s, 32); /* Padding (32 bytes) */

    Stream_SealLength(s);

	status = WTSVirtualChannelWrite(context->priv->ChannelHandle, (PCHAR) Stream_Buffer(s), Stream_Length(s), &written);

    Stream_Free(s, TRUE);

    return status;
}

static BOOL rdpdr_server_send_device_read_request(
	RdpdrServerContext* context,
	UINT32 deviceId,
	UINT32 fileId,
	UINT32 completionId,
	UINT32 length,
	UINT32 offset
)
{
	ULONG written;
	BOOL status;
	wStream* s;

	WLog_DBG(TAG, "%s: deviceId=%d, fileId=%d, length=%d, offset=%d", __FUNCTION__, deviceId, fileId, length, offset);

	s = Stream_New(NULL, 128);

	rdpdr_server_write_device_iorequest(s, deviceId, fileId, completionId, IRP_MJ_READ, 0);

	Stream_Write_UINT32(s, length); /* Length (4 bytes) */
	Stream_Write_UINT32(s, offset); /* Offset (8 bytes) */
	Stream_Write_UINT32(s, 0);
	Stream_Zero(s, 20); /* Padding (20 bytes) */

    Stream_SealLength(s);

	status = WTSVirtualChannelWrite(context->priv->ChannelHandle, (PCHAR) Stream_Buffer(s), Stream_Length(s), &written);

    Stream_Free(s, TRUE);

    return status;
}

static BOOL rdpdr_server_send_device_write_request(
	RdpdrServerContext* context,
	UINT32 deviceId,
	UINT32 fileId,
	UINT32 completionId,
	const char* data,
	UINT32 length,
	UINT32 offset
)
{
	ULONG written;
	BOOL status;
	wStream* s;

	WLog_DBG(TAG, "%s: deviceId=%d, fileId=%d, length=%d, offset=%d", __FUNCTION__, deviceId, fileId, length, offset);

	s = Stream_New(NULL, 64 + length);

	rdpdr_server_write_device_iorequest(s, deviceId, fileId, completionId, IRP_MJ_WRITE, 0);

	Stream_Write_UINT32(s, length); /* Length (4 bytes) */
	Stream_Write_UINT32(s, offset); /* Offset (8 bytes) */
	Stream_Write_UINT32(s, 0);
	Stream_Zero(s, 20); /* Padding (20 bytes) */
	Stream_Write(s, data, length); /* WriteData (variable) */

    Stream_SealLength(s);

	status = WTSVirtualChannelWrite(context->priv->ChannelHandle, (PCHAR) Stream_Buffer(s), Stream_Length(s), &written);

    Stream_Free(s, TRUE);

    return status;
}

static BOOL rdpdr_server_send_device_query_directory_request(
	RdpdrServerContext* context,
	UINT32 deviceId,
	UINT32 fileId,
	UINT32 completionId,
	const char* path
)
{
	int pathLength;
	ULONG written;
	BOOL status;
	wStream* s;

	WLog_DBG(TAG, "%s: deviceId=%d, fileId=%d, path=%s", __FUNCTION__, deviceId, fileId, path);

	/* Compute the required Unicode size. */
	pathLength = path ? (strlen(path) + 1) * sizeof(WCHAR) : 0;

	s = Stream_New(NULL, 64 + pathLength);

	rdpdr_server_write_device_iorequest(s, deviceId, fileId, completionId, IRP_MJ_DIRECTORY_CONTROL, IRP_MN_QUERY_DIRECTORY);

	Stream_Write_UINT32(s, FileDirectoryInformation); /* FsInformationClass (4 bytes) */
	Stream_Write_UINT8(s, path ? 1 : 0); /* InitialQuery (1 byte) */
	Stream_Write_UINT32(s, pathLength); /* PathLength (4 bytes) */
	Stream_Zero(s, 23); /* Padding (23 bytes) */

	/* Convert the path to Unicode. */
	if (pathLength > 0)
	{
		MultiByteToWideChar(CP_ACP, 0, path, -1, (LPWSTR) Stream_Pointer(s), pathLength);
		Stream_Seek(s, pathLength);
	}

    Stream_SealLength(s);

	status = WTSVirtualChannelWrite(context->priv->ChannelHandle, (PCHAR) Stream_Buffer(s), Stream_Length(s), &written);

    Stream_Free(s, TRUE);

    return status;
}

static BOOL rdpdr_server_send_device_file_rename_request(
	RdpdrServerContext* context,
	UINT32 deviceId,
	UINT32 fileId,
	UINT32 completionId,
	const char* path
)
{
	int pathLength;
	ULONG written;
	BOOL status;
	wStream* s;

	WLog_DBG(TAG, "%s: deviceId=%d, fileId=%d, path=%s", __FUNCTION__, deviceId, fileId, path);

	/* Compute the required Unicode size. */
	pathLength = path ? (strlen(path) + 1) * sizeof(WCHAR) : 0;

	s = Stream_New(NULL, 64 + pathLength);

	rdpdr_server_write_device_iorequest(s, deviceId, fileId, completionId, IRP_MJ_SET_INFORMATION, 0);

	Stream_Write_UINT32(s, FileRenameInformation); /* FsInformationClass (4 bytes) */
	Stream_Write_UINT32(s, pathLength + 6); /* Length (4 bytes) */
	Stream_Zero(s, 24); /* Padding (24 bytes) */

	/* RDP_FILE_RENAME_INFORMATION */
	Stream_Write_UINT8(s, 0); /* ReplaceIfExists (1 byte) */
	Stream_Write_UINT8(s, 0); /* RootDirectory (1 byte) */
	Stream_Write_UINT32(s, pathLength); /* FileNameLength (4 bytes) */

	/* Convert the path to Unicode. */
	if (pathLength > 0)
	{
		MultiByteToWideChar(CP_ACP, 0, path, -1, (LPWSTR) Stream_Pointer(s), pathLength);
		Stream_Seek(s, pathLength);
	}

    Stream_SealLength(s);

	status = WTSVirtualChannelWrite(context->priv->ChannelHandle, (PCHAR) Stream_Buffer(s), Stream_Length(s), &written);

    Stream_Free(s, TRUE);

    return status;
}

static void rdpdr_server_convert_slashes(char* path, int size)
{
	int i;

	for (i = 0; (i < size) && (path[i] != '\0'); i++)
	{
		if (path[i] == '/')
			path[i] = '\\';
	}
}

/*************************************************
 * Drive Create Directory
 ************************************************/

static void rdpdr_server_drive_create_directory_callback2(RdpdrServerContext* context, wStream* s, RDPDR_IRP* irp, UINT32 deviceId, UINT32 completionId, UINT32 ioStatus)
{
	WLog_DBG(TAG, "%s: deviceId=%d, completionId=%d, ioStatus=0x%x", __FUNCTION__, deviceId, completionId, ioStatus);

	/* Invoke the create directory completion routine. */
	context->OnDriveCreateDirectoryComplete(context, irp->CallbackData, ioStatus);

	/* Destroy the IRP. */
	rdpdr_server_irp_free(irp);
}

static void rdpdr_server_drive_create_directory_callback1(RdpdrServerContext* context, wStream* s, RDPDR_IRP* irp, UINT32 deviceId, UINT32 completionId, UINT32 ioStatus)
{
	UINT32 fileId;
	UINT8 information;

	WLog_DBG(TAG, "%s: deviceId=%d, completionId=%d, ioStatus=0x%x", __FUNCTION__, deviceId, completionId, ioStatus);

	if (ioStatus != STATUS_SUCCESS)
	{
		/* Invoke the create directory completion routine. */
		context->OnDriveCreateDirectoryComplete(context, irp->CallbackData, ioStatus);

		/* Destroy the IRP. */
		rdpdr_server_irp_free(irp);

		return;
	}

	Stream_Read_UINT32(s, fileId); /* FileId (4 bytes) */
	Stream_Read_UINT8(s, information); /* Information (1 byte) */

	/* Setup the IRP. */
	irp->CompletionId = context->priv->NextCompletionId++;
	irp->Callback = rdpdr_server_drive_create_directory_callback2;
	irp->DeviceId = deviceId;
	irp->FileId = fileId;

	rdpdr_server_enqueue_irp(context, irp);

	/* Send a request to close the file */
	rdpdr_server_send_device_close_request(context, deviceId, fileId, irp->CompletionId);
}

static BOOL rdpdr_server_drive_create_directory(RdpdrServerContext* context, void* callbackData, UINT32 deviceId, const char* path)
{
	RDPDR_IRP* irp;

	/* Create an IRP. */
	irp = rdpdr_server_irp_new();
	if (irp == NULL) return FALSE;

	irp->CompletionId = context->priv->NextCompletionId++;
	irp->Callback = rdpdr_server_drive_create_directory_callback1;
	irp->CallbackData = callbackData;
	irp->DeviceId = deviceId;
	strncpy(irp->PathName, path, sizeof(irp->PathName));

	rdpdr_server_convert_slashes(irp->PathName, sizeof(irp->PathName));

	printf("PathName=%s\n", irp->PathName); fflush(stdout);

	rdpdr_server_enqueue_irp(context, irp);

	/* Send a request to open the file. */
	rdpdr_server_send_device_create_request(
		context, deviceId, irp->CompletionId, irp->PathName,
		FILE_READ_DATA | SYNCHRONIZE,
		FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT,
		FILE_CREATE);

	return TRUE;
}

/*************************************************
 * Drive Delete Directory
 ************************************************/

static void rdpdr_server_drive_delete_directory_callback2(RdpdrServerContext* context, wStream* s, RDPDR_IRP* irp, UINT32 deviceId, UINT32 completionId, UINT32 ioStatus)
{
	WLog_DBG(TAG, "%s: deviceId=%d, completionId=%d, ioStatus=0x%x", __FUNCTION__, deviceId, completionId, ioStatus);

	/* Invoke the delete directory completion routine. */
	context->OnDriveDeleteDirectoryComplete(context, irp->CallbackData, ioStatus);

	/* Destroy the IRP. */
	rdpdr_server_irp_free(irp);
}

static void rdpdr_server_drive_delete_directory_callback1(RdpdrServerContext* context, wStream* s, RDPDR_IRP* irp, UINT32 deviceId, UINT32 completionId, UINT32 ioStatus)
{
	UINT32 fileId;
	UINT8 information;

	WLog_DBG(TAG, "%s: deviceId=%d, completionId=%d, ioStatus=0x%x", __FUNCTION__, deviceId, completionId, ioStatus);

	if (ioStatus != STATUS_SUCCESS)
	{
		/* Invoke the delete directory completion routine. */
		context->OnDriveDeleteFileComplete(context, irp->CallbackData, ioStatus);

		/* Destroy the IRP. */
		rdpdr_server_irp_free(irp);

		return;
	}

	Stream_Read_UINT32(s, fileId); /* FileId (4 bytes) */
	Stream_Read_UINT8(s, information); /* Information (1 byte) */

	/* Setup the IRP. */
	irp->CompletionId = context->priv->NextCompletionId++;
	irp->Callback = rdpdr_server_drive_delete_directory_callback2;
	irp->DeviceId = deviceId;
	irp->FileId = fileId;

	rdpdr_server_enqueue_irp(context, irp);

	/* Send a request to close the file */
	rdpdr_server_send_device_close_request(context, deviceId, fileId, irp->CompletionId);
}

static BOOL rdpdr_server_drive_delete_directory(RdpdrServerContext* context, void* callbackData, UINT32 deviceId, const char* path)
{
	RDPDR_IRP* irp;

	/* Create an IRP. */
	irp = rdpdr_server_irp_new();
	if (irp == NULL) return FALSE;

	irp->CompletionId = context->priv->NextCompletionId++;
	irp->Callback = rdpdr_server_drive_delete_directory_callback1;
	irp->CallbackData = callbackData;
	irp->DeviceId = deviceId;
	strncpy(irp->PathName, path, sizeof(irp->PathName));

	rdpdr_server_convert_slashes(irp->PathName, sizeof(irp->PathName));

	printf("PathName=%s\n", irp->PathName); fflush(stdout);

	rdpdr_server_enqueue_irp(context, irp);

	/* Send a request to open the file. */
	rdpdr_server_send_device_create_request(
		context, deviceId, irp->CompletionId, irp->PathName,
		DELETE | SYNCHRONIZE,
		FILE_DIRECTORY_FILE | FILE_DELETE_ON_CLOSE | FILE_SYNCHRONOUS_IO_NONALERT,
		FILE_OPEN);

	return TRUE;
}

/*************************************************
 * Drive Query Directory
 ************************************************/

static void rdpdr_server_drive_query_directory_callback2(RdpdrServerContext* context, wStream* s, RDPDR_IRP* irp, UINT32 deviceId, UINT32 completionId, UINT32 ioStatus)
{
	FILE_DIRECTORY_INFORMATION fdi;
	UINT32 length;

	WLog_DBG(TAG, "%s: deviceId=%d, completionId=%d, ioStatus=0x%x", __FUNCTION__, deviceId, completionId, ioStatus);

	Stream_Read_UINT32(s, length); /* Length (4 bytes) */

	if (length > 0)
	{
		rdpdr_server_read_file_directory_information(s, &fdi);
	}
	else
	{
		Stream_Seek(s, 1); /* Padding (1 byte) */
	}

	if (ioStatus == STATUS_SUCCESS)
	{
		/* Invoke the query directory completion routine. */
		context->OnDriveQueryDirectoryComplete(context, irp->CallbackData, ioStatus, length > 0 ? &fdi : NULL);

		/* Setup the IRP. */
		irp->CompletionId = context->priv->NextCompletionId++;
		irp->Callback = rdpdr_server_drive_query_directory_callback2;

		rdpdr_server_enqueue_irp(context, irp);

		/* Send a request to query the directory. */
		rdpdr_server_send_device_query_directory_request(context, irp->DeviceId, irp->FileId, irp->CompletionId, NULL);
	}
	else
	{
		/* Invoke the query directory completion routine. */
		context->OnDriveQueryDirectoryComplete(context, irp->CallbackData, ioStatus, NULL);

		/* Destroy the IRP. */
		rdpdr_server_irp_free(irp);
	}
}

static void rdpdr_server_drive_query_directory_callback1(RdpdrServerContext* context, wStream* s, RDPDR_IRP* irp, UINT32 deviceId, UINT32 completionId, UINT32 ioStatus)
{
	UINT32 fileId;

	WLog_DBG(TAG, "%s: deviceId=%d, completionId=%d, ioStatus=0x%x", __FUNCTION__, deviceId, completionId, ioStatus);

	if (ioStatus != STATUS_SUCCESS)
	{
		/* Invoke the query directory completion routine. */
		context->OnDriveQueryDirectoryComplete(context, irp->CallbackData, ioStatus, NULL);

		/* Destroy the IRP. */
		rdpdr_server_irp_free(irp);

		return;
	}

	Stream_Read_UINT32(s, fileId);

	/* Setup the IRP. */
	irp->CompletionId = context->priv->NextCompletionId++;
	irp->Callback = rdpdr_server_drive_query_directory_callback2;
	irp->DeviceId = deviceId;
	irp->FileId = fileId;

	strcat(irp->PathName, "\\*.*");

	rdpdr_server_enqueue_irp(context, irp);

	/* Send a request to query the directory. */
	rdpdr_server_send_device_query_directory_request(context, deviceId, fileId, irp->CompletionId, irp->PathName);
}

static BOOL rdpdr_server_drive_query_directory(RdpdrServerContext* context, void* callbackData, UINT32 deviceId, const char* path)
{
	RDPDR_IRP* irp;

	/* Create an IRP. */
	irp = rdpdr_server_irp_new();
	if (irp == NULL) return FALSE;

	irp->CompletionId = context->priv->NextCompletionId++;
	irp->Callback = rdpdr_server_drive_query_directory_callback1;
	irp->CallbackData = callbackData;
	irp->DeviceId = deviceId;
	strncpy(irp->PathName, path, sizeof(irp->PathName));

	rdpdr_server_convert_slashes(irp->PathName, sizeof(irp->PathName));

	printf("PathName=%s\n", irp->PathName); fflush(stdout);

	rdpdr_server_enqueue_irp(context, irp);

	/* Send a request to open the directory. */
	rdpdr_server_send_device_create_request(
		context, deviceId, irp->CompletionId, irp->PathName,
		FILE_READ_DATA | SYNCHRONIZE,
		FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT,
		FILE_OPEN);

	return TRUE;
}

/*************************************************
 * Drive Open File
 ************************************************/

static void rdpdr_server_drive_open_file_callback(RdpdrServerContext* context, wStream* s, RDPDR_IRP* irp, UINT32 deviceId, UINT32 completionId, UINT32 ioStatus)
{
	UINT32 fileId;
	UINT8 information;

	WLog_DBG(TAG, "%s: deviceId=%d, completionId=%d, ioStatus=0x%x", __FUNCTION__, deviceId, completionId, ioStatus);

	Stream_Read_UINT32(s, fileId); /* FileId (4 bytes) */
	Stream_Read_UINT8(s, information); /* Information (1 byte) */

	/* Invoke the open file completion routine. */
	context->OnDriveOpenFileComplete(context, irp->CallbackData, ioStatus, deviceId, fileId);

	/* Destroy the IRP. */
	rdpdr_server_irp_free(irp);
}

static BOOL rdpdr_server_drive_open_file(RdpdrServerContext* context, void* callbackData, UINT32 deviceId, const char* path, UINT32 desiredAccess, UINT32 createDisposition)
{
	RDPDR_IRP* irp;

	/* Create an IRP. */
	irp = rdpdr_server_irp_new();
	if (irp == NULL) return FALSE;

	irp->CompletionId = context->priv->NextCompletionId++;
	irp->Callback = rdpdr_server_drive_open_file_callback;
	irp->CallbackData = callbackData;
	irp->DeviceId = deviceId;
	strncpy(irp->PathName, path, sizeof(irp->PathName));

	rdpdr_server_convert_slashes(irp->PathName, sizeof(irp->PathName));

	printf("PathName=%s\n", irp->PathName); fflush(stdout);

	rdpdr_server_enqueue_irp(context, irp);

	/* Send a request to open the file. */
	rdpdr_server_send_device_create_request(
		context, deviceId, irp->CompletionId, irp->PathName,
		desiredAccess | SYNCHRONIZE,
		FILE_SYNCHRONOUS_IO_NONALERT,
		createDisposition);

	return TRUE;
}

/*************************************************
 * Drive Read File
 ************************************************/

static void rdpdr_server_drive_read_file_callback(RdpdrServerContext* context, wStream* s, RDPDR_IRP* irp, UINT32 deviceId, UINT32 completionId, UINT32 ioStatus)
{
	UINT32 length;
	char* buffer = NULL;

	WLog_DBG(TAG, "%s: deviceId=%d, completionId=%d, ioStatus=0x%x", __FUNCTION__, deviceId, completionId, ioStatus);

	Stream_Read_UINT32(s, length); /* Length (4 bytes) */
	if (length > 0)
	{
		buffer = (char*) Stream_Pointer(s);
		Stream_Seek(s, length);
	}

	/* Invoke the read file completion routine. */
	context->OnDriveReadFileComplete(context, irp->CallbackData, ioStatus, buffer, length);

	/* Destroy the IRP. */
	rdpdr_server_irp_free(irp);
}

static BOOL rdpdr_server_drive_read_file(RdpdrServerContext* context, void* callbackData, UINT32 deviceId, UINT32 fileId, UINT32 length, UINT32 offset)
{
	RDPDR_IRP* irp;

	/* Create an IRP. */
	irp = rdpdr_server_irp_new();
	if (irp == NULL) return FALSE;

	irp->CompletionId = context->priv->NextCompletionId++;
	irp->Callback = rdpdr_server_drive_read_file_callback;
	irp->CallbackData = callbackData;
	irp->DeviceId = deviceId;
	irp->FileId = fileId;

	rdpdr_server_enqueue_irp(context, irp);

	/* Send a request to open the directory. */
	rdpdr_server_send_device_read_request(context, deviceId, fileId, irp->CompletionId,	length, offset);

	return TRUE;
}

/*************************************************
 * Drive Write File
 ************************************************/

static void rdpdr_server_drive_write_file_callback(RdpdrServerContext* context, wStream* s, RDPDR_IRP* irp, UINT32 deviceId, UINT32 completionId, UINT32 ioStatus)
{
	UINT32 length;

	WLog_DBG(TAG, "%s: deviceId=%d, completionId=%d, ioStatus=0x%x", __FUNCTION__, deviceId, completionId, ioStatus);

	Stream_Read_UINT32(s, length); /* Length (4 bytes) */
	Stream_Seek(s, 1); /* Padding (1 byte) */

	/* Invoke the write file completion routine. */
	context->OnDriveWriteFileComplete(context, irp->CallbackData, ioStatus, length);

	/* Destroy the IRP. */
	rdpdr_server_irp_free(irp);
}

static BOOL rdpdr_server_drive_write_file(RdpdrServerContext* context, void* callbackData, UINT32 deviceId, UINT32 fileId, const char* buffer, UINT32 length, UINT32 offset)
{
	RDPDR_IRP* irp;

	/* Create an IRP. */
	irp = rdpdr_server_irp_new();
	if (irp == NULL) return FALSE;

	irp->CompletionId = context->priv->NextCompletionId++;
	irp->Callback = rdpdr_server_drive_write_file_callback;
	irp->CallbackData = callbackData;
	irp->DeviceId = deviceId;
	irp->FileId = fileId;

	rdpdr_server_enqueue_irp(context, irp);

	/* Send a request to open the directory. */
	rdpdr_server_send_device_write_request(context, deviceId, fileId, irp->CompletionId, buffer, length, offset);

	return TRUE;
}

/*************************************************
 * Drive Close File
 ************************************************/

static void rdpdr_server_drive_close_file_callback(RdpdrServerContext* context, wStream* s, RDPDR_IRP* irp, UINT32 deviceId, UINT32 completionId, UINT32 ioStatus)
{
	WLog_DBG(TAG, "%s: deviceId=%d, completionId=%d, ioStatus=0x%x", __FUNCTION__, deviceId, completionId, ioStatus);

	/* Invoke the close file completion routine. */
	context->OnDriveCloseFileComplete(context, irp->CallbackData, ioStatus);

	/* Destroy the IRP. */
	rdpdr_server_irp_free(irp);
}

static BOOL rdpdr_server_drive_close_file(RdpdrServerContext* context, void* callbackData, UINT32 deviceId, UINT32 fileId)
{
	RDPDR_IRP* irp;

	/* Create an IRP. */
	irp = rdpdr_server_irp_new();
	if (irp == NULL) return FALSE;

	irp->CompletionId = context->priv->NextCompletionId++;
	irp->Callback = rdpdr_server_drive_close_file_callback;
	irp->CallbackData = callbackData;
	irp->DeviceId = deviceId;
	irp->FileId = fileId;

	rdpdr_server_enqueue_irp(context, irp);

	/* Send a request to open the directory. */
	rdpdr_server_send_device_close_request(context, deviceId, fileId, irp->CompletionId);

	return TRUE;
}

/*************************************************
 * Drive Delete File
 ************************************************/

static void rdpdr_server_drive_delete_file_callback2(RdpdrServerContext* context, wStream* s, RDPDR_IRP* irp, UINT32 deviceId, UINT32 completionId, UINT32 ioStatus)
{
	WLog_DBG(TAG, "%s: deviceId=%d, completionId=%d, ioStatus=0x%x", __FUNCTION__, deviceId, completionId, ioStatus);

	/* Invoke the delete file completion routine. */
	context->OnDriveDeleteFileComplete(context, irp->CallbackData, ioStatus);

	/* Destroy the IRP. */
	rdpdr_server_irp_free(irp);
}

static void rdpdr_server_drive_delete_file_callback1(RdpdrServerContext* context, wStream* s, RDPDR_IRP* irp, UINT32 deviceId, UINT32 completionId, UINT32 ioStatus)
{
	UINT32 fileId;
	UINT8 information;

	WLog_DBG(TAG, "%s: deviceId=%d, completionId=%d, ioStatus=0x%x", __FUNCTION__, deviceId, completionId, ioStatus);

	if (ioStatus != STATUS_SUCCESS)
	{
		/* Invoke the close file completion routine. */
		context->OnDriveDeleteFileComplete(context, irp->CallbackData, ioStatus);

		/* Destroy the IRP. */
		rdpdr_server_irp_free(irp);

		return;
	}

	Stream_Read_UINT32(s, fileId); /* FileId (4 bytes) */
	Stream_Read_UINT8(s, information); /* Information (1 byte) */

	/* Setup the IRP. */
	irp->CompletionId = context->priv->NextCompletionId++;
	irp->Callback = rdpdr_server_drive_delete_file_callback2;
	irp->DeviceId = deviceId;
	irp->FileId = fileId;

	rdpdr_server_enqueue_irp(context, irp);

	/* Send a request to close the file */
	rdpdr_server_send_device_close_request(context, deviceId, fileId, irp->CompletionId);
}

static BOOL rdpdr_server_drive_delete_file(RdpdrServerContext* context, void* callbackData, UINT32 deviceId, const char* path)
{
	RDPDR_IRP* irp;

	/* Create an IRP. */
	irp = rdpdr_server_irp_new();
	if (irp == NULL) return FALSE;

	irp->CompletionId = context->priv->NextCompletionId++;
	irp->Callback = rdpdr_server_drive_delete_file_callback1;
	irp->CallbackData = callbackData;
	irp->DeviceId = deviceId;
	strncpy(irp->PathName, path, sizeof(irp->PathName));

	rdpdr_server_convert_slashes(irp->PathName, sizeof(irp->PathName));

	printf("PathName=%s\n", irp->PathName); fflush(stdout);

	rdpdr_server_enqueue_irp(context, irp);

	/* Send a request to open the file. */
	rdpdr_server_send_device_create_request(
		context, deviceId, irp->CompletionId, irp->PathName,
		FILE_READ_DATA | SYNCHRONIZE,
		FILE_DELETE_ON_CLOSE | FILE_SYNCHRONOUS_IO_NONALERT,
		FILE_OPEN);

	return TRUE;
}

/*************************************************
 * Drive Rename File
 ************************************************/

static void rdpdr_server_drive_rename_file_callback3(RdpdrServerContext* context, wStream* s, RDPDR_IRP* irp, UINT32 deviceId, UINT32 completionId, UINT32 ioStatus)
{
	WLog_DBG(TAG, "%s: deviceId=%d, completionId=%d, ioStatus=0x%x", __FUNCTION__, deviceId, completionId, ioStatus);

	/* Destroy the IRP. */
	rdpdr_server_irp_free(irp);
}

static void rdpdr_server_drive_rename_file_callback2(RdpdrServerContext* context, wStream* s, RDPDR_IRP* irp, UINT32 deviceId, UINT32 completionId, UINT32 ioStatus)
{
	UINT32 length;

	WLog_DBG(TAG, "%s: deviceId=%d, completionId=%d, ioStatus=0x%x", __FUNCTION__, deviceId, completionId, ioStatus);

	Stream_Read_UINT32(s, length); /* Length (4 bytes) */
	Stream_Seek(s, 1); /* Padding (1 byte) */

	/* Invoke the rename file completion routine. */
	context->OnDriveRenameFileComplete(context, irp->CallbackData, ioStatus);

	/* Setup the IRP. */
	irp->CompletionId = context->priv->NextCompletionId++;
	irp->Callback = rdpdr_server_drive_rename_file_callback3;
	irp->DeviceId = deviceId;

	rdpdr_server_enqueue_irp(context, irp);

	/* Send a request to close the file */
	rdpdr_server_send_device_close_request(context, deviceId, irp->FileId, irp->CompletionId);
}

static void rdpdr_server_drive_rename_file_callback1(RdpdrServerContext* context, wStream* s, RDPDR_IRP* irp, UINT32 deviceId, UINT32 completionId, UINT32 ioStatus)
{
	UINT32 fileId;
	UINT8 information;

	WLog_DBG(TAG, "%s: deviceId=%d, completionId=%d, ioStatus=0x%x", __FUNCTION__, deviceId, completionId, ioStatus);

	if (ioStatus != STATUS_SUCCESS)
	{
		/* Invoke the rename file completion routine. */
		context->OnDriveRenameFileComplete(context, irp->CallbackData, ioStatus);

		/* Destroy the IRP. */
		rdpdr_server_irp_free(irp);

		return;
	}

	Stream_Read_UINT32(s, fileId); /* FileId (4 bytes) */
	Stream_Read_UINT8(s, information); /* Information (1 byte) */

	printf("%s: FileId=%u\n", __FUNCTION__, fileId); fflush(stdout);

	/* Setup the IRP. */
	irp->CompletionId = context->priv->NextCompletionId++;
	irp->Callback = rdpdr_server_drive_rename_file_callback2;
	irp->DeviceId = deviceId;
	irp->FileId = fileId;

	rdpdr_server_enqueue_irp(context, irp);

	/* Send a request to rename the file */
	rdpdr_server_send_device_file_rename_request(context, deviceId, fileId, irp->CompletionId, irp->ExtraBuffer);
}

static BOOL rdpdr_server_drive_rename_file(RdpdrServerContext* context, void* callbackData, UINT32 deviceId, const char* oldPath, const char* newPath)
{
	RDPDR_IRP* irp;

	/* Create an IRP. */
	irp = rdpdr_server_irp_new();
	if (irp == NULL) return FALSE;

	irp->CompletionId = context->priv->NextCompletionId++;
	irp->Callback = rdpdr_server_drive_rename_file_callback1;
	irp->CallbackData = callbackData;
	irp->DeviceId = deviceId;
	strncpy(irp->PathName, oldPath, sizeof(irp->PathName));
	strncpy(irp->ExtraBuffer, newPath, sizeof(irp->ExtraBuffer));

	rdpdr_server_convert_slashes(irp->PathName, sizeof(irp->PathName));
	rdpdr_server_convert_slashes(irp->ExtraBuffer, sizeof(irp->ExtraBuffer));

	printf("PathName=%s\n", irp->PathName); fflush(stdout);
	printf("ExtraBuffer=%s\n", irp->ExtraBuffer); fflush(stdout);

	rdpdr_server_enqueue_irp(context, irp);

	/* Send a request to open the file. */
	rdpdr_server_send_device_create_request(
		context, deviceId, irp->CompletionId, irp->PathName,
		FILE_READ_DATA | SYNCHRONIZE,
		FILE_SYNCHRONOUS_IO_NONALERT,
		FILE_OPEN);

	return TRUE;
}

RdpdrServerContext* rdpdr_server_context_new(HANDLE vcm)
{
	RdpdrServerContext* context;
	context = (RdpdrServerContext*) malloc(sizeof(RdpdrServerContext));

	if (context)
	{
		ZeroMemory(context, sizeof(RdpdrServerContext));

		context->vcm = vcm;

		context->Start = rdpdr_server_start;
		context->Stop = rdpdr_server_stop;

		context->DriveCreateDirectory = rdpdr_server_drive_create_directory;
		context->DriveDeleteDirectory = rdpdr_server_drive_delete_directory;
		context->DriveQueryDirectory = rdpdr_server_drive_query_directory;
		context->DriveOpenFile = rdpdr_server_drive_open_file;
		context->DriveReadFile = rdpdr_server_drive_read_file;
		context->DriveWriteFile = rdpdr_server_drive_write_file;
		context->DriveCloseFile = rdpdr_server_drive_close_file;
		context->DriveDeleteFile = rdpdr_server_drive_delete_file;
		context->DriveRenameFile = rdpdr_server_drive_rename_file;

		context->priv = (RdpdrServerPrivate*) malloc(sizeof(RdpdrServerPrivate));

		if (context->priv)
		{
			ZeroMemory(context->priv, sizeof(RdpdrServerPrivate));
			context->priv->VersionMajor = RDPDR_VERSION_MAJOR;
			context->priv->VersionMinor = RDPDR_VERSION_MINOR_RDP6X;
			context->priv->ClientId = g_ClientId++;
			context->priv->UserLoggedOnPdu = TRUE;

			context->priv->IrpList = ListDictionary_New(TRUE);
			context->priv->NextCompletionId = 1;
		}
	}

	return context;
}

void rdpdr_server_context_free(RdpdrServerContext* context)
{
	if (context)
	{
		if (context->priv)
		{
			ListDictionary_Free(context->priv->IrpList);

			free(context->priv);
		}

		free(context);
	}
}
