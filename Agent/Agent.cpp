///////////////////////////////////////////////////////////////////////////////
//
//    (C) Copyright 1995 - 2013 OSR Open Systems Resources, Inc.
//    All Rights Reserved
//
//    This software is supplied for instructional purposes only.
//
//    OSR Open Systems Resources, Inc. (OSR) expressly disclaims any warranty
//    for this software.  THIS SOFTWARE IS PROVIDED  "AS IS" WITHOUT WARRANTY
//    OF ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING, WITHOUT LIMITATION,
//    THE IMPLIED WARRANTIES OF MERCHANTABILITY OR FITNESS FOR A PARTICULAR
//    PURPOSE.  THE ENTIRE RISK ARISING FROM THE USE OF THIS SOFTWARE REMAINS
//    WITH YOU.  OSR's entire liability and your exclusive remedy shall not
//    exceed the price paid for this material.  In no event shall OSR or its
//    suppliers be liable for any damages whatsoever (including, without
//    limitation, damages for loss of business profit, business interruption,
//    loss of business information, or any other pecuniary loss) arising out
//    of the use or inability to use this software, even if OSR has been
//    advised of the possibility of such damages.  Because some states/
//    jurisdictions do not allow the exclusion or limitation of liability for
//    consequential or incidental damages, the above limitation may not apply
//    to you.
//
//    OSR Open Systems Resources, Inc.
//    105 Route 101A Suite 19
//    Amherst, NH 03031  (603) 595-6500 FAX: (603) 595-6503
//    email bugs to: bugs@osr.com
///////////////////////////////////////////////////////////////////////////////

///////////////////////////////////////////////////////////////////////////////
// (C) Copyright 2023 @LeHackermann
///////////////////////////////////////////////////////////////////////////////

#include "Agent.h"
#include<ntddser.h>

///////////////////////////////////////////////////////////////////////////////
//
//  DriverEntry
//
//    This routine is called by Windows when the driver is first loaded.  It
//    is the responsibility of this routine to create the WDFDRIVER
//
//  INPUTS:
//
//      DriverObject - Address of the DRIVER_OBJECT created by Windows for this
//                     driver.
//
//      RegistryPath - UNICODE_STRING which represents this driver's key in the
//                     Registry.  
//
//  OUTPUTS:
//
//      None.
//
//  RETURNS:
//
//      STATUS_SUCCESS, otherwise an error indicating why the driver could not
//                      load.
//
//  IRQL:
//
//      This routine is called at IRQL == PASSIVE_LEVEL.
//
//  NOTES:
//
//
///////////////////////////////////////////////////////////////////////////////

HANDLE   handle;
WDFIOTARGET ioTarget;
PFILE_OBJECT fileObject;
PDEVICE_OBJECT deviceObject;


extern "C" NTSTATUS
DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath) {
	WDF_DRIVER_CONFIG config;
	NTSTATUS status;

#if DBG
	DbgPrint("\nCaspian Agent Driver -- Compiled %s %s\n", __DATE__, __TIME__);
#endif

	//
	// Provide pointer to our EvtDeviceAdd event processing callback
	// function
	//
	WDF_DRIVER_CONFIG_INIT(&config, AgentEvtDeviceAdd);


	//
	// Create our WDFDriver instance
	//
	status = WdfDriverCreate(DriverObject,
		RegistryPath,
		WDF_NO_OBJECT_ATTRIBUTES,
		&config,
		WDF_NO_HANDLE);

	if (!NT_SUCCESS(status)) {
#if DBG
		DbgPrint("WdfDriverCreate failed 0x%0x\n", status);
#endif
	}

	return(status);
}

///////////////////////////////////////////////////////////////////////////////
//
//  AgentEvtDeviceAdd
//
//    This routine is called by the framework when a device of
//    the type we support is found in the system.
//
//  INPUTS:
//
//      DriverObject - Our WDFDRIVER object
//
//      DeviceInit   - The device initialization structure we'll
//                     be using to create our WDFDEVICE
//
//  OUTPUTS:
//
//      None.
//
//  RETURNS:
//
//      STATUS_SUCCESS, otherwise an error indicating why the driver could not
//                      load.
//
//  IRQL:
//
//      This routine is called at IRQL == PASSIVE_LEVEL.
//
//  NOTES:
//
//
///////////////////////////////////////////////////////////////////////////////
NTSTATUS
AgentEvtDeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT DeviceInit) {
	NTSTATUS status;
	WDF_OBJECT_ATTRIBUTES objAttributes;
	WDFDEVICE device;
	WDF_IO_QUEUE_CONFIG queueConfig;
	PAGENT_DEVICE_CONTEXT devContext;

	//
	// Our user-accessible device name
	//
	DECLARE_CONST_UNICODE_STRING(userDeviceName, L"\\Global??\\Caspian");

	UNREFERENCED_PARAMETER(Driver);

	//
	// Prepare for WDFDEVICE creation
	//
	// Initialize standard WDF Object Attributes structure
	//
	WDF_OBJECT_ATTRIBUTES_INIT(&objAttributes);

	//
	// Specify our device context
	//
	WDF_OBJECT_ATTRIBUTES_SET_CONTEXT_TYPE(&objAttributes,
		AGENT_DEVICE_CONTEXT);

	//
	// Let's create our device object
	//
	status = WdfDeviceCreate(&DeviceInit,
		&objAttributes,
		&device);

	if (!NT_SUCCESS(status)) {
#if DBG
		DbgPrint("WdfDeviceCreate failed 0x%0x\n", status);
#endif
		return status;
	}

	//
	// Now that our device has been created, get our per-device-instance
	// storage area
	// 
	devContext = AgentGetContextFromDevice(device);


	//
	// Create a symbolic link for our device so that user-mode can open
	// the device by name.
	//
	status = WdfDeviceCreateSymbolicLink(device, &userDeviceName);

	if (!NT_SUCCESS(status)) {
#if DBG
		DbgPrint("WdfDeviceCreateSymbolicLink failed 0x%0x\n", status);
#endif
		return(status);
	}

	//
	// Configure our queue of incoming requests
	//
	// We only use our default Queue to receive requests from the Framework,
	// and we set it for parallel processing.  This means that the driver can
	// have multiple requests outstanding from this queue simultaneously.
	//
	WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig,
		WdfIoQueueDispatchParallel);

	//
	// Declare our I/O Event Processing callbacks
	//
	// This driver only handles IOCTLs.
	//
	// WDF will automagically handle Create and Close requests for us and will
	// will complete any OTHER request types with STATUS_INVALID_DEVICE_REQUEST.    
	//
	queueConfig.EvtIoDeviceControl = AgentEvtIoDeviceControl;

	//
	// Because this is a queue for a software-only device, indicate
	// that the queue doesn't need to be power managed
	//
	queueConfig.PowerManaged = WdfFalse;

	status = WdfIoQueueCreate(device,
		&queueConfig,
		WDF_NO_OBJECT_ATTRIBUTES,
		WDF_NO_HANDLE);

	if (!NT_SUCCESS(status)) {
#if DBG
		DbgPrint("WdfIoQueueCreate for default queue failed 0x%0x\n", status);
#endif
		return(status);
	}

	//
	// And we're going to use a Queue with manual dispatching to hold the
	// Requests that we'll use for notification purposes
	// 
	WDF_IO_QUEUE_CONFIG_INIT(&queueConfig,
		WdfIoQueueDispatchManual);

	queueConfig.PowerManaged = WdfFalse;

	status = WdfIoQueueCreate(device,
		&queueConfig,
		WDF_NO_OBJECT_ATTRIBUTES,
		&devContext->NotificationQueue);

	if (!NT_SUCCESS(status)) {
		#if DBG
		DbgPrint("WdfIoQueueCreate for manual queue failed 0x%0x\n", status);
		#endif
		return(status);
	}

	WDF_OBJECT_ATTRIBUTES attribs;

	//
	// Initializing WDF_OBJECT attributes to be used in the IO Target
	//
	WDF_OBJECT_ATTRIBUTES_INIT(&attribs);
	attribs.ParentObject = device;
	
	//
	// Create the IO Target linked to the driver's device
	//
	status = WdfIoTargetCreate(device, &attribs, &ioTarget);

	if (NT_SUCCESS(status)) {
		UNICODE_STRING portName;
		//
		// Serial Port to be read from 
		//
		WCHAR portNameBuffer[] = L"\\device\\Serial0";
		RtlInitUnicodeString(&portName, portNameBuffer);
		IO_STATUS_BLOCK ioStatusBlock = { 0 };
		WDF_IO_TARGET_OPEN_PARAMS params;

		WDF_IO_TARGET_OPEN_PARAMS_INIT_OPEN_BY_NAME(
			&params,
			&portName,
			MAXIMUM_ALLOWED);  // this should be pared down to specific rights
		status = WdfIoTargetOpen(ioTarget, &params);



		if (!NT_SUCCESS(status)) {
			#if DBG
			DbgPrint("%s: Failed WdfIoTargetOpen 0x%x\n", __FUNCTION__, status);
			#endif
		}
		else {
			WDFREQUEST request;
			status = WdfRequestCreate(WDF_NO_OBJECT_ATTRIBUTES, ioTarget, &request);
			if (NT_SUCCESS(status)) {
				//
				// Setting the baud rate
				//
				SERIAL_BAUD_RATE baudRate = { 9600 };
				WDF_MEMORY_DESCRIPTOR memDesc;
				WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&memDesc, &baudRate, sizeof(baudRate));
				WDF_REQUEST_SEND_OPTIONS sendOptions;
				WDF_REQUEST_SEND_OPTIONS_INIT(&sendOptions, 0);
				ULONG_PTR bytesReturned = 0;
				status = WdfIoTargetSendIoctlSynchronously(ioTarget, request, IOCTL_SERIAL_SET_BAUD_RATE, &memDesc, nullptr, &sendOptions, &bytesReturned);
				if (!NT_SUCCESS(status)) {
					#if DBG
					DbgPrint("%s: serial set baud rate failed 0x%x\n",__FUNCTION__, status);
					#endif
				}
				else {
					UNICODE_STRING     uniName;
					OBJECT_ATTRIBUTES  objAttr;
					IO_STATUS_BLOCK    ioStatusBlock;

					//
					// File to drop the malware at.
					//

					RtlInitUnicodeString(&uniName, L"\\DosDevices\\C:\\malware.exe");
					InitializeObjectAttributes(&objAttr, &uniName,
						OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
						NULL, NULL);
					status = ZwCreateFile(&handle,
						GENERIC_WRITE,
						&objAttr,
						&ioStatusBlock,
						NULL,
						FILE_ATTRIBUTE_NORMAL,
						0,
						FILE_OVERWRITE_IF,
						FILE_SYNCHRONOUS_IO_NONALERT,
						NULL, 0);
				}
			}
		}
	}



	return(status);
}

///////////////////////////////////////////////////////////////////////////////
//
//  DropExecutable
//
//    This routine is called when the driver received a sample and is ready to
//    drop it. Using one of the hanging IOCTLs, it initiates the the file creation
//    procedure by sending the sample as a response to the user-mode component. 
//
//  INPUTS:
//      DevContext      A pointer to our device context
//
//  OUTPUTS:
//
//      None.
//
//  RETURNS:
//
//      None.
//
//  IRQL:
//
//      This routine is called at IRQL <= DISPATCH_LEVEL
//
//  NOTES:
//
//
///////////////////////////////////////////////////////////////////////////////
VOID
DropExecutable(PAGENT_DEVICE_CONTEXT DevContext) {
	NTSTATUS status;
	ULONG_PTR info;
	WDFREQUEST notifyRequest;
	PULONG  bufferPointer;
	//LONG valueToReturn;

	status = WdfIoQueueRetrieveNextRequest(DevContext->NotificationQueue,
		&notifyRequest);

	//
	// Be sure we got a Request
	// 
	if (!NT_SUCCESS(status)) {

		//
		// Nope!  We were NOT able to successfully remove a Request from the
		// notification queue.  Well, perhaps there aren't any right now.
		// Whatever... not much we can do in this example about this.
		// 
		#if DBG
		DbgPrint("DropExecutable: Failed to retrieve request. Status = 0x%0x\n",
			status);
		#endif
		return;
	}

	//
	// We've successfully removed a Request from the queue of pending 
	// notification IOCTLs.
	// 

	status = WdfRequestRetrieveOutputBuffer(notifyRequest,
		sizeof(LONG),
		(PVOID*)&bufferPointer,
		nullptr);
	//
	// Valid OutBuffer?
	// 
	if (!NT_SUCCESS(status)) {

		//
		// The OutBuffer associated with the pending notification Request that
		// we just dequeued is somehow not valid. This doesn't really seem
		// possible, but... you know... they return you a status, you have to
		// check it and handle it.
		// 
#if DBG
		DbgPrint("DropExecutable: WdfRequestRetrieveOutputBuffer failed.  Status = 0x%0x\n",
			status);
#endif

		//
		// Complete the IOCTL_READY_TO_RECEIVE with success, but
		// indicate that we're not returning any additional information.
		// 
		status = STATUS_SUCCESS;
		info = 0;

	}
	else {

		//
		// We successfully retrieved a Request from the notification Queue
		// AND we retrieved an output buffer into which to return some
		// additional information.
		// 

		//
		// We first read the size of the sample written by the host machine to 
		// efficiently allocate a buffer for it.
		//
		PULONG size = reinterpret_cast<PULONG>(ExAllocatePool2(POOL_FLAG_NON_PAGED, 8, 'zssC'));

		WDFREQUEST sRequest;
		status = WdfRequestCreate(WDF_NO_OBJECT_ATTRIBUTES, ioTarget, &sRequest);

		if (NT_SUCCESS(status)) {
			WDF_MEMORY_DESCRIPTOR sizeMemoryDesc;
			WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&sizeMemoryDesc, size, 8);
			WDF_MEMORY_DESCRIPTOR binMemoryDesc;


			WDF_REQUEST_SEND_OPTIONS sendOptions;
			WDF_REQUEST_SEND_OPTIONS_INIT(&sendOptions, 0);

			ULONG_PTR bytesRead = 0;

			status = WdfIoTargetSendReadSynchronously(ioTarget, sRequest, &sizeMemoryDesc, 0, &sendOptions, &bytesRead);
			//
			// We allocate a buffer for the sample according to the size 
			// we received
			//
			char* Buffer = reinterpret_cast<char*>(ExAllocatePoolWithTag(
				NonPagedPoolNx,
				*size,
				'lmsC'));

			WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&binMemoryDesc, Buffer, *size);
			WDF_REQUEST_REUSE_PARAMS params;
			WDF_REQUEST_REUSE_PARAMS_INIT(&params, WDF_REQUEST_REUSE_NO_FLAGS, status);
			WdfRequestReuse(sRequest, &params);
			status = WdfIoTargetSendReadSynchronously(ioTarget, sRequest, &binMemoryDesc, 0, &sendOptions, &bytesRead);
			IO_STATUS_BLOCK ioStatusBlock;

			//
			// Upon receiving the sample, we write it out to the created file.
			//
			status = ZwWriteFile(handle, NULL, NULL, NULL, &ioStatusBlock,
				Buffer, *size, NULL, NULL);
			ZwClose(handle);
			WdfObjectDelete(sRequest);
		}
		//
		// Complete the IOCTL_READY_TO_RECEIVE with success, indicating
		// we're returning a binary file in the user's OutBuffer
		// 
		status = STATUS_SUCCESS;
		info = *size;
	}

	//
	// And now... NOTIFY the user about the event. We do this just
	// by completing the dequeued Request.
	// 
	WdfRequestCompleteWithInformation(notifyRequest, status, info);
}

///////////////////////////////////////////////////////////////////////////////
//
//  AgentEvtIoDeviceControl
//
//    This routine is called by the framework when there is a
//    device control request for us to process
//
//  INPUTS:
//
//      Queue    - Our default queue
//
//      Request  - A device control request
//
//      OutputBufferLength - The length of the output buffer
//
//      InputBufferLength  - The length of the input buffer
//
//      IoControlCode      - The operation being performed
//
//  OUTPUTS:
//
//      None.
//
//  RETURNS:
//
//      None.
//
//  IRQL:
//
//      This routine is called at IRQL <= DISPATCH_LEVEL
//
//  NOTES:
//
//
///////////////////////////////////////////////////////////////////////////////
VOID
AgentEvtIoDeviceControl(WDFQUEUE Queue,
	WDFREQUEST Request,
	size_t OutputBufferLength,
	size_t InputBufferLength,
	ULONG IoControlCode) {
	PAGENT_DEVICE_CONTEXT devContext;
	NTSTATUS status;
	ULONG_PTR info;

	UNREFERENCED_PARAMETER(OutputBufferLength);
	UNREFERENCED_PARAMETER(InputBufferLength);

	devContext = AgentGetContextFromDevice(WdfIoQueueGetDevice(Queue));

	//
	// Set the default completion status and information field
	// 
	status = STATUS_INVALID_PARAMETER;
	info = 0;

#if DBG
	DbgPrint("AgentEvtIoDeviceControl\n");
#endif

	switch (IoControlCode) {

		//
		// This IOCTL are sent by the user application, and will be completed
		// by the driver when an event occurs.
		// 
	case IOCTL_READY_TO_RECEIVE: {

		status = WdfRequestForwardToIoQueue(Request,
			devContext->NotificationQueue);

		//
		// If we can't forward the Request to our holding queue,
		// we have to complete it.  We'll use whatever status we get
		// back from WdfRequestForwardToIoQueue.
		// 
		if (!NT_SUCCESS(status)) {
			break;
		}

		//
		// *** RETURN HERE WITH REQUEST PENDING ***
		//     We do not break, we do not fall through.
		//
		return;
	}

	//
	// This IOCTL is sent by the application to inform the driver to drop the sample 
	// as it's ready to execute it. 
	// 
	case IOCTL_LAUNCH_EXECUTABLE: {

		DropExecutable(devContext);

		//
		// Regardless of the success of the notification operation, we
		// complete the event simulation IOCTL with success
		// 
		status = STATUS_SUCCESS;

		break;
	}

	default: {
#if DBG
		DbgPrint("AgentEvtIoDeviceControl: Invalid IOCTL received\n");
#endif
		break;
	}

	}

	//
	// Complete the received Request
	// 
	WdfRequestCompleteWithInformation(Request,
		status,
		info);
}
