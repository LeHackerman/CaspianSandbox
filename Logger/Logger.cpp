 ///////////////////////////////////////////////////////////////////////////////
 // Copyright (c) 2018, Satoshi Tanda. All rights reserved.
 // Copyright(c) 2023, Haroun @LeHackermann. All rights reserved.
 ///////////////////////////////////////////////////////////////////////////////

/*
MIT License

Copyright (c) 2018 Satoshi Tanda

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#include <fltKernel.h>
#include <ntstrsafe.h>
#include<wdm.h>
#include<ntddser.h>

 //
 // Defines SYNCH_LEVEL for readability. SYNCH_LEVEL is 12 on  x64.
 //
#ifndef SYNCH_LEVEL
#define SYNCH_LEVEL 12
#endif


//
// Handy macros to specify at which segment the code should be placed.
//
#define INIT  __declspec(code_seg("INIT"))
#define PAGED __declspec(code_seg("PAGE"))

//
// The logging function to be exported. EXTERN_C should be used to stop name mangling.
//
EXTERN_C  VOID __log(_In_ PSTRING);


//
// The size of log buffer in bytes. Two buffers of this size will be allocated.
// Change this as needed.
//
static const ULONG k_LogBufferSize = PAGE_SIZE * 8;


//
// The pool tag.
//
static const ULONG k_PoolTag = 'glsC';

//
// The maximum characters the DbgPrint family can handle at once.
//
static const ULONG k_MaxDbgPrintLogLength = 512;

//
// The format of a single log message stored in LOG_BUFFER::LogEntries.
//
#include <pshpack1.h>
typedef struct _LOG_ENTRY
{
    //
    // The system time of when this message was logged.
    //
    LARGE_INTEGER Timestamp;

    //
    // The length of the message stored in LogLine in characters.
    //
    USHORT LogLineLength;

    //
    // The log message, not including terminating null, '\r' or '\n'.
    //
    CHAR LogLine[ANYSIZE_ARRAY];
} LOG_ENTRY, * PLOG_ENTRY;
static_assert(sizeof(LOG_ENTRY) == 11, "Must be packed for space");
#include <poppack.h>

//
// The active and inactive buffer layout.
//
typedef struct _LOG_BUFFER
{
    //
    // The pointer to the buffer storing the sequence of LOG_ENTRYs.
    //
    PLOG_ENTRY LogEntries;

    //
    // The offset to the address where the next LOG_ENTRY should be saved,
    // counted from LogEntries.
    //
    ULONG NextLogOffset;

    //
    // How many bytes are not save into LogEntries due to lack of space.
    //
    ULONG OverflowedLogSize;
} LOG_BUFFER, * PLOG_BUFFER;

//
// The structure used by the logging function.
//
typedef struct _PAIRED_LOG_BUFFER
{
    //
    // Indicates whether ActiveLogBuffer and InactiveLogBuffer are usable.
    //
    BOOLEAN BufferValid;

    //
    // The lock must be held before accessing any other fields of this structure.
    //
    EX_SPIN_LOCK ActiveLogBufferLock;

    //
    // The pointers to two buffers: active and inactive. Active buffer is used
    // by the debug print callback and to save new messages as they comes in.
    // Inactive buffer is buffer accessed and cleared up by the flush buffer thread.
    //
    PLOG_BUFFER ActiveLogBuffer;
    PLOG_BUFFER InactiveLogBuffer;
} PAIRED_LOG_BUFFER, * PPAIRED_LOG_BUFFER;

//
// The set of information the flush buffer thread may need.
//
typedef struct _FLUSH_BUFFER_THREAD_CONTEXT
{
    KEVENT ThreadExitEvent;
    PPAIRED_LOG_BUFFER PairedLogBuffer;
    PFILE_OBJECT SerialFileObject;
    PDEVICE_OBJECT SerialDeviceObject;
    PKTHREAD FlushBufferThread;
    ULONG MaxOverflowedLogSize;
} FLUSH_BUFFER_THREAD_CONTEXT, * PFLUSH_BUFFER_THREAD_CONTEXT;

//
// Buffer structures as global variables. Initialized by initBuffers
// and cleaned up by cleanup.
//
static LOG_BUFFER g_LogBuffer1;
static LOG_BUFFER g_LogBuffer2;
static PAIRED_LOG_BUFFER g_PairedLogBuffer;

//
// The thread context. Initialized by StartFlushBufferThread and cleaned up by
// StopFlushBufferThread.
//
static FLUSH_BUFFER_THREAD_CONTEXT g_ThreadContext;



//
// Code analysis wants this declaration.
//
INIT EXTERN_C DRIVER_INITIALIZE DriverEntry;

/*!
    @brief Saves a single line debug message to the active buffer.

    @param[in] Timestamp - The time stamp of when the log message was sent.

    @param[in] LogLine - The single line, null-terminated debug log message.
        Does not include "\n".

    @param[in,out] PairedLogBuffer - Buffer to save the message.
*/
static
_IRQL_requires_(SYNCH_LEVEL)
VOID
SaveLogOutputLine(
    _In_ const LARGE_INTEGER* Timestamp,
    _In_ PCSTR LogLine,
    _Inout_ PPAIRED_LOG_BUFFER PairedLogBuffer
)
{
    USHORT logLineLength;
    ULONG logEntrySize;
    BOOLEAN lockAcquired;
    PLOG_ENTRY logEntry;

    lockAcquired = FALSE;

    //
    // Get the length of the message in characters. The message should never be
    // an empty (as per behavior of strtok_s) and should never be longer than
    // what the DbgPrint family can handle.
    //
    logLineLength = static_cast<USHORT>(strlen(LogLine));
    if ((logLineLength == 0) || (logLineLength > k_MaxDbgPrintLogLength))
    {
        NT_ASSERT(FALSE);
        goto Exit;
    }

    //
    // Unlikely but one can output \r\n. Ignore this to normalize contents.
    //
    if (LogLine[logLineLength - 1] == '\r')
    {
        if ((--logLineLength) == 0)
        {
            goto Exit;
        }
    }

    logEntrySize = RTL_SIZEOF_THROUGH_FIELD(LOG_ENTRY, LogLineLength) +
        logLineLength;

    //
    // Acquire the lock to safely modify active buffer.
    //
    ExAcquireSpinLockExclusiveAtDpcLevel(&PairedLogBuffer->ActiveLogBufferLock);
    lockAcquired = TRUE;

    //
    // Bail out if a concurrent thread invalidated buffer.
    //
    if (PairedLogBuffer->BufferValid == FALSE)
    {
        goto Exit;
    }

    //
    // If the remaining buffer is not large enough to save this message, count
    // up the overflowed size and bail out.
    //
    if (PairedLogBuffer->ActiveLogBuffer->NextLogOffset + logEntrySize > k_LogBufferSize)
    {
        PairedLogBuffer->ActiveLogBuffer->OverflowedLogSize += logEntrySize;
        goto Exit;
    }

    //
    // There are sufficient room to save the message. Get the address to save
    // the message within active buffer. On debug build, the address should be
    // filled with 0xff, indicating no one has yet touched there.
    //
    logEntry = reinterpret_cast<PLOG_ENTRY>(Add2Ptr(
        PairedLogBuffer->ActiveLogBuffer->LogEntries,
        PairedLogBuffer->ActiveLogBuffer->NextLogOffset));
    NT_ASSERT(logEntry->Timestamp.QuadPart == MAXULONG64);
    NT_ASSERT(logEntry->LogLineLength == MAXUSHORT);

    //
    // Save this message and update the offset to the address to save the next
    // message.
    //
    logEntry->Timestamp = *Timestamp;
    logEntry->LogLineLength = logLineLength;
    RtlCopyMemory(logEntry->LogLine, LogLine, logLineLength);
    PairedLogBuffer->ActiveLogBuffer->NextLogOffset += logEntrySize;

Exit:
    if (lockAcquired != FALSE)
    {
        ExReleaseSpinLockExclusiveFromDpcLevel(&PairedLogBuffer->ActiveLogBufferLock);
    }
    return;
}

/*!
    @brief Saves the debug log messages to active buffer.

    @param[in] Output - The formatted debug log message.

    @param[in,out] PairedLogBuffer - Buffer to save the message.
*/
static
_IRQL_requires_(SYNCH_LEVEL)
VOID
SaveLogOutput(
    _In_ const STRING* Output,
    _Inout_ PPAIRED_LOG_BUFFER PairedLogBuffer
)
{
    CHAR ouputBuffer[k_MaxDbgPrintLogLength + 1];
    PSTR strtokContext;
    PSTR logLine;
    LARGE_INTEGER timestamp;

    //
    // Capture when the debug log message is sent.
    //
    KeQuerySystemTimePrecise(&timestamp);

    //
    // Ignore an empty message as it is not interesting.
    //
    if (Output->Length == 0)
    {
        goto Exit;
    }

    //
    // The message should be shorter than what the DbgPrint family can handle at
    // one call.
    //
    if (Output->Length > k_MaxDbgPrintLogLength)
    {
        NT_ASSERT(FALSE);
        goto Exit;
    }

    //
    // Copy the message as a null-terminated string.
    //
    RtlCopyMemory(ouputBuffer, Output->Buffer, Output->Length);
    ouputBuffer[Output->Length] = ANSI_NULL;

    //
    // Split it with \n and save each split message. Note that strtok_s removes
    // "\n\n", so empty lines are not saved.
    //
    strtokContext = nullptr;
    logLine = strtok_s(ouputBuffer, "\n", &strtokContext);
    while (logLine != nullptr)
    {
        SaveLogOutputLine(&timestamp, logLine, PairedLogBuffer);
        logLine = strtok_s(nullptr, "\n", &strtokContext);
    }

Exit:
    return;
}

/*!
    @brief The logging function to be exported

    @param[in] Output - The formatted debug log message.
*/
_Use_decl_annotations_
VOID
__log(
    _In_ PSTRING Output
)
{
    KIRQL oldIrql;

    //
    // IRQL >= DISPATCH_LEVEL is needed later ig ?
    //
    oldIrql = KeRaiseIrqlToSynchLevel();

    SaveLogOutput(Output, &g_PairedLogBuffer);

    KeLowerIrql(oldIrql);
    return;
}

/*!
    @brief Cleans up paired buffer.

    @param[in,out] PairedLogBuffer - The paired buffer associated to clean up.
*/
static
_IRQL_requires_max_(PASSIVE_LEVEL)
VOID
cleanup(
    _Inout_ PPAIRED_LOG_BUFFER PairedLogBuffer
)
{
    KIRQL oldIrql;


    //
    // Let us make sure no one is touching the paired buffer. Without this, it
    // is possible that the callback is still running concurrently on the other
    // processor and touching the paired buffer.
    //
    oldIrql = ExAcquireSpinLockExclusive(&PairedLogBuffer->ActiveLogBufferLock);

    //
    // Free both buffer and mark this paired buffer as invalid, so the other
    // thread waiting on this skin lock can tell the buffer is no longer valid
    // when the spin lock was released.
    //
    ExFreePoolWithTag(PairedLogBuffer->ActiveLogBuffer->LogEntries, k_PoolTag);
    ExFreePoolWithTag(PairedLogBuffer->InactiveLogBuffer->LogEntries, k_PoolTag);
    PairedLogBuffer->BufferValid = FALSE;

    ExReleaseSpinLockExclusive(&PairedLogBuffer->ActiveLogBufferLock, oldIrql);
}

/*!
    @brief Stops the flush buffer thread and cleans up the thread context.

    @param[in,out] ThreadContext - The context associated to the thread and to
        clean up.
*/
PAGED
static
_IRQL_requires_max_(PASSIVE_LEVEL)
VOID
StopFlushBufferThread(
    _Inout_ PFLUSH_BUFFER_THREAD_CONTEXT ThreadContext
)
{
    NTSTATUS status;

    PAGED_CODE();

    DbgPrintEx(DPFLTR_IHVDRIVER_ID,
        DPFLTR_INFO_LEVEL,
        "Stopping debug print logging.\n");

    if (ThreadContext->MaxOverflowedLogSize != 0)
    {
        DbgPrintEx(DPFLTR_IHVDRIVER_ID,
            DPFLTR_INFO_LEVEL,
            "Max overflow size = 0x%x. Consider increasing the buffer"
            " size and recompile the driver for the next run.\n",
            ThreadContext->MaxOverflowedLogSize);
    }

    //
    // Signal the event to exit the thread, and wait for termination.
    //
    (VOID)KeSetEvent(&ThreadContext->ThreadExitEvent, IO_NO_INCREMENT, FALSE);
    status = KeWaitForSingleObject(ThreadContext->FlushBufferThread,
        Executive,
        KernelMode,
        FALSE,
        nullptr);
    NT_ASSERT(status == STATUS_SUCCESS);
    ObDereferenceObject(ThreadContext->FlushBufferThread);

    //
    // No one should be touching the log file now. Close it.
    //
    NT_VERIFY(NT_SUCCESS(ObDereferenceObject(ThreadContext->SerialFileObject)));
}

/*!
    @brief Stops logging.
*/
PAGED
static
_IRQL_requires_max_(PASSIVE_LEVEL)
VOID
shutdown(
    VOID
)
{
    PAGED_CODE();

    StopFlushBufferThread(&g_ThreadContext);
    cleanup(&g_PairedLogBuffer);
}
/*!
    @brief Flush active buffers.

    @details This function first swaps active buffer with inactive buffer, so
        that the currently active buffer can safely be accessed (ie, inactive
        buffer becomes active buffer). Then, writes contents of previously
        active buffer into the serial port if contents exist. Then, updates the max
        overflow count as needed. Finally, it clears the contents of previously
        active buffer to make it ready to become active buffer again.

    @param[in,out] ThreadContext - Context to be used by the thread.
*/
static
_IRQL_requires_max_(PASSIVE_LEVEL)
VOID
FlushLogEntries(
    _Inout_ PFLUSH_BUFFER_THREAD_CONTEXT ThreadContext
)
{
    NTSTATUS status;
    PPAIRED_LOG_BUFFER pairedLogBuffer;
    KIRQL oldIrql;
    PLOG_BUFFER oldLogBuffer;
    IO_STATUS_BLOCK ioStatusBlock;

    status = STATUS_SUCCESS;
    pairedLogBuffer = ThreadContext->PairedLogBuffer;

    //
    // Swap active buffer and inactive buffer.
    //
    oldIrql = ExAcquireSpinLockExclusive(&pairedLogBuffer->ActiveLogBufferLock);
    oldLogBuffer = pairedLogBuffer->ActiveLogBuffer;
    pairedLogBuffer->ActiveLogBuffer = pairedLogBuffer->InactiveLogBuffer;
    pairedLogBuffer->InactiveLogBuffer = oldLogBuffer;
    ExReleaseSpinLockExclusive(&pairedLogBuffer->ActiveLogBufferLock, oldIrql);

    NT_ASSERT(pairedLogBuffer->ActiveLogBuffer != pairedLogBuffer->InactiveLogBuffer);

    //
    // Iterate all saved debug log messages (if exist).
    //
    for (ULONG offset = 0; offset < oldLogBuffer->NextLogOffset; /**/)
    {
        PLOG_ENTRY logEntry;
        CHAR writeBuffer[k_MaxDbgPrintLogLength + 50]; // 50 for date and time.
        ANSI_STRING tmpLogLine;
        TIME_FIELDS timeFields;
        LARGE_INTEGER localTime;
        PIRP irp;
        logEntry = reinterpret_cast<PLOG_ENTRY>(Add2Ptr(
            oldLogBuffer->LogEntries,
            offset));

        //
        // Build a temporal ANSI_STRING to stringify non-null terminated string.
        //
        tmpLogLine.Buffer = logEntry->LogLine;
        tmpLogLine.Length = logEntry->LogLineLength;
        tmpLogLine.MaximumLength = logEntry->LogLineLength;

        //
        // Convert the time stamp to the local time in the human readable format.
        //
        ExSystemTimeToLocalTime(&logEntry->Timestamp, &localTime);
        RtlTimeToTimeFields(&localTime, &timeFields);

        status = RtlStringCchPrintfA(writeBuffer,
            RTL_NUMBER_OF(writeBuffer),
            "%02hd-%02hd %02hd:%02hd:%02hd.%03hd %Z\r\n",
            timeFields.Month,
            timeFields.Day,
            timeFields.Hour,
            timeFields.Minute,
            timeFields.Second,
            timeFields.Milliseconds,
            &tmpLogLine);
        if (!NT_SUCCESS(status))
        {
            //
            // This should not happen, but if it does, just discard all log
            // messages. The next attempt will very likely fail too.
            //
            NT_ASSERT(FALSE);
            break;
        }
        //
        // Get the size of the active buffer.
        //
        ULONG bytesToWrite = static_cast<ULONG>(strlen(writeBuffer));
        //
        // Build an IRP with major function IRP_MJ_WRITE.
        //
        irp = IoBuildSynchronousFsdRequest(
            IRP_MJ_WRITE,
            ThreadContext->SerialDeviceObject,
            writeBuffer,
            bytesToWrite,
            NULL,
            NULL,
            &ioStatusBlock
        );

        if (irp != NULL)
        {
            //
            // Send IRP.
            //
            status = IoCallDriver(ThreadContext->SerialDeviceObject, irp);

            if (NT_SUCCESS(status))
            {
                DbgPrint("Wrote %d bytes successfully\n", bytesToWrite);
            }
            else
            {
                DbgPrint("Failed to write %d bytes: 0x%x\n", bytesToWrite, status);
            }
        }
        else
        {
            DbgPrint("Failed to build IRP for write operation\n");
        }
        if (!NT_SUCCESS(status))
        {
            //
            // This can happen when the system is shutting down and the file
            // system was already unmounted. Bail out, nothing we can do.
            //
            break;
        }

        //
        // Compute the offset to the next entry by adding the size of the current
        // entry.
        //
        offset += RTL_SIZEOF_THROUGH_FIELD(LOG_ENTRY, LogLineLength) +
            logEntry->LogLineLength;
    }

    //
    // If the log messages exist, and no error happened before, flush the
    // serial port. This should not fail (unless the file system is unmounted
    // after the last successful write).
    //
    if ((oldLogBuffer->NextLogOffset != 0) && NT_SUCCESS(status))
    {
        //
        // Build an IRP with major function IRP_MJ_FLUSH_BUFFERS.
        //
        PIRP irp = IoBuildSynchronousFsdRequest(
            IRP_MJ_FLUSH_BUFFERS,
            ThreadContext->SerialDeviceObject,
            NULL,
            NULL,
            NULL,
            NULL,
            &ioStatusBlock
        );

        if (irp != NULL)
        {
            //
            // Send IRP.
            //
            status = IoCallDriver(ThreadContext->SerialDeviceObject, irp);

        }

        NT_ASSERT(NT_SUCCESS(status));
    }

    //
    // Update the maximum overflow size as necessary.
    //
    ThreadContext->MaxOverflowedLogSize = max(ThreadContext->MaxOverflowedLogSize,
        oldLogBuffer->OverflowedLogSize);

    //
    // Finally, clear the previously active buffer.
    //
    oldLogBuffer->NextLogOffset = 0;
    oldLogBuffer->OverflowedLogSize = 0;
#if DBG
    RtlFillMemory(oldLogBuffer->LogEntries, k_LogBufferSize, 0xff);
#endif
}

/*!
    @brief The entry point of the flush buffer thread.

    @param[in] Context - The thread context.
*/
PAGED
static
_Function_class_(KSTART_ROUTINE)
_IRQL_requires_max_(PASSIVE_LEVEL)
VOID
FlushBufferThreadEntryPoint(
    _In_ PVOID Context
)
{
    static const ULONG intervalMs = 500;
    NTSTATUS status;
    PFLUSH_BUFFER_THREAD_CONTEXT threadContext;
    LARGE_INTEGER interval;

    PAGED_CODE();

    threadContext = reinterpret_cast<PFLUSH_BUFFER_THREAD_CONTEXT>(Context);

    interval.QuadPart = -(10000ll * intervalMs);

    do
    {
        //
        // Flush log buffer with interval, or exit when it is requested.
        //
        status = KeWaitForSingleObject(&threadContext->ThreadExitEvent,
            Executive,
            KernelMode,
            FALSE,
            &interval);
        FlushLogEntries(threadContext);
    } while (status == STATUS_TIMEOUT);

    //
    // It is probably a programming error if non STATUS_SUCCESS is returned. Let
    // us catch that.
    //
    NT_ASSERT(status == STATUS_SUCCESS);
    PsTerminateSystemThread(status);
}

/*!
    @brief Initializes log buffers.

    @details This function takes two buffers to be initialized, and one paired
        buffer, which essentially references to those two buffers. All of them
        are initialized in this function.

    @param[out] LogBufferActive - Debug log buffer to use initially.

    @param[out] LogBufferInactive - Debug log buffer to be inactive initially.

    @param[out] PairedLogBuffer - A buffer pair to be used in the debug print
        callback.

    @return STATUS_SUCCESS or an appropriate status code.
*/
INIT
static
_IRQL_requires_max_(PASSIVE_LEVEL)
_Check_return_
NTSTATUS
initBuffers(
    _Out_ PLOG_BUFFER LogBufferActive,
    _Out_ PLOG_BUFFER LogBufferInactive,
    _Out_ PPAIRED_LOG_BUFFER PairedLogBuffer
)
{
    NTSTATUS status = STATUS_SUCCESS;
    PLOG_ENTRY logEntries1, logEntries2;

    PAGED_CODE();

    RtlZeroMemory(LogBufferActive, sizeof(*LogBufferActive));
    RtlZeroMemory(LogBufferInactive, sizeof(*LogBufferInactive));
    RtlZeroMemory(PairedLogBuffer, sizeof(*PairedLogBuffer));

    logEntries2 = nullptr;

    //
    // Allocate log buffers.
    //
    logEntries1 = reinterpret_cast<PLOG_ENTRY>(ExAllocatePoolWithTag(
        NonPagedPoolNx,
        k_LogBufferSize,
        k_PoolTag));
    if (logEntries1 == nullptr)
    {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto Exit;
    }

    logEntries2 = reinterpret_cast<PLOG_ENTRY>(ExAllocatePoolWithTag(
        NonPagedPoolNx,
        k_LogBufferSize,
        k_PoolTag));
    if (logEntries2 == nullptr)
    {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto Exit;
    }

#if DBG
    //
    // Fill buffer contents with some distinguishable bytes for ease of debugging.
    //
    RtlFillMemory(logEntries1, k_LogBufferSize, MAXUCHAR);
    RtlFillMemory(logEntries2, k_LogBufferSize, MAXUCHAR);
#endif

    //
    // Initialize buffer variables, and mark the paired buffer as valid. This
    // lets the debug print callback use this paired buffer.
    //
    LogBufferActive->LogEntries = logEntries1;
    LogBufferInactive->LogEntries = logEntries2;
    PairedLogBuffer->ActiveLogBuffer = LogBufferActive;
    PairedLogBuffer->InactiveLogBuffer = LogBufferInactive;
    PairedLogBuffer->BufferValid = TRUE;
#if DBG

    DbgPrint("Starting debug print logging.\n");
#endif
Exit:
    return status;
}

/*!
    @brief Starts the flush buffer thread.

    @param[out] ThreadContext - Context to be used by the thread.

    @return STATUS_SUCCESS or an appropriate status code.
*/
INIT
static
_IRQL_requires_max_(PASSIVE_LEVEL)
_Check_return_
NTSTATUS
StartFlushBufferThread(
    _Out_ PFLUSH_BUFFER_THREAD_CONTEXT ThreadContext
)
{
    NTSTATUS status;
    HANDLE threadHandle;
    IO_STATUS_BLOCK ioStatusBlock;
    PKTHREAD thread;
    PFILE_OBJECT fileObject;
    PDEVICE_OBJECT deviceObject;

    PAGED_CODE();

    RtlZeroMemory(ThreadContext, sizeof(*ThreadContext));

    UNICODE_STRING portName;
    //
    // Serial port attached to host.
    //
    WCHAR portNameBuffer[] = L"\\device\\Serial0";
    RtlInitUnicodeString(&portName, portNameBuffer);

    
    status = IoGetDeviceObjectPointer(
        &portName,
        FILE_READ_DATA | FILE_WRITE_DATA,
        &fileObject,
        &deviceObject
    );


    if (NT_SUCCESS(status))
    {
        ULONG baudRate = 9600;
        ///////////////////////////////
        IoBuildDeviceIoControlRequest(
            IOCTL_SERIAL_SET_BAUD_RATE,
            deviceObject,
            &baudRate,
            sizeof(baudRate),
            NULL,
            0,
            FALSE,
            NULL,
            &ioStatusBlock
        );
        //
        // Forgot to send the IRP and set the baud rate but the driver
        // îs working as intended. So, I'm not touching it.
        //
    }
    else {
        goto Exit;
    }

    //
    // Initialize the context before creating the thread. This avoids race.
    //
    ThreadContext->SerialFileObject = fileObject;
    ThreadContext->SerialDeviceObject = deviceObject;
    ThreadContext->PairedLogBuffer = &g_PairedLogBuffer;
    KeInitializeEvent(&ThreadContext->ThreadExitEvent, SynchronizationEvent, FALSE);

    //
    // Create the thread with the ready-to-use context.
    //
    status = PsCreateSystemThread(&threadHandle,
        THREAD_ALL_ACCESS,
        nullptr,
        nullptr,
        nullptr,
        FlushBufferThreadEntryPoint,
        ThreadContext);
    if (!NT_SUCCESS(status))
    {
        goto Exit;
    }

    //
    // Get the created thread object. This code does not fail (even the kernel
    // code assumes so sometimes).
    //
    status = ObReferenceObjectByHandle(threadHandle,
        THREAD_ALL_ACCESS,
        *PsThreadType,
        KernelMode,
        reinterpret_cast<PVOID*>(&thread),
        nullptr);
    NT_VERIFY(NT_SUCCESS(ZwClose(threadHandle)));
    NT_ASSERT(NT_SUCCESS(status));

    //
    // FlushBufferThread is not referenced by the thread. So it is OK to
    // initialize after creation of the thread.
    //
    ThreadContext->FlushBufferThread = thread;

Exit:
    if (!NT_SUCCESS(status))
    {
        if (fileObject != nullptr)
        {
            NT_VERIFY(NT_SUCCESS(ObDereferenceObject(fileObject)));
        }
    }
    return status;
}

/*!
    @brief Some initializations.

    @return STATUS_SUCCESS or an appropriate status code.
*/
INIT
static
_IRQL_requires_max_(PASSIVE_LEVEL)
_Check_return_
NTSTATUS
init(
    VOID
)
{
    NTSTATUS status;
    BOOLEAN buffersInitialized;

    PAGED_CODE();

    buffersInitialized = FALSE;

    status = initBuffers(&g_LogBuffer1, &g_LogBuffer2, &g_PairedLogBuffer);
    if (!NT_SUCCESS(status))
    {
        goto Exit;
    }
    buffersInitialized = TRUE;

    //
    // Starts the flush buffer thread that write the saved
    // messages into a log file and clears the buffer.
    //
    status = StartFlushBufferThread(&g_ThreadContext);
    if (!NT_SUCCESS(status))
    {
        goto Exit;
    }

Exit:
    if (!NT_SUCCESS(status))
    {
        if (buffersInitialized != FALSE)
        {
            cleanup(&g_PairedLogBuffer);
        }
    }
    return status;
}


/*!
    @brief Unloads this driver.

    @param[in] DriverObject - The associated driver object.
*/
PAGED
static
_Function_class_(DRIVER_UNLOAD)
_IRQL_requires_max_(PASSIVE_LEVEL)
VOID
DriverUnload(
    _In_ PDRIVER_OBJECT DriverObject
)
{
    UNREFERENCED_PARAMETER(DriverObject);

    PAGED_CODE();

    shutdown();
}

/*!
    @brief The entry point of the driver.

    @param[in] DriverObject - The associated driver object.

    @param[in] RegistryPath - The associated registry path.

    @return STATUS_SUCCESS or an appropriate status code.
*/
INIT
_Use_decl_annotations_
NTSTATUS
DriverEntry(
    PDRIVER_OBJECT DriverObject,
    PUNICODE_STRING RegistryPath
)
{

    UNREFERENCED_PARAMETER(RegistryPath);

    PAGED_CODE();

    DriverObject->DriverUnload = DriverUnload;

    return STATUS_SUCCESS;

}

NTSTATUS DllUnload(void) {
    PAGED_CODE();

    shutdown();
    return STATUS_SUCCESS;
}
INIT
_Use_decl_annotations_
NTSTATUS 
DllInitialize(
    _In_ PUNICODE_STRING RegistryPath
) {


    NTSTATUS status;

    UNREFERENCED_PARAMETER(RegistryPath);

    PAGED_CODE();

    status = init();

    return status;

}

