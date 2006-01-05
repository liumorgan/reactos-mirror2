#ifndef __NTOSKRNL_INCLUDE_INTERNAL_KE_H
#define __NTOSKRNL_INCLUDE_INTERNAL_KE_H

/* INCLUDES *****************************************************************/

#include "arch/ke.h"

/* INTERNAL KERNEL TYPES ****************************************************/

typedef struct _WOW64_PROCESS
{
  PVOID Wow64;
} WOW64_PROCESS, *PWOW64_PROCESS;

typedef struct _KPROFILE_SOURCE_OBJECT
{
    KPROFILE_SOURCE Source;
    LIST_ENTRY ListEntry;
} KPROFILE_SOURCE_OBJECT, *PKPROFILE_SOURCE_OBJECT;

/* Cached modules from the loader block */
typedef enum _CACHED_MODULE_TYPE
{
    AnsiCodepage,
    OemCodepage,
    UnicodeCasemap,
    SystemRegistry,
    HardwareRegistry,
    MaximumCachedModuleType,
} CACHED_MODULE_TYPE, *PCACHED_MODULE_TYPE;
extern PLOADER_MODULE CachedModules[MaximumCachedModuleType];

struct _KIRQ_TRAPFRAME;
struct _KPCR;
struct _KPRCB;
struct _KEXCEPTION_FRAME;

extern PVOID KeUserApcDispatcher;
extern PVOID KeUserCallbackDispatcher;
extern PVOID KeUserExceptionDispatcher;
extern PVOID KeRaiseUserExceptionDispatcher;
extern LARGE_INTEGER SystemBootTime;
extern ULONG_PTR KERNEL_BASE;

/* MACROS *************************************************************************/

/*
 * On UP machines, we don't actually have a spinlock, we merely raise
 * IRQL to DPC level.
 */
#ifndef CONFIG_SMP
#define KeInitializeDispatcher()
#define KeAcquireDispatcherDatabaseLock() KeRaiseIrqlToDpcLevel();
#define KeAcquireDispatcherDatabaseLockAtDpcLevel()
#define KeReleaseDispatcherDatabaseLockFromDpcLevel()
#else
#define KeInitializeDispatcher() KeInitializeSpinLock(&DispatcherDatabaseLock);
#define KeAcquireDispatcherDatabaseLock() KfAcquireSpinLock(&DispatcherDatabaseLock);
#define KeAcquireDispatcherDatabaseLockAtDpcLevel() \
    KeAcquireSpinLockAtDpcLevel (&DispatcherDatabaseLock);
#define KeReleaseDispatcherDatabaseLockFromDpcLevel() \
    KeReleaseSpinLockFromDpcLevel(&DispatcherDatabaseLock);
#endif

/* The following macro initializes a dispatcher object's header */
#define KeInitializeDispatcherHeader(Header, t, s, State)                   \
{                                                                           \
    (Header)->Type = t;                                                     \
    (Header)->Absolute = 0;                                                 \
    (Header)->Inserted = 0;                                                 \
    (Header)->Size = s;                                                     \
    (Header)->SignalState = State;                                          \
    InitializeListHead(&((Header)->WaitListHead));                          \
}

/* The following macro satisfies the wait of any dispatcher object */
#define KiSatisfyObjectWait(Object, Thread)                                 \
{                                                                           \
    /* Special case for Mutants */                                          \
    if ((Object)->Header.Type == MutantObject)                              \
    {                                                                       \
        /* Decrease the Signal State */                                     \
        (Object)->Header.SignalState--;                                     \
                                                                            \
        /* Check if it's now non-signaled */                                \
        if (!(Object)->Header.SignalState)                                  \
        {                                                                   \
            /* Set the Owner Thread */                                      \
            (Object)->OwnerThread = Thread;                                 \
                                                                            \
            /* Disable APCs if needed */                                    \
            Thread->KernelApcDisable -= (Object)->ApcDisable;               \
                                                                            \
            /* Check if it's abandoned */                                   \
            if ((Object)->Abandoned)                                        \
            {                                                               \
                /* Unabandon it */                                          \
                (Object)->Abandoned = FALSE;                                \
                                                                            \
                /* Return Status */                                         \
                Thread->WaitStatus = STATUS_ABANDONED;                      \
            }                                                               \
                                                                            \
            /* Insert it into the Mutant List */                            \
            InsertHeadList(&Thread->MutantListHead,                         \
                           &(Object)->MutantListEntry);                     \
        }                                                                   \
    }                                                                       \
    else if (((Object)->Header.Type & TIMER_OR_EVENT_TYPE) ==               \
             EventSynchronizationObject)                                    \
    {                                                                       \
        /* Synchronization Timers and Events just get un-signaled */        \
        (Object)->Header.SignalState = 0;                                   \
    }                                                                       \
    else if ((Object)->Header.Type == SemaphoreObject)                      \
    {                                                                       \
        /* These ones can have multiple states, so we only decrease it */   \
        (Object)->Header.SignalState--;                                     \
    }                                                                       \
}

/* The following macro satisfies the wait of a mutant dispatcher object */
#define KiSatisfyMutantWait(Object, Thread)                                 \
{                                                                           \
    /* Decrease the Signal State */                                         \
    (Object)->Header.SignalState--;                                         \
                                                                            \
    /* Check if it's now non-signaled */                                    \
    if (!(Object)->Header.SignalState)                                      \
    {                                                                       \
        /* Set the Owner Thread */                                          \
        (Object)->OwnerThread = Thread;                                     \
                                                                            \
        /* Disable APCs if needed */                                        \
        Thread->KernelApcDisable -= (Object)->ApcDisable;                   \
                                                                            \
        /* Check if it's abandoned */                                       \
        if ((Object)->Abandoned)                                            \
        {                                                                   \
            /* Unabandon it */                                              \
            (Object)->Abandoned = FALSE;                                    \
                                                                            \
            /* Return Status */                                             \
            Thread->WaitStatus = STATUS_ABANDONED;                          \
        }                                                                   \
                                                                            \
        /* Insert it into the Mutant List */                                \
        InsertHeadList(&Thread->MutantListHead,                             \
                       &(Object)->MutantListEntry);                         \
    }                                                                       \
}

/* The following macro satisfies the wait of any nonmutant dispatcher object */
#define KiSatisfyNonMutantWait(Object, Thread)                              \
{                                                                           \
    if (((Object)->Header.Type & TIMER_OR_EVENT_TYPE) ==                    \
             EventSynchronizationObject)                                    \
    {                                                                       \
        /* Synchronization Timers and Events just get un-signaled */        \
        (Object)->Header.SignalState = 0;                                   \
    }                                                                       \
    else if ((Object)->Header.Type == SemaphoreObject)                      \
    {                                                                       \
        /* These ones can have multiple states, so we only decrease it */   \
        (Object)->Header.SignalState--;                                     \
    }                                                                       \
}

/* The following macro satisfies multiple objects in a wait state */
#define KiSatisifyMultipleObjectWaits(FirstBlock)                           \
{                                                                           \
    PKWAIT_BLOCK WaitBlock = FirstBlock;                                    \
    PKTHREAD WaitThread = WaitBlock->Thread;                                \
                                                                            \
    /* Loop through all the Wait Blocks, and wake each Object */            \
    do                                                                      \
    {                                                                       \
        /* Make sure it hasn't timed out */                                 \
        if (WaitBlock->WaitKey != STATUS_TIMEOUT)                           \
        {                                                                   \
            /* Wake the Object */                                           \
            KiSatisfyObjectWait((PKMUTANT)WaitBlock->Object, WaitThread);   \
        }                                                                   \
                                                                            \
        /* Move to the next block */                                        \
        WaitBlock = WaitBlock->NextWaitBlock;                               \
    } while (WaitBlock != FirstBlock);                                      \
}

extern KSPIN_LOCK DispatcherDatabaseLock;

#define KeEnterCriticalRegion()                                             \
{                                                                           \
    PKTHREAD _Thread = KeGetCurrentThread();                                \
    if (_Thread) _Thread->KernelApcDisable--;                               \
}

#define KeLeaveCriticalRegion()                                             \
{                                                                           \
    PKTHREAD _Thread = KeGetCurrentThread();                                \
    if((_Thread) && (++_Thread->KernelApcDisable == 0))                     \
    {                                                                       \
        if (!IsListEmpty(&_Thread->ApcState.ApcListHead[KernelMode]) &&     \
            (_Thread->SpecialApcDisable == 0))                              \
        {                                                                   \
            KiCheckForKernelApcDelivery();                                  \
        }                                                                   \
    }                                                                       \
}

#define KEBUGCHECKWITHTF(a,b,c,d,e,f) \
    DbgPrint("KeBugCheckWithTf at %s:%i\n",__FILE__,__LINE__), \
             KeBugCheckWithTf(a,b,c,d,e,f)

/* INTERNAL KERNEL FUNCTIONS ************************************************/

/* threadsch.c ********************************************************************/

/* Thread Scheduler Functions */

/* Readies a Thread for Execution. */
VOID
STDCALL
KiDispatchThreadNoLock(ULONG NewThreadStatus);

/* Readies a Thread for Execution. */
VOID
STDCALL
KiDispatchThread(ULONG NewThreadStatus);

/* Puts a Thread into a block state. */
VOID
STDCALL
KiBlockThread(
    PNTSTATUS Status,
    UCHAR Alertable,
    ULONG WaitMode,
    UCHAR WaitReason
);

/* Removes a thread out of a block state. */
VOID
STDCALL
KiUnblockThread(
    PKTHREAD Thread,
    PNTSTATUS WaitStatus,
    KPRIORITY Increment
);

NTSTATUS
STDCALL
KeSuspendThread(PKTHREAD Thread);

NTSTATUS
FASTCALL
KiSwapContext(PKTHREAD NewThread);

VOID
STDCALL
KiAdjustQuantumThread(IN PKTHREAD Thread);

/* gmutex.c ********************************************************************/

VOID
FASTCALL
KiAcquireGuardedMutexContented(PKGUARDED_MUTEX GuardedMutex);

/* gate.c **********************************************************************/

VOID
FASTCALL
KeInitializeGate(PKGATE Gate);

VOID
FASTCALL
KeSignalGateBoostPriority(PKGATE Gate);

VOID
FASTCALL
KeWaitForGate(
    PKGATE Gate,
    KWAIT_REASON WaitReason,
    KPROCESSOR_MODE WaitMode
);

/* ipi.c ********************************************************************/

BOOLEAN
STDCALL
KiIpiServiceRoutine(
    IN PKTRAP_FRAME TrapFrame,
    IN struct _KEXCEPTION_FRAME* ExceptionFrame
);

VOID
NTAPI
KiIpiSendRequest(
    KAFFINITY TargetSet,
    ULONG IpiRequest
);

VOID
NTAPI
KeIpiGenericCall(
    VOID (STDCALL *WorkerRoutine)(PVOID),
    PVOID Argument
);

/* next file ***************************************************************/

VOID 
STDCALL
DbgBreakPointNoBugCheck(VOID);

VOID
STDCALL
KeInitializeProfile(
    struct _KPROFILE* Profile,
    struct _KPROCESS* Process,
    PVOID ImageBase,
    ULONG ImageSize,
    ULONG BucketSize,
    KPROFILE_SOURCE ProfileSource,
    KAFFINITY Affinity
);

VOID
STDCALL
KeStartProfile(
    struct _KPROFILE* Profile,
    PVOID Buffer
);

BOOLEAN
STDCALL
KeStopProfile(struct _KPROFILE* Profile);

ULONG
STDCALL
KeQueryIntervalProfile(KPROFILE_SOURCE ProfileSource);

VOID
STDCALL
KeSetIntervalProfile(
    KPROFILE_SOURCE ProfileSource,
    ULONG Interval
);

VOID
STDCALL
KeProfileInterrupt(
    PKTRAP_FRAME TrapFrame
);

VOID
STDCALL
KeProfileInterruptWithSource(
    IN PKTRAP_FRAME TrapFrame,
    IN KPROFILE_SOURCE Source
);

BOOLEAN
STDCALL
KiRosPrintAddress(PVOID Address);

VOID
STDCALL
KeUpdateSystemTime(
    PKTRAP_FRAME TrapFrame,
    KIRQL Irql
);

VOID
STDCALL
KeUpdateRunTime(
    PKTRAP_FRAME TrapFrame,
    KIRQL Irql
);

VOID
STDCALL
KiExpireTimers(
    PKDPC Dpc,
    PVOID DeferredContext,
    PVOID SystemArgument1,
    PVOID SystemArgument2
);

VOID
FASTCALL
KeReleaseDispatcherDatabaseLock(KIRQL Irql);

VOID
STDCALL
KeInitializeThread(
    struct _KPROCESS* Process,
    PKTHREAD Thread,
    PKSYSTEM_ROUTINE SystemRoutine,
    PKSTART_ROUTINE StartRoutine,
    PVOID StartContext,
    PCONTEXT Context,
    PVOID Teb,
    PVOID KernelStack
);

VOID
STDCALL
KeRundownThread(VOID);

NTSTATUS
NTAPI
KeReleaseThread(PKTHREAD Thread);

LONG
STDCALL
KeQueryBasePriorityThread(IN PKTHREAD Thread);

VOID
STDCALL
KiSetPriorityThread(
    PKTHREAD Thread,
    KPRIORITY Priority,
    PBOOLEAN Released
);

BOOLEAN
NTAPI
KiDispatcherObjectWake(
    DISPATCHER_HEADER* hdr,
    KPRIORITY increment
);

VOID
STDCALL
KeExpireTimers(
    PKDPC Apc,
    PVOID Arg1,
    PVOID Arg2,
    PVOID Arg3
);

VOID
NTAPI
KeDumpStackFrames(PULONG Frame);

BOOLEAN
NTAPI
KiTestAlert(VOID);

VOID
FASTCALL
KiAbortWaitThread(
    PKTHREAD Thread,
    NTSTATUS WaitStatus,
    KPRIORITY Increment
);

VOID
STDCALL
KeInitializeProcess(
    struct _KPROCESS *Process,
    KPRIORITY Priority,
    KAFFINITY Affinity,
    LARGE_INTEGER DirectoryTableBase
);

ULONG
STDCALL
KeForceResumeThread(IN PKTHREAD Thread);

BOOLEAN
STDCALL
KeDisableThreadApcQueueing(IN PKTHREAD Thread);

BOOLEAN
STDCALL
KiInsertTimer(
    PKTIMER Timer,
    LARGE_INTEGER DueTime
);

BOOLEAN
__inline
FASTCALL
KiIsObjectSignaled(
    PDISPATCHER_HEADER Object,
    PKTHREAD Thread
);

VOID
FASTCALL
KiWaitTest(
    PVOID Object,
    KPRIORITY Increment
);

PULONG 
NTAPI
KeGetStackTopThread(struct _ETHREAD* Thread);

BOOLEAN
STDCALL
KeContextToTrapFrame(
    PCONTEXT Context,
    PKEXCEPTION_FRAME ExeptionFrame,
    PKTRAP_FRAME TrapFrame,
    KPROCESSOR_MODE PreviousMode
);

VOID
STDCALL
KiDeliverApc(
    KPROCESSOR_MODE PreviousMode,
    PVOID Reserved,
    PKTRAP_FRAME TrapFrame
);

VOID
STDCALL
KiCheckForKernelApcDelivery(VOID);

LONG
STDCALL
KiInsertQueue(
    IN PKQUEUE Queue,
    IN PLIST_ENTRY Entry,
    BOOLEAN Head
);

ULONG
STDCALL
KeSetProcess(
    struct _KPROCESS* Process,
    KPRIORITY Increment
);

VOID
STDCALL
KeInitializeEventPair(PKEVENT_PAIR EventPair);

VOID
STDCALL
KiInitializeUserApc(
    IN PKEXCEPTION_FRAME Reserved,
    IN PKTRAP_FRAME TrapFrame,
    IN PKNORMAL_ROUTINE NormalRoutine,
    IN PVOID NormalContext,
    IN PVOID SystemArgument1,
    IN PVOID SystemArgument2
);

PLIST_ENTRY
STDCALL
KeFlushQueueApc(
    IN PKTHREAD Thread,
    IN KPROCESSOR_MODE PreviousMode
);

VOID
STDCALL
KiAttachProcess(
    struct _KTHREAD *Thread,
    struct _KPROCESS *Process,
    KIRQL ApcLock,
    struct _KAPC_STATE *SavedApcState
);

VOID
STDCALL
KiSwapProcess(
    struct _KPROCESS *NewProcess,
    struct _KPROCESS *OldProcess
);

BOOLEAN
STDCALL
KeTestAlertThread(IN KPROCESSOR_MODE AlertMode);

BOOLEAN
STDCALL
KeRemoveQueueApc(PKAPC Apc);

VOID
FASTCALL
KiWakeQueue(IN PKQUEUE Queue);

PLIST_ENTRY
STDCALL
KeRundownQueue(IN PKQUEUE Queue);

/* INITIALIZATION FUNCTIONS *************************************************/

VOID
NTAPI
KeInitExceptions(VOID);

VOID
NTAPI
KeInitInterrupts(VOID);

VOID
NTAPI
KeInitTimer(VOID);

VOID
NTAPI
KeInitDpc(struct _KPRCB* Prcb);

VOID
NTAPI
KeInitDispatcher(VOID);

VOID
NTAPI
KiInitializeSystemClock(VOID);

VOID
NTAPI
KiInitializeBugCheck(VOID);

VOID
NTAPI
Phase1Initialization(PVOID Context);

VOID
NTAPI
KeInit1(
    PCHAR CommandLine,
    PULONG LastKernelAddress
);

VOID
NTAPI
KeInit2(VOID);

BOOLEAN
NTAPI
KiDeliverUserApc(PKTRAP_FRAME TrapFrame);

VOID
STDCALL
KiMoveApcState(
    PKAPC_STATE OldState,
    PKAPC_STATE NewState
);

VOID
NTAPI
KiAddProfileEvent(
    KPROFILE_SOURCE Source,
    ULONG Pc
);

VOID
NTAPI
KiDispatchException(
    PEXCEPTION_RECORD ExceptionRecord,
    PKEXCEPTION_FRAME ExceptionFrame,
    PKTRAP_FRAME Tf,
    KPROCESSOR_MODE PreviousMode,
    BOOLEAN SearchFrames
);

VOID
NTAPI
KeTrapFrameToContext(
    IN PKTRAP_FRAME TrapFrame,
    IN PKEXCEPTION_FRAME ExceptionFrame,
    IN OUT PCONTEXT Context
);

VOID
NTAPI
KeApplicationProcessorInit(VOID);

VOID
NTAPI
KePrepareForApplicationProcessorInit(ULONG id);

ULONG
NTAPI
KiUserTrapHandler(
    PKTRAP_FRAME Tf,
    ULONG ExceptionNr,
    PVOID Cr2
);

VOID
STDCALL
KePushAndStackSwitchAndSysRet(
    ULONG Push,
    PVOID NewStack
);

VOID
STDCALL
KeStackSwitchAndRet(PVOID NewStack);

VOID
STDCALL
KeBugCheckWithTf(
    ULONG BugCheckCode,
    ULONG BugCheckParameter1,
    ULONG BugCheckParameter2,
    ULONG BugCheckParameter3,
    ULONG BugCheckParameter4,
    PKTRAP_FRAME Tf
);

VOID
STDCALL
KiDumpTrapFrame(
    PKTRAP_FRAME Tf,
    ULONG ExceptionNr,
    ULONG cr2
);

VOID
STDCALL
KeFlushCurrentTb(VOID);

VOID
STDCALL
KeRosDumpStackFrames(
    PULONG Frame,
    ULONG FrameCount
);

ULONG
STDCALL
KeRosGetStackFrames(
    PULONG Frames,
    ULONG FrameCount
);

VOID
NTAPI
KiSetSystemTime(PLARGE_INTEGER NewSystemTime);

NTSTATUS 
STDCALL
Ke386CallBios(
    UCHAR Int,
    PKV86M_REGISTERS Regs
);

ULONG
NTAPI
KeV86Exception(
    ULONG ExceptionNr,
    PKTRAP_FRAME Tf,
    ULONG address
);

#endif /* __NTOSKRNL_INCLUDE_INTERNAL_KE_H */
