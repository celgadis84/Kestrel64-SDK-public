# Threads and scheduling: decision

## Decision

1. **The core SDK stays single-threaded and polled.** A native game is one main loop:
   poll pad, run timers (`k64_timer_poll`), build the frame, queue RSP/RDP work, show.
   Interrupts stay masked. The parallelism of the N64 is the RCP (RSP, RDP, AI, PI DMA run
   on their own while the CPU works), and every SDK queue is already asynchronous: the CPU
   prepares frame N+1 while the RSP/RDP draw frame N (triple buffering), the AI plays one
   buffer while the next is mixed. A CPU thread switch buys nothing there and costs a full
   context save, locking in every subsystem and lost determinism.

2. **Threads are an opt-in module, `k64/thread.h`, with libultra semantics** — fixed
   priorities, a thread runs until it blocks or a higher-priority thread becomes ready; the
   only preemption points are interrupts and message-queue / priority operations. Message
   queues (`k64_mq_*`) carry RCP events (`k64_event_set`, the `osSetEventMesg` model).
   It exists for code written for libultra threads; a typical libultra game:

   - runs main (pri 10), audio (20), scheduler (30) and fault (40) threads, woken by VI
     retrace and SP/DP done messages; the audio thread must preempt a long main-thread frame
     every retrace or the AI underruns.
   - uses `osSetThreadPri(0, audio_pri + 1)` around shared audio state as a mutex:
     this only works with strict priority scheduling.
   - Cooperative tasks (switch only on explicit yield) would break the first point; a
     time-sliced scheduler would break the second. Hence libultra's model, exactly.

   With the module the shim is a rename: `osCreateThread` → `k64_thread_create`,
   `osRecvMesg` → `k64_mq_recv`, `osSetEventMesg` → `k64_event_set`, and so on.

3. **No RTOS beyond that**: no time slicing, no mutex objects (priority or a 1-slot message
   queue serve), no dynamic thread allocation (the caller owns thread structs and stacks).

## Rules when the module is on

- `k64_threads_init()` turns the caller into a thread and enables interrupts (RCP line +
  the Count/Compare timer line, each MI source only once an event is registered for it).
- A device whose event is registered belongs to the kernel's interrupt handler, which
  acknowledges it. Code that polls the same interrupt flag must not be used for it then
  (`k64_rdp_wait_idle` polls `MI_INTR_DP`: with a DP event registered, wait on the queue).
- SDK subsystems are not reentrant: each (display, RDP list, kgfx, audio, save, pad,
  timers, debug output) is used by one thread, or under a priority/queue lock.
- `k64_time()` is safe from any thread (it updates its wrap state with interrupts off).
- The context switch saves the full 64-bit GPRs, HI/LO and the 32 64-bit FPRs (n32,
  Status.FR=1). Measured cost: see demo 18_threads.
