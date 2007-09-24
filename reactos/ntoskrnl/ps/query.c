/*
 * PROJECT:         ReactOS Kernel
 * LICENSE:         GPL - See COPYING in the top level directory
 * FILE:            ntoskrnl/ps/query.c
 * PURPOSE:         Process Manager: Thread/Process Query/Set Information
 * PROGRAMMERS:     Alex Ionescu (alex.ionescu@reactos.org)
 *                  Thomas Weidenmueller (w3seek@reactos.org)
 */

/* INCLUDES ******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <internal/debug.h>

/* Include Information Class Tables */
#include "internal/ps_i.h"

/* Debugging Level */
ULONG PspTraceLevel = 0;

/* PRIVATE FUNCTIONS *********************************************************/

NTSTATUS
NTAPI
PsReferenceProcessFilePointer(IN PEPROCESS Process,
                              OUT PFILE_OBJECT *FileObject)
{
    PSECTION Section;
    PAGED_CODE();

    /* Lock the process */
    ExAcquireRundownProtection(&Process->RundownProtect);

    /* Get the section */
    Section = Process->SectionObject;
    if (Section)
    {
        /* Get the file object and reference it */
        *FileObject = MmGetFileObjectForSection((PVOID)Section);
        ObReferenceObject(*FileObject);
    }

    /* Release the protection */
    ExReleaseRundownProtection(&Process->RundownProtect);

    /* Return status */
    return Section ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
}

/* PUBLIC FUNCTIONS **********************************************************/

/*
 * @implemented
 */
NTSTATUS
NTAPI
NtQueryInformationProcess(IN HANDLE ProcessHandle,
                          IN PROCESSINFOCLASS ProcessInformationClass,
                          OUT PVOID ProcessInformation,
                          IN ULONG ProcessInformationLength,
                          OUT PULONG ReturnLength OPTIONAL)
{
    PEPROCESS Process;
    KPROCESSOR_MODE PreviousMode = ExGetPreviousMode();
    NTSTATUS Status = STATUS_SUCCESS;
    ULONG Length = 0;
    PPROCESS_BASIC_INFORMATION ProcessBasicInfo =
        (PPROCESS_BASIC_INFORMATION)ProcessInformation;
    PKERNEL_USER_TIMES ProcessTime = (PKERNEL_USER_TIMES)ProcessInformation;
    ULONG HandleCount;
    PPROCESS_SESSION_INFORMATION SessionInfo =
        (PPROCESS_SESSION_INFORMATION)ProcessInformation;
    PVM_COUNTERS VmCounters = (PVM_COUNTERS)ProcessInformation;
    PROCESS_DEVICEMAP_INFORMATION DeviceMap;
    PUNICODE_STRING ImageName;
    ULONG Cookie;
    PAGED_CODE();

    /* Check validity of Information Class */
#if 0
    Status = DefaultQueryInfoBufferCheck(ProcessInformationClass,
                                         PsProcessInfoClass,
                                         RTL_NUMBER_OF(PsProcessInfoClass),
                                         ProcessInformation,
                                         ProcessInformationLength,
                                         ReturnLength,
                                         PreviousMode);
    if (!NT_SUCCESS(Status)) return Status;
#endif

    /* Check if this isn't the cookie class */
    if(ProcessInformationClass != ProcessCookie)
    {
        /* Reference the process */
        Status = ObReferenceObjectByHandle(ProcessHandle,
                                           PROCESS_QUERY_INFORMATION,
                                           PsProcessType,
                                           PreviousMode,
                                           (PVOID*)&Process,
                                           NULL);
        if (!NT_SUCCESS(Status)) return Status;
    }
    else if(ProcessHandle != NtCurrentProcess())
    {
        /*
         * Retreiving the process cookie is only allowed for the calling process
         * itself! XP only allowes NtCurrentProcess() as process handles even if
         * a real handle actually represents the current process.
         */
        return STATUS_INVALID_PARAMETER;
    }

    /* Check the information class */
    switch (ProcessInformationClass)
    {
        /* Basic process information */
        case ProcessBasicInformation:

            /* Set return length */
            Length = sizeof(PROCESS_BASIC_INFORMATION);

			if ( ProcessInformationLength != Length )
			{
				Status = STATUS_INFO_LENGTH_MISMATCH;
				break;
			}
            /* Protect writes with SEH */
            _SEH_TRY
            {
                /* Write all the information from the EPROCESS/KPROCESS */
                ProcessBasicInfo->ExitStatus = Process->ExitStatus;
                ProcessBasicInfo->PebBaseAddress = Process->Peb;
                ProcessBasicInfo->AffinityMask = Process->Pcb.Affinity;
                ProcessBasicInfo->UniqueProcessId = (ULONG)Process->
                                                    UniqueProcessId;
                ProcessBasicInfo->InheritedFromUniqueProcessId =
                    (ULONG)Process->InheritedFromUniqueProcessId;
                ProcessBasicInfo->BasePriority = Process->Pcb.BasePriority;

            }
            _SEH_HANDLE
            {
                /* Get exception code */
                Status = _SEH_GetExceptionCode();
            }
            _SEH_END;
            break;

        /* Quote limits and I/O Counters: not implemented */
        case ProcessQuotaLimits:
        case ProcessIoCounters:

			Length = sizeof(IO_COUNTERS);
			if ( ProcessInformationLength != Length )
			{
				Status = STATUS_INFO_LENGTH_MISMATCH;
				break;
			}

            Status = STATUS_NOT_IMPLEMENTED;
            break;

        /* Timing */
        case ProcessTimes:

            /* Set the return length */
            Length = sizeof(KERNEL_USER_TIMES);

			if ( ProcessInformationLength != Length )
			{
				Status = STATUS_INFO_LENGTH_MISMATCH;
				break;
			}

            /* Protect writes with SEH */
            _SEH_TRY
            {
                /* Copy time information from EPROCESS/KPROCESS */
                ProcessTime->CreateTime = Process->CreateTime;
                ProcessTime->UserTime.QuadPart = Process->Pcb.UserTime *
                                                 100000LL;
                ProcessTime->KernelTime.QuadPart = Process->Pcb.KernelTime *
                                                   100000LL;
                ProcessTime->ExitTime = Process->ExitTime;
            }
            _SEH_HANDLE
            {
                /* Get exception code */
                Status = _SEH_GetExceptionCode();
            }
            _SEH_END;
            break;

        /* Process Debug Port */
        case ProcessDebugPort:

            /* Protect write with SEH */
            _SEH_TRY
            {
                /* Return whether or not we have a debug port */
                *(PHANDLE)ProcessInformation = (Process->DebugPort ?
                                                (HANDLE)-1 : NULL);

                /* Set the return length*/
                Length = sizeof(HANDLE);
            }
            _SEH_HANDLE
            {
                /* Get exception code */
                Status = _SEH_GetExceptionCode();
            }
            _SEH_END;
            break;

        /* LDT, WS and VDM Information: not implemented */
        case ProcessLdtInformation:
        case ProcessWorkingSetWatch:
        case ProcessWx86Information:
            Status = STATUS_NOT_IMPLEMENTED;
            break;

        case ProcessHandleCount:

            /* Set the return length*/
            Length = sizeof(ULONG);

			if ( ProcessInformationLength != Length )
			{
				Status = STATUS_INFO_LENGTH_MISMATCH;
				break;
			}

            /* Count the number of handles this process has */
            HandleCount = ObpGetHandleCountByHandleTable(Process->ObjectTable);

            /* Protect write in SEH */
            _SEH_TRY
            {
                /* Return the count of handles */
                *(PULONG)ProcessInformation = HandleCount;
            }
            _SEH_HANDLE
            {
                /* Get the exception code */
                Status = _SEH_GetExceptionCode();
            }
            _SEH_END;
            break;

        /* Session ID for the process */
        case ProcessSessionInformation:

            /* Enter SEH for write safety */
            _SEH_TRY
            {
                /* Write back the Session ID */
                SessionInfo->SessionId = Process->Session;

                /* Set the return length */
                Length = sizeof(PROCESS_SESSION_INFORMATION);
            }
            _SEH_HANDLE
            {
                /* Get the exception code */
                Status = _SEH_GetExceptionCode();
            }
            _SEH_END;
            break;

        /* WOW64: Not implemented */
        case ProcessWow64Information:
            Status = STATUS_NOT_IMPLEMENTED;
            break;

        /* Virtual Memory Statistics */
        case ProcessVmCounters:

            /* Set the return length */
            Length = sizeof(VM_COUNTERS);

			if ( ProcessInformationLength != Length )
			{
				Status = STATUS_INFO_LENGTH_MISMATCH;
				break;
			}

            /* Enter SEH for write safety */
            _SEH_TRY
            {
                /* Return data from EPROCESS */
                VmCounters->PeakVirtualSize = Process->PeakVirtualSize;
                VmCounters->VirtualSize = Process->VirtualSize;
                VmCounters->PageFaultCount = Process->Vm.PageFaultCount;
                VmCounters->PeakWorkingSetSize = Process->Vm.PeakWorkingSetSize;
                VmCounters->WorkingSetSize = Process->Vm.WorkingSetSize;
                VmCounters->QuotaPeakPagedPoolUsage = Process->QuotaPeak[0];
                VmCounters->QuotaPagedPoolUsage = Process->QuotaUsage[0];
                VmCounters->QuotaPeakNonPagedPoolUsage = Process->QuotaPeak[1];
                VmCounters->QuotaNonPagedPoolUsage = Process->QuotaUsage[1];
                VmCounters->PagefileUsage = Process->QuotaUsage[2];
                VmCounters->PeakPagefileUsage = Process->QuotaPeak[2];

            }
            _SEH_HANDLE
            {
                /* Get the exception code */
                Status = _SEH_GetExceptionCode();
            }
            _SEH_END;
            break;

        /* Hard Error Processing Mode */
        case ProcessDefaultHardErrorMode:

            /* Enter SEH for writing back data */
            _SEH_TRY
            {
                /* Write the current processing mode */
                *(PULONG)ProcessInformation = Process->
                                              DefaultHardErrorProcessing;

                /* Set the return length */
                Length = sizeof(ULONG);
            }
            _SEH_HANDLE
            {
                /* Get the exception code */
                Status = _SEH_GetExceptionCode();
            }
            _SEH_END;
            break;

        /* Priority Boosting status */
        case ProcessPriorityBoost:

            /* Enter SEH for writing back data */
            _SEH_TRY
            {
                /* Return boost status */
                *(PULONG)ProcessInformation = Process->Pcb.DisableBoost ?
                                              FALSE : TRUE;

                /* Set the return length */
                Length = sizeof(ULONG);
            }
            _SEH_HANDLE
            {
                /* Get the exception code */
                Status = _SEH_GetExceptionCode();
            }
            _SEH_END;
            break;

        /* DOS Device Map */
        case ProcessDeviceMap:

            /* Query the device map information */
            ObQueryDeviceMapInformation(Process, &DeviceMap);

            /* Enter SEH for writing back data */
            _SEH_TRY
            {
                *(PPROCESS_DEVICEMAP_INFORMATION)ProcessInformation = DeviceMap;

                /* Set the return length */
                Length = sizeof(PROCESS_DEVICEMAP_INFORMATION);
            }
            _SEH_HANDLE
            {
                /* Get the exception code */
                Status = _SEH_GetExceptionCode();
            }
            _SEH_END;
            break;

        /* Priority class */
        case ProcessPriorityClass:

            /* Enter SEH for writing back data */
            _SEH_TRY
            {
                /* Return current priority class */
                *(PUSHORT)ProcessInformation = Process->PriorityClass;

                /* Set the return length */
                Length = sizeof(USHORT);
            }
            _SEH_HANDLE
            {
                /* Get the exception code */
                Status = _SEH_GetExceptionCode();
            }
            _SEH_END;
            break;

        case ProcessImageFileName:

            /* Get the image path */
            Status = SeLocateProcessImageName(Process, &ImageName);
            if (NT_SUCCESS(Status))
            {
                /* Set return length */
                Length = ImageName->MaximumLength +
                         sizeof(OBJECT_NAME_INFORMATION);

                /* Make sure it's large enough */
                if (Length <= ProcessInformationLength)
                {
                    /* Enter SEH to protect write */
                    _SEH_TRY
                    {
                        /* Copy it */
                        RtlCopyMemory(ProcessInformation,
                                      ImageName,
                                      Length);

                        /* Update pointer */
                        ((PUNICODE_STRING)ProcessInformation)->Buffer =
                            (PWSTR)((PUNICODE_STRING)ProcessInformation + 1);
                   }
                    _SEH_HANDLE
                    {
                        /* Get the exception code */
                        Status = _SEH_GetExceptionCode();
                    }
                    _SEH_END;
                }
            }
            break;

        /* Per-process security cookie */
        case ProcessCookie:

            /* Get the current process and cookie */
            Process = PsGetCurrentProcess();
            Cookie = Process->Cookie;
            if (!Cookie)
            {
                LARGE_INTEGER SystemTime;
                ULONG NewCookie;
                PKPRCB Prcb;

                /* Generate a new cookie */
                KeQuerySystemTime(&SystemTime);
                Prcb = KeGetCurrentPrcb();
                NewCookie = Prcb->KeSystemCalls ^ Prcb->InterruptTime ^
                            SystemTime.u.LowPart ^ SystemTime.u.HighPart;

                /* Set the new cookie or return the current one */
                Cookie = InterlockedCompareExchange((LONG*)&Process->Cookie,
                                                    NewCookie,
                                                    Cookie);
                if (!Cookie) Cookie = NewCookie;

                /* Set return length */
                Length = sizeof(ULONG);
            }

            /* Enter SEH to protect write */
            _SEH_TRY
            {
                /* Write back the cookie */
                *(PULONG)ProcessInformation = Cookie;
            }
            _SEH_HANDLE
            {
                /* Get the exception code */
                Status = _SEH_GetExceptionCode();
            }
            _SEH_END;
            break;

        /* Not yet implemented, or unknown */
        case ProcessBasePriority:
        case ProcessRaisePriority:
        case ProcessExceptionPort:
        case ProcessAccessToken:
        case ProcessLdtSize:
        case ProcessIoPortHandlers:
        case ProcessUserModeIOPL:
        case ProcessEnableAlignmentFaultFixup:
        case ProcessAffinityMask:
        case ProcessForegroundInformation:
        default:
            Status = STATUS_INVALID_INFO_CLASS;
    }

    /* Protect write with SEH */
    _SEH_TRY
    {
        /* Check if caller wanted return length */
        if (ReturnLength) *ReturnLength = Length;
    }
    _SEH_HANDLE
    {
        /* Get exception code */
        Status = _SEH_GetExceptionCode();
    }
    _SEH_END;

    /* If we referenced the process, dereference it */
    if(ProcessInformationClass != ProcessCookie) ObDereferenceObject(Process);
    return Status;
}

/*
 * @implemented
 */
NTSTATUS
NTAPI
NtSetInformationProcess(IN HANDLE ProcessHandle,
                        IN PROCESSINFOCLASS ProcessInformationClass,
                        IN PVOID ProcessInformation,
                        IN ULONG ProcessInformationLength)
{
    PEPROCESS Process;
    KPROCESSOR_MODE PreviousMode = ExGetPreviousMode();
    ACCESS_MASK Access;
    NTSTATUS Status;
    HANDLE PortHandle = NULL;
    HANDLE TokenHandle = NULL;
    PROCESS_SESSION_INFORMATION SessionInfo = {0};
    PVOID ExceptionPort;
    PAGED_CODE();

    /* Verify Information Class validity */
#if 0
    Status = DefaultSetInfoBufferCheck(ProcessInformationClass,
                                       PsProcessInfoClass,
                                       RTL_NUMBER_OF(PsProcessInfoClass),
                                       ProcessInformation,
                                       ProcessInformationLength,
                                       PreviousMode);
    if (!NT_SUCCESS(Status)) return Status;
#endif

    /* Check what class this is */
    Access = PROCESS_SET_INFORMATION;
    if (ProcessInformationClass == ProcessSessionInformation)
    {
        /* Setting the Session ID needs a special mask */
        Access |= PROCESS_SET_SESSIONID;
    }
    else if (ProcessInformationClass == ProcessExceptionPort)
    {
        /* Setting the exception port needs a special mask */
        Access |= PROCESS_SUSPEND_RESUME;
    }

    /* Reference the process */
    Status = ObReferenceObjectByHandle(ProcessHandle,
                                       Access,
                                       PsProcessType,
                                       PreviousMode,
                                       (PVOID*)&Process,
                                       NULL);
    if (!NT_SUCCESS(Status)) return Status;

    /* Check what kind of information class this is */
    switch (ProcessInformationClass)
    {
        /* Quotas and priorities: not implemented */
        case ProcessQuotaLimits:
        case ProcessBasePriority:
        case ProcessRaisePriority:
            Status = STATUS_NOT_IMPLEMENTED;
            break;

        /* Error/Exception Port */
        case ProcessExceptionPort:

            /* Use SEH for capture */
            _SEH_TRY
            {
                /* Capture the handle */
                PortHandle = *(PHANDLE)ProcessInformation;
            }
            _SEH_HANDLE
            {
                /* Get the exception code */
                Status = _SEH_GetExceptionCode();
            }
            _SEH_END;
            if (!NT_SUCCESS(Status)) break;

            /* Get the LPC Port */
            Status = ObReferenceObjectByHandle(PortHandle,
                                               0,
                                               LpcPortObjectType,
                                               PreviousMode,
                                               (PVOID)&ExceptionPort,
                                               NULL);
            if (!NT_SUCCESS(Status)) break;

            /* Change the pointer */
            if (InterlockedCompareExchangePointer(&Process->ExceptionPort,
                                                  ExceptionPort,
                                                  NULL))
            {
                /* We already had one, fail */
                ObDereferenceObject(ExceptionPort);
                Status = STATUS_PORT_ALREADY_SET;
            }
            break;

        /* Security Token */
        case ProcessAccessToken:

            /* Use SEH for capture */
            _SEH_TRY
            {
                /* Save the token handle */
                TokenHandle = ((PPROCESS_ACCESS_TOKEN)ProcessInformation)->
                               Token;
            }
            _SEH_HANDLE
            {
                /* Get the exception code */
                Status = _SEH_GetExceptionCode();
            }
            _SEH_END;
            if (!NT_SUCCESS(Status)) break;

            /* Assign the actual token */
            Status = PspSetPrimaryToken(Process, TokenHandle, NULL);
            break;

        /* Hard error processing */
        case ProcessDefaultHardErrorMode:

            /* Enter SEH for direct buffer read */
            _SEH_TRY
            {
                /* Update the current mode abd return the previous one */
                InterlockedExchange((LONG*)&Process->DefaultHardErrorProcessing,
                                    *(PLONG)ProcessInformation);
            }
            _SEH_HANDLE
            {
                /* Get exception code */
                Status = _SEH_GetExceptionCode();
            }
            _SEH_END;
            break;

        /* Session ID */
        case ProcessSessionInformation:

            /* Enter SEH for capture */
            _SEH_TRY
            {
                /* Capture the caller's buffer */
                SessionInfo = *(PPROCESS_SESSION_INFORMATION)ProcessInformation;
            }
            _SEH_HANDLE
            {
                /* Get the exception code */
                Status = _SEH_GetExceptionCode();
            }
            _SEH_END;
            if (!NT_SUCCESS(Status)) break;

            /* Setting the session id requires the SeTcbPrivilege */
            if (!SeSinglePrivilegeCheck(SeTcbPrivilege, PreviousMode))
            {
                /* Can't set the session ID, bail out. */
                Status = STATUS_PRIVILEGE_NOT_HELD;
                break;
            }

            /* FIXME - update the session id for the process token */
            //Status = PsLockProcess(Process, FALSE);
            if (!NT_SUCCESS(Status)) break;

            /* Write the session ID in the EPROCESS */
            Process->Session = SessionInfo.SessionId;

            /* Check if the process also has a PEB */
            if (Process->Peb)
            {
                /*
                 * Attach to the process to make sure we're in the right
                 * context to access the PEB structure
                 */
                KeAttachProcess(&Process->Pcb);

                /* Enter SEH for write to user-mode PEB */
                _SEH_TRY
                {
                    /* Write the session ID */
                    Process->Peb->SessionId = SessionInfo.SessionId;
                }
                _SEH_HANDLE
                {
                    /* Get exception code */
                    Status = _SEH_GetExceptionCode();
                }
                _SEH_END;

                /* Detach from the process */
                KeDetachProcess();
            }

            /* Unlock the process */
            //PsUnlockProcess(Process);
            break;

        /* Priority class: HACK! */
        case ProcessPriorityClass:
            break;

        /* We currently don't implement any of these */
        case ProcessLdtInformation:
        case ProcessLdtSize:
        case ProcessIoPortHandlers:
        case ProcessWorkingSetWatch:
        case ProcessUserModeIOPL:
        case ProcessEnableAlignmentFaultFixup:
        case ProcessAffinityMask:
            Status = STATUS_NOT_IMPLEMENTED;
            break;

        /* Supposedly these are invalid...!? verify! */
        case ProcessBasicInformation:
        case ProcessIoCounters:
        case ProcessTimes:
        case ProcessPooledUsageAndLimits:
        case ProcessWx86Information:
        case ProcessHandleCount:
        case ProcessWow64Information:
        case ProcessDebugPort:
        default:
            Status = STATUS_INVALID_INFO_CLASS;
    }

    /* Dereference and return status */
    ObDereferenceObject(Process);
    return Status;
}

/*
 * @implemented
 */
NTSTATUS
NTAPI
NtSetInformationThread(IN HANDLE ThreadHandle,
                       IN THREADINFOCLASS ThreadInformationClass,
                       IN PVOID ThreadInformation,
                       IN ULONG ThreadInformationLength)
{
    PETHREAD Thread;
    ULONG Access;
    KPROCESSOR_MODE PreviousMode = ExGetPreviousMode();
    NTSTATUS Status;
    HANDLE TokenHandle = NULL;
    KPRIORITY Priority = 0;
    KAFFINITY Affinity = 0, CombinedAffinity;
    PVOID Address = NULL;
    PEPROCESS Process;
    ULONG DisableBoost = 0;
    ULONG IdealProcessor = 0;
    PTEB Teb;
    ULONG TlsIndex = 0;
    PVOID *ExpansionSlots;
    PETHREAD ProcThread;
    PAGED_CODE();

    /* Verify Information Class validity */
#if 0
    Status = DefaultSetInfoBufferCheck(ThreadInformationClass,
                                       PsThreadInfoClass,
                                       RTL_NUMBER_OF(PsThreadInfoClass),
                                       ThreadInformation,
                                       ThreadInformationLength,
                                       PreviousMode);
    if (!NT_SUCCESS(Status)) return Status;
#endif

    /* Check what class this is */
    Access = THREAD_SET_INFORMATION;
    if (ThreadInformationClass == ThreadImpersonationToken)
    {
        /* Setting the impersonation token needs a special mask */
        Access = THREAD_SET_THREAD_TOKEN;
    }

    /* Reference the process */
    Status = ObReferenceObjectByHandle(ThreadHandle,
                                       Access,
                                       PsThreadType,
                                       PreviousMode,
                                       (PVOID*)&Thread,
                                       NULL);
    if (!NT_SUCCESS(Status)) return Status;

    /* Check what kind of information class this is */
    switch (ThreadInformationClass)
    {
        /* Thread priority */
        case ThreadPriority:

            /* Use SEH for capture */
            _SEH_TRY
            {
                /* Get the priority */
                Priority = *(PLONG)ThreadInformation;
            }
            _SEH_HANDLE
            {
                /* Get the exception code */
                Status = _SEH_GetExceptionCode();
            }
            _SEH_END;
            if (!NT_SUCCESS(Status)) break;

            /* Validate it */
            if ((Priority > HIGH_PRIORITY) ||
                (Priority <= LOW_PRIORITY))
            {
                /* Fail */
                Status = STATUS_INVALID_PARAMETER;
                break;
            }

            /* Set the priority */
            KeSetPriorityThread(&Thread->Tcb, Priority);
            break;

        case ThreadBasePriority:

            /* Use SEH for capture */
            _SEH_TRY
            {
                /* Get the priority */
                Priority = *(PLONG)ThreadInformation;
            }
            _SEH_HANDLE
            {
                /* Get the exception code */
                Status = _SEH_GetExceptionCode();
            }
            _SEH_END;
            if (!NT_SUCCESS(Status)) break;

            /* Validate it */
            if ((Priority > THREAD_BASE_PRIORITY_MAX) ||
                (Priority < THREAD_BASE_PRIORITY_MIN))
            {
                /* These ones are OK */
                if ((Priority != THREAD_BASE_PRIORITY_LOWRT + 1) &&
                    (Priority != THREAD_BASE_PRIORITY_IDLE - 1))
                {
                    /* Check if the process is real time */
                    if (PsGetCurrentProcess()->PriorityClass !=
                        PROCESS_PRIORITY_CLASS_REALTIME)
                    {
                        /* It isn't, fail */
                        Status = STATUS_INVALID_PARAMETER;
                        break;
                    }
                }
            }

            /* Set the base priority */
            KeSetBasePriorityThread(&Thread->Tcb, Priority);
            break;

        case ThreadAffinityMask:

            /* Use SEH for capture */
            _SEH_TRY
            {
                /* Get the priority */
                Affinity = *(PULONG_PTR)ThreadInformation;
            }
            _SEH_HANDLE
            {
                /* Get the exception code */
                Status = _SEH_GetExceptionCode();
            }
            _SEH_END;
            if (!NT_SUCCESS(Status)) break;

            /* Validate it */
            if (!Affinity)
            {
                /* Fail */
                Status = STATUS_INVALID_PARAMETER;
                break;
            }

            /* Get the process */
            Process = Thread->ThreadsProcess;

            /* Try to acquire rundown */
            if (ExAcquireRundownProtection(&Process->RundownProtect))
            {
                /* Lock it */
                KeEnterCriticalRegion();
                ExAcquirePushLockShared(&Process->ProcessLock);

                /* Combine masks */
                CombinedAffinity = Affinity & Process->Pcb.Affinity;
                if (CombinedAffinity != Affinity)
                {
                    /* Fail */
                    Status = STATUS_INVALID_PARAMETER;
                }
                else
                {
                    /* Set the affinity */
                    KeSetAffinityThread(&Thread->Tcb, CombinedAffinity);
                }

                /* Release the lock and rundown */
                ExReleasePushLockShared(&Process->ProcessLock);
                KeLeaveCriticalRegion();
                ExReleaseRundownProtection(&Process->RundownProtect);
            }
            else
            {
                /* Too late */
                Status = STATUS_PROCESS_IS_TERMINATING;
            }

            /* Return status */
            break;

        case ThreadImpersonationToken:

            /* Use SEH for capture */
            _SEH_TRY
            {
                /* Save the token handle */
                TokenHandle = *(PHANDLE)ThreadInformation;
            }
            _SEH_HANDLE
            {
                /* Get the exception code */
                Status = _SEH_GetExceptionCode();
            }
            _SEH_END;
            if (!NT_SUCCESS(Status)) break;

            /* Assign the actual token */
            Status = PsAssignImpersonationToken(Thread, TokenHandle);
            break;

        case ThreadQuerySetWin32StartAddress:

            /* Use SEH for capture */
            _SEH_TRY
            {
                /* Get the priority */
                Address = *(PVOID*)ThreadInformation;
            }
            _SEH_HANDLE
            {
                /* Get the exception code */
                Status = _SEH_GetExceptionCode();
            }
            _SEH_END;
            if (!NT_SUCCESS(Status)) break;

            /* Set the address */
            Thread->Win32StartAddress = Address;
            break;

        case ThreadIdealProcessor:

            /* Use SEH for capture */
            _SEH_TRY
            {
                /* Get the priority */
                IdealProcessor = *(PULONG_PTR)ThreadInformation;
            }
            _SEH_HANDLE
            {
                /* Get the exception code */
                Status = _SEH_GetExceptionCode();
            }
            _SEH_END;
            if (!NT_SUCCESS(Status)) break;

            /* Validate it */
            if (IdealProcessor > MAXIMUM_PROCESSORS)
            {
                /* Fail */
                Status = STATUS_INVALID_PARAMETER;
                break;
            }

            /* Set the ideal */
            Status = KeSetIdealProcessorThread(&Thread->Tcb,
                                               (CCHAR)IdealProcessor);

            /* Get the TEB and protect the thread */
            Teb = Thread->Tcb.Teb;
            if ((Teb) && (ExAcquireRundownProtection(&Thread->RundownProtect)))
            {
                /* Save the ideal processor */
                Teb->IdealProcessor = Thread->Tcb.IdealProcessor;

                /* Release rundown protection */
                ExReleaseRundownProtection(&Thread->RundownProtect);
            }

            break;

        case ThreadPriorityBoost:

            /* Use SEH for capture */
            _SEH_TRY
            {
                /* Get the priority */
                DisableBoost = *(PULONG_PTR)ThreadInformation;
            }
            _SEH_HANDLE
            {
                /* Get the exception code */
                Status = _SEH_GetExceptionCode();
            }
            _SEH_END;
            if (!NT_SUCCESS(Status)) break;

            /* Call the kernel */
            KeSetDisableBoostThread(&Thread->Tcb, (BOOLEAN)DisableBoost);
            break;

        case ThreadZeroTlsCell:

            /* Use SEH for capture */
            _SEH_TRY
            {
                /* Get the priority */
                TlsIndex = *(PULONG_PTR)ThreadInformation;
            }
            _SEH_HANDLE
            {
                /* Get the exception code */
                Status = _SEH_GetExceptionCode();
            }
            _SEH_END;
            if (!NT_SUCCESS(Status)) break;

            /* This is only valid for the current thread */
            if (Thread != PsGetCurrentThread())
            {
                /* Fail */
                Status = STATUS_INVALID_PARAMETER;
                break;
            }

            /* Get the process */
            Process = Thread->ThreadsProcess;

            /* Loop the threads */
            ProcThread = PsGetNextProcessThread(Process, NULL);
            while (ProcThread)
            {
                /* Acquire rundown */
                if (ExAcquireRundownProtection(&ProcThread->RundownProtect))
                {
                    /* Get the TEB */
                    Teb = ProcThread->Tcb.Teb;
                    if (Teb)
                    {
                        /* Check if we're in the expansion range */
                        if (TlsIndex > TLS_MINIMUM_AVAILABLE - 1)
                        {
                            if (TlsIndex < (TLS_MINIMUM_AVAILABLE +
                                            TLS_EXPANSION_SLOTS) - 1)
                            {
                                /* Check if we have expansion slots */
                                ExpansionSlots = Teb->TlsExpansionSlots;
                                if (ExpansionSlots)
                                {
                                    /* Clear the index */
                                    ExpansionSlots[TlsIndex - TLS_MINIMUM_AVAILABLE] = 0;
                                }
                            }
                        }
                        else
                        {
                            /* Clear the index */
                            Teb->TlsSlots[TlsIndex] = NULL;
                        }
                    }

                    /* Release rundown */
                    ExReleaseRundownProtection(&ProcThread->RundownProtect);
                }

                /* Go to the next thread */
                ProcThread = PsGetNextProcessThread(Process, ProcThread);
            }

            /* All done */
            break;

        default:
            /* We don't implement it yet */
            DPRINT1("Not implemented: %lx\n", ThreadInformationClass);
            Status = STATUS_NOT_IMPLEMENTED;
    }

    /* Dereference and return status */
    ObDereferenceObject(Thread);
    return Status;
}

/*
 * @implemented
 */
NTSTATUS
NTAPI
NtQueryInformationThread(IN HANDLE ThreadHandle,
                         IN THREADINFOCLASS ThreadInformationClass,
                         OUT PVOID ThreadInformation,
                         IN ULONG ThreadInformationLength,
                         OUT PULONG ReturnLength OPTIONAL)
{
    PETHREAD Thread;
    KPROCESSOR_MODE PreviousMode = ExGetPreviousMode();
    NTSTATUS Status = STATUS_SUCCESS;
    ULONG Access;
    ULONG Length = 0;
    PTHREAD_BASIC_INFORMATION ThreadBasicInfo =
        (PTHREAD_BASIC_INFORMATION)ThreadInformation;
    PKERNEL_USER_TIMES ThreadTime = (PKERNEL_USER_TIMES)ThreadInformation;
    KIRQL OldIrql;
    PAGED_CODE();

    /* Verify Information Class validity */
#if 0
    Status = DefaultQueryInfoBufferCheck(ThreadInformationClass,
                                         PsThreadInfoClass,
                                         RTL_NUMBER_OF(PsThreadInfoClass),
                                         ThreadInformation,
                                         ThreadInformationLength,
                                         ReturnLength,
                                         PreviousMode);
    if (!NT_SUCCESS(Status)) return Status;
#endif

    /* Check what class this is */
    Access = THREAD_QUERY_INFORMATION;

    /* Reference the process */
    Status = ObReferenceObjectByHandle(ThreadHandle,
                                       Access,
                                       PsThreadType,
                                       PreviousMode,
                                       (PVOID*)&Thread,
                                       NULL);
    if (!NT_SUCCESS(Status)) return Status;

    /* Check what kind of information class this is */
    switch (ThreadInformationClass)
    {
        /* Basic thread information */
        case ThreadBasicInformation:

            /* Protect writes with SEH */
            _SEH_TRY
            {
                /* Write all the information from the ETHREAD/KTHREAD */
                ThreadBasicInfo->ExitStatus = Thread->ExitStatus;
                ThreadBasicInfo->TebBaseAddress = (PVOID)Thread->Tcb.Teb;
                ThreadBasicInfo->ClientId = Thread->Cid;
                ThreadBasicInfo->AffinityMask = Thread->Tcb.Affinity;
                ThreadBasicInfo->Priority = Thread->Tcb.Priority;
                ThreadBasicInfo->BasePriority = KeQueryBasePriorityThread(&Thread->Tcb);

                /* Set return length */
                Length = sizeof(THREAD_BASIC_INFORMATION);
            }
            _SEH_HANDLE
            {
                /* Get exception code */
                Status = _SEH_GetExceptionCode();
            }
            _SEH_END;
            break;

        /* Thread time information */
        case ThreadTimes:

            /* Protect writes with SEH */
            _SEH_TRY
            {
                /* Copy time information from ETHREAD/KTHREAD */
                ThreadTime->KernelTime.QuadPart = Thread->Tcb.KernelTime *
                                                   100000LL;
                ThreadTime->UserTime.QuadPart = Thread->Tcb.UserTime *
                                                 100000LL;
                ThreadTime->CreateTime = Thread->CreateTime;
                ThreadTime->ExitTime = Thread->ExitTime;

                /* Set the return length */
                Length = sizeof(KERNEL_USER_TIMES);
            }
            _SEH_HANDLE
            {
                /* Get exception code */
                Status = _SEH_GetExceptionCode();
            }
            _SEH_END;
            break;

        case ThreadQuerySetWin32StartAddress:

            /* Protect write with SEH */
            _SEH_TRY
            {
                /* Return the Win32 Start Address */
                *(PVOID*)ThreadInformation = Thread->Win32StartAddress;

                /* Set the return length*/
                Length = sizeof(PVOID);
            }
            _SEH_HANDLE
            {
                /* Get exception code */
                Status = _SEH_GetExceptionCode();
            }
            _SEH_END;
            break;

        case ThreadPerformanceCount:

            /* Protect write with SEH */
            _SEH_TRY
            {
                /* FIXME */
                (*(PLARGE_INTEGER)ThreadInformation).QuadPart = 0;

                /* Set the return length*/
                Length = sizeof(LARGE_INTEGER);
            }
            _SEH_HANDLE
            {
                /* Get exception code */
                Status = _SEH_GetExceptionCode();
            }
            _SEH_END;
            break;

        case ThreadAmILastThread:

            /* Protect write with SEH */
            _SEH_TRY
            {
                /* Return whether or not we are the last thread */
                *(PULONG)ThreadInformation = ((Thread->ThreadsProcess->
                                               ThreadListHead.Flink->Flink ==
                                               &Thread->ThreadsProcess->
                                               ThreadListHead) ?
                                              TRUE : FALSE);

                /* Set the return length*/
                Length = sizeof(ULONG);
            }
            _SEH_HANDLE
            {
                /* Get exception code */
                Status = _SEH_GetExceptionCode();
            }
            _SEH_END;
            break;

        case ThreadIsIoPending:

            /* Raise the IRQL to protect the IRP list */
            KeRaiseIrql(APC_LEVEL, &OldIrql);

            /* Protect write with SEH */
            _SEH_TRY
            {
                /* Check if the IRP list is empty or not */
                *(PULONG)ThreadInformation = !IsListEmpty(&Thread->IrpList);

                /* Set the return length*/
                Length = sizeof(ULONG);
            }
            _SEH_HANDLE
            {
                /* Get exception code */
                Status = _SEH_GetExceptionCode();
            }
            _SEH_END;

            /* Lower IRQL back */
            KeLowerIrql(OldIrql);
            break;

        /* Anything else */
        default:

            /* Not yet implemented */
            DPRINT1("Not implemented: %lx\n", ThreadInformationClass);
            Status = STATUS_NOT_IMPLEMENTED;
    }

    /* Protect write with SEH */
    _SEH_TRY
    {
        /* Check if caller wanted return length */
        if (ReturnLength) *ReturnLength = Length;
    }
    _SEH_HANDLE
    {
        /* Get exception code */
        Status = _SEH_GetExceptionCode();
    }
    _SEH_END;

    /* Dereference the thread, and return */
    ObDereferenceObject(Thread);
    return Status;
}

/* EOF */
