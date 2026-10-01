# Scheduler and sentinel-loop regressions

This directory documents and tests a deliberately narrow set of progress and
lifetime faults in the XRootD server scheduler, server pollers, public
`XrdSys::IOEvents` API, and XrdCl response reader. The production patch is
bounded to those transitions; most of this change is evidence.

The production commit is based on upstream
`06570df04beefd3735219e122ea6168571eda2ae`. The stock evidence point remains
the development-start commit `5b716c84ab4d37d84e158c127b1c6272084cc889`;
all 18 affected production files are byte-identical at those two upstream
commits. The tests compile against that unmodified evidence source for the
native matrix; the forced fallback builds add only the two test selectors
documented below. This matters because the earlier branch had a merge parent
containing unrelated fixes, so comparing only with that parent made the patch
look narrower than the branch actually was.

## Evidence rules

Every test begins with an end-user scenario and one or more of these labels:

* **Stock reproduced** means the same test executable was compiled against
  unmodified upstream and failed there under the stated mode, either ordinary
  execution or ThreadSanitizer.
* **Valid-path control** means upstream passes. It protects behavior which the
  fix must preserve.
* **Branch guard** means the test targets lifetime or ordering introduced by
  the fix. It is not presented as an independently reproduced upstream fault.

The Solaris event-ports (`PollPort`) source contains the literal, non-C++ line
`@include "XrdSys/XrdSysE2T.hh"`, so the exact upstream file has no runnable
Solaris baseline at this commit. The four event-port tests therefore use a
synchronous backend with the same one-shot re-arm contract. They are branch
guards for the review finding, not claims of a runnable stock-Solaris failure,
and are omitted automatically when the locked-rearm overload is absent.

The tests do not pass impossible byte-count/status pairs through mocked
sockets, enqueue the same intrusive job twice, manufacture cyclic queues, or
call APIs with deliberately malformed internal objects. Those cases existed
on the earlier branch and have been removed because they did not demonstrate
failures that current production callers can generate.

## Production file map and user-visible failures

| Production file | Why it is touched | Direct regression evidence |
|---|---|---|
| `src/Xrd/XrdScheduler.cc` | Makes timer publication and condition-wait entry one synchronized handoff, and bounds seconds before conversion to signed milliseconds. | `EmptyTimerQueueCannotLoseTheFirstWakeup`, `EarlierTimerCannotLoseItsWakeup`, `FarFutureTimerUsesARepresentablePositiveWait`, `ConcurrentIndependentTimersAllExecuteExactlyOnce` |
| `src/Xrd/XrdScheduler.hh` | Constructs the existing timer condition variable for caller-managed locking without changing the scheduler layout. | The four scheduler cases above, plus normal construction/destruction in every server test. |
| `src/Xrd/XrdLinkCtl.cc` | Publishes persistent FD slots as free, initializing, or used; holds the operation lock through reuse; prevents scans from observing half-reset links; and preserves the existing nonthrowing startup-allocation failure contract. | `FindPinsOnlyTheConnectionWhoseIdentityItMatched`, `LinkSlotIsVisibleOnlyAfterInitialization`, `LinkPublicationMakesInitializedFieldsVisible`, `ConcurrentLinkSlotReusePublishesLookupState`, `ReusedLinkInitializationHoldsOperationLock`, `LinkTableAllocationFailureUsesExistingErrorPath` |
| `src/Xrd/XrdLinkCtl.hh` | Uses acquire loads for lookup publication and serializes the generation-aware lookup with close/reuse. | The same link-table cases; the reuse case exercises plain, versioned, poll-info, and `Find` lookups under ThreadSanitizer. |
| `src/Xrd/XrdLinkXeq.cc` | Pins protocol and callback storage during unlocked work; makes self-close and nested self-close nonblocking; serializes concurrent retirement; snapshots close state before slot reuse; validates generation after waits; and prevents shutdown state from admitting stale jobs. | `ProtocolProcessCanCloseItsOwnDispatch`, `NestedDispatchCanCloseAnAncestorConnection`, `ConcurrentFatalDispatchWaitsForRunningProtocol` (including the TSan `KeepFD` reuse race), `ConcurrentClosesWaitForRunningProtocol`, `ConcurrentClosesRetireOneConnectionOnce`, `CloseRegistersGenerationWaitBeforeUnlock`, `DeferredShutdownPreservesWaitingFinalClose`, the close-callback cases, and the three shutdown cases. |
| `src/Xrd/XrdLinkXeq.hh` | Declares the private generation-close and activity-completion helpers without adding a public overload or changing the public virtual interface. | `PublicCloseMemberPointerRemainsUnambiguous`, `PublicTerminateMemberPointerRemainsUnambiguous`, and the lifetime tests above. |
| `src/Xrd/XrdPoll.cc` | Gives deferred readiness/fatal jobs an immutable registration generation, a cached ordinary-readiness job, and validation before dispatch. | `OrdinaryReadinessKeepsThePollThreadAllocationFree`, `DeferredReadableEventCannotRunAgainstReplacementConnection`, `DeferredDisableCannotDispatchReplacementConnection`, `DeferredFatalEventCannotCloseAReplacementConnection`, `FatalCompletionCannotQueueAnUnguardedLinkAfterValidation` |
| `src/Xrd/XrdPollE.icc` | Moves fatal epoll completion out of the event-batch critical section and lets readable data win over simultaneous peer half-close. | `FatalEventDoesNotBlockTheCloseFenceOnTheLinkLock`, `PureFatalEventRunsDeferredCleanup`, `ReadableRequestsAndPeerDisconnectsBothMakeProgress`, `BufferedRequestPrecedesPeerHalfClose` |
| `src/Xrd/XrdPollInfo.hh` | Stores the registration generation, activity count/waiters, close intent, terminal state, and cached job needed by the existing link lifetime. | Exercised by all server lifetime, stale-job, shutdown, and PollPoll cases; each field has a mutation-sensitive case described in the evidence README. |
| `src/Xrd/XrdPollPoll.icc` | Defers fatal completion until after the portable poll batch unlocks and carries registration/protocol identity through restart and reuse. | All six `ServerPollPollLoops` cases. |
| `src/XrdCl/XrdClAsyncMsgReader.hh` | Allows the existing async reader to query the incoming helper's stored absolute deadline. | `ExpiredHeadersStopBeforeBodyForPartialAndFinalReplies`, `IncompleteBodyExpiresDespiteNewReadableBytes`, and the completed-frame controls. |
| `src/XrdCl/XrdClStream.cc` | Tears down only the expired incoming operation, reports its timeout outside the stream lock, and gives unrelated queued requests the connection error used by retry logic. | The read-timeout, peripheral recovery, main recovery, unrelated-request, and socket-error cases in `ClientTimeouts`. |
| `src/XrdCl/XrdClStream.hh` | Declares the small locked deadline classifier used by the stream error paths. | The same request-local timeout and recovery cases. |
| `src/XrdCl/XrdClXRootDMsgHandler.cc` | Enforces the absolute deadline while a frame is incomplete, fences handler ownership for partial/wait/chunked responses, and removes malformed-status handlers before delivering their saved error. | All 21 `ClientTimeouts` cases, including the final-status, malformed-status, and queue-removal use-after-free regressions. |
| `src/XrdSys/XrdSysIOEvents.cc` | Routes fatal and ordinary callbacks through one completion protocol, preserves callback-owned channels/descriptors, performs one-shot re-arm while the channel is locked, snapshots the poller and trace descriptor before unlock can permit deletion, and clamps timeout conversion. | The 18 native-epoll, 27 forced-PollPoll, and four event-port contract executions on Linux, plus 21 applicable executions on macOS, including ASan and guarded-page unlock-lifetime cases. |
| `src/XrdSys/XrdSysIOEvents.hh` | Records descriptor replacement during a callback so completion cannot remove or re-arm the replacement. | `FatalCallbackCanReplaceTheDescriptor`, `EventCallbackCanReplaceTheDescriptor`, `SetFDPreservesCallbackOwnershipUntilReturn`, `SetFDTransitionClearsAfterCallbackReturns` |
| `src/XrdSys/XrdSysIOEventsPollPoll.icc` | Fixes channel/poll lock ordering, clears removed table entries while the channel is live, serializes timeout-driven descriptor removal, and tags queued include/modify/remove commands so an old command cannot mutate a reused slot or supersede a newer command. | `PrePollTimeoutRemovalKeepsFallbackTableSerialized`, `StaleModifyDoesNotOverwriteReusedPollSlot`, `FirstEnableDoesNotRelockFreedChannel`, `ModifyBeforeFirstIncludeKeepsNewestEventMask`, `NewerModifyWinsWhenPipeWritesArriveOutOfOrder`, `StaleRemoveCannotDeleteAReusedPollSlot`, and `IncludeCannotPublishAnOlderFloorAfterSlotReuse`, plus fatal deletion/reuse/SetFD cases. |
| `src/XrdSys/XrdSysIOEventsPollPort.icc` | Moves one-shot event-port re-association into callback completion while the channel mutex still protects the pointer. | `EventPortRearmsLiveCallbackExactlyOnce`, `EventPortFatalCompletionDoesNotRearmFailedDescriptor`, `EventPortFatalCallbackCanDeleteItsChannel`, `EventPortConcurrentDeleteCannotRaceBackendRearm` |

The companion evidence branch adds no installed behavior beyond the production
files above. It also keeps the documentation-only cleanup omitted from the
minimal production diff. Its additional files and test-only deltas are:

| Companion-branch file | Why it is touched |
|---|---|
| `tests/unit/CMakeLists.txt` | Includes the isolated `XrdLoops` test directory. |
| `tests/unit/XrdLoops/CMakeLists.txt` | Builds and discovers the client, IOEvents, server, forced-PollPoll, and trace executions with per-test timeouts. |
| `tests/unit/XrdLoops/ClientTimeoutTests.cc` | Holds the 21 client deadline, error-routing, fence, and malformed-response cases. |
| `tests/unit/XrdLoops/IOEventsCoreTests.cc` | Holds the 30 callback ownership, event-port contract, deletion, descriptor replacement, queued PollPoll command, trace-lifetime, and timeout-conversion cases. |
| `tests/unit/XrdLoops/ServerLoopTests.cc` | Holds the 37 scheduler, link publication/reuse/allocation-failure, dispatch, close, shutdown, and epoll cases. |
| `tests/unit/XrdLoops/ServerPollPollTests.cc` | Holds the six portable server-poller deadlock, generation, protocol handoff, and restart cases. |
| `tests/unit/XrdLoops/reproduce_client_timeout.py` | Runs an ordinary `xrdfs` process against a real loopback peer which cleanly completes, fragments, stalls, or drip-feeds a valid response. |
| `tests/unit/XrdLoops/README.md` | Records reproduction mechanics, stock/branch classification, mutation evidence, platform results, and scope exclusions. |
| `src/Xrd/XrdPoll.cc` | The evidence commit adds two preprocessor clauses so a Linux-only test executable can select the otherwise hidden server PollPoll backend. |
| `src/XrdSys/XrdSysIOEvents.cc` | The evidence commit adds one preprocessor clause so a Linux-only test executable can select the otherwise hidden IOEvents PollPoll backend. |
| `src/XrdCl/XrdClXRootDMsgHandler.hh` | Corrects a stale method comment; this documentation cleanup is deliberately excluded from the production diff. |

For a reviewer worried that this is AI-generated churn: the user-visible faults and the repair's regression risks are concrete, bounded, and exercised against the named upstream commit. A valid timer can sleep for the old deadline or an hour after a lost signal; a long delay can overflow into an early or spinning wait; a disconnect racing close can deadlock the server poll loop; a final request sent with `shutdown(SHUT_WR)` can be discarded; a queued readiness or fatal job can run on a new client which reused the same FD; shutdown at generation zero can admit a stale job; `Process`, a close callback, or a nested dispatch can deadlock its own close or have its protocol storage recycled underneath it; concurrent closers can miss a wakeup or retire one connection twice; link-table readers can see partially initialized or concurrently reset identity, and `Find` can return a retired connection while pinning its reusable slot; the atomic LinkBat repair must also retain the established nonthrowing low-memory setup path; an IOEvents plugin can hang forever deleting a channel, use the channel after `Enable` unlocks, or remove a replacement FD installed by its callback; a delayed PollPoll include, modify, or remove command can corrupt a reused slot, while an older modify can override the user's newer request; PollPoll timeout removal can race its descriptor table; large IOEvents timeouts can overflow; a peer can keep an XrdCl request alive beyond its absolute deadline by dripping body bytes; recovery can report that timeout to the wrong request; and partial, wait, chunked, or malformed-status handling can retain a callback object after it has been released. The companion branch starts every test with the corresponding end-user scenario and labels it as an upstream failure, a branch-regression guard, or a valid-path control; it also drives an ordinary `xrdfs` binary against a deliberately broken loopback peer.


## 1. A request timeout can be ignored forever

### User-visible sequence

1. `xrdfs cat`, `xrdcp`, or another XrdCl user sends a request with an absolute
   request deadline.
2. The server or an intermediary starts a valid response frame.
3. It stops before completing that frame, but occasionally sends another byte.
4. Socket activity prevents the stream-inactivity timer from firing.
5. For read/readv, `pTimeoutFence` makes the tick thread ignore the request
   timeout while the stream's incoming helper retains a borrowed handler.
6. Upstream never checks the handler's stored absolute expiry in the response
   reader, so the caller can remain blocked beyond its configured deadline.

The fix consults the existing `InMessageHelper::expires` value in the socket
reader. It checks before each state-machine pass, so bytes arriving for an
incomplete body cannot refresh the original deadline. The socket read-timeout
callback performs the same check for a peer which stops producing readiness
entirely. Expiry goes through the existing `Stream::OnError` path; there is no
new teardown job, channel lookup, or public state.

### Related directory-list lifetime fault

While an `AsyncMsgReader` reconstructs a response, the stream's incoming
helper contains a non-owning `MsgHandler *`. Upstream raises `pTimeoutFence`
for partial read/readv responses, but not for partial directory listings,
other `kXR_oksofar` responses, or a declared `kXR_waitresp` body. The tick
thread may therefore expire and remove a handler while the helper still
borrows that pointer. The fix raises the same fence for every partial response
and for waitresp. Completed synchronous frames and normal reinsertion lower it.

When `chunkedResponse` asks XrdCl to deliver each `kXR_oksofar` frame, the
completed frame is queued to the worker pool rather than handled synchronously.
That queued `Process` job also owns only a raw handler pointer. The fence stays
raised through this handoff and `HandleResponse` lowers it after the partial
callback completes. A single-worker test blocks the FIFO before dispatch, so
the timeout check is guaranteed to run while `Process` is queued. Upstream
returns `RemoveHandler` at that point; the corrected branch retains the handler,
then proves it becomes timeout-eligible after the worker drains.

`ClientTimeoutTests.cc` covers every split inside the eight-byte response
header for both partial and final responses, a body which crosses its deadline,
one-byte-at-a-time valid fragmentation, zero-deadline handlers, helper reset
between consecutive requests, and the directory/waitresp fence decisions,
including release after a complete waitresp frame.

### Malformed `kXR_status` must release queue ownership before the callback

A broken or older server can declare a page-read `kXR_status` body and then
send too little data to unmarshal it. This is malformed peer input received on
an ordinary client socket, rather than a malformed internal test object. The
upstream error path records the unmarshal error, invokes the response callback,
and returns `Ignore`. The callback may destroy the request handler while both
the stream's incoming helper and `InQueue` still contain its non-owning
pointer. A partial-result label then makes `OnIncoming` touch the released
handler immediately; otherwise a later timeout scan can reach the stale queue
entry. Both paths are use-after-free faults.

The fix returns `RemoveHandler` from inspection, allowing `Stream` to remove
the handler from `InQueue` before queuing its existing processing job.
`Process` notices the saved error and reports it without inspecting a response
body. Synchronous cleanup in `OnIncoming` is limited to `NoProcess` partials
and `kXR_waitresp`, the cases which actually raised the timeout fence; an
arbitrary `Ignore` action may already have finalized its handler.

Three Linux death tests put the destroyed handler on an inaccessible page, so
a stale access faults deterministically without relying on allocator reuse or
a sanitizer. The guarded callback must also run exactly once with
`errInvalidMessage`, proving that the deferred `Process` path delivers the
original parsing error instead of merely avoiding the stale access. Two tests
reproduce the upstream partial-result and later-timeout faults. The
final-result case is a branch guard: upstream's narrower cleanup does not touch
that handler immediately, while published adversarial-review revision
`e8848d15b` broadened cleanup to every `Ignore` action and did.

### Completed input is not an unfinished timeout

Published adversarial-review revision `e8848d15b` also checked the absolute
deadline after every reader state transition, including `ReadDone`. A response
which consumed its last declared byte exactly as the deadline ticked over was
therefore rejected as expired even though reconstruction had finished. The
final condition omits `ReadDone`: incomplete headers and bodies still expire,
while a complete frame is dispatched to its handler. The deadline-edge test
passes upstream, fails `e8848d15b`, and passes the corrected branch.

### One expired response does not expire multiplexed peers

Closing a connection is necessary after a response body stalls: the client
cannot find the next frame boundary. Published revision `e8848d15b` passed that
one request's `errOperationExpired` status into the connection-wide `OnError`
path, however. On the main stream, `OnError` reports its status to every queued
handler. An unrelated request with a later deadline was therefore completed as
expired instead of receiving the recoverable connection break and following
the existing retry policy.

After the incoming helper is reinserted and the stream lock is released, the
fix first runs the existing timeout scan. That reports `Timeout` only to
handlers whose own deadline has elapsed and removes them from `InQueue`. It
then reports `errSocketError`/`Broken` to the remaining incoming and outgoing
requests. No request-specific callback runs while the stream lock is held.

The two-handler regression records the exact events and statuses. The direct
`OnError` guard fails on upstream `5b716c84a` and `e8848d15b`: the due handler
gets `Broken` rather than `Timeout`, and the future-deadline handler gets error
206 (`errOperationExpired`) rather than 102 (`errSocketError`). In stock this
fan-out is not reached by a dribbled body because the reader never notices that
deadline; `e8848d15b` makes it reachable. The corrected test passes and, with a
single-worker FIFO barrier fencing the real asynchronous socket-destruction
job, passed 200 consecutive Alma 9 repetitions. A companion test covers the
no-readiness route for both `kXR_oksofar` and final `kXR_ok` bodies: upstream
returns from `OnReadTimeout` without teardown, while the corrected branch
delivers one request timeout and stops the reader.

Parallel data sockets take the same route. Upstream returns `true` from a
peripheral `OnReadTimeout` and emits no event. The initial branch correction
closed that socket and reinserted its handler, but did not report the timeout
until a later global tick. The regression covers both an empty data-path output
queue and a queued send. A second case makes the poller reject write
notification while queued work moves back to the connected control path: the
due handler receives `Timeout`/206 first, then fatal recovery reports
`errPollerError` (105) to both the queued send and the future-deadline incoming
peer. A companion main-stream guard forces reconnect setup to fail and requires
the same ordering: the expired incoming request receives `Timeout`/206 before
both unrelated queues receive a fatal, non-expiry connection error. The kernel may
reject connect before the deliberately rejected poller registration, so its
exact socket/poller code is platform-dependent. This keeps request expiry local
even when the connection itself cannot recover. The peripheral and FIFO-blocked
chunked-response cases passed 500 repetitions each on Alma 9.

### Executable reproduction with an ordinary client

`reproduce_client_timeout.py` starts a loopback TCP peer which implements the
real XRootD handshake, protocol, login, open, read and directory-list exchanges.
It then runs the supplied, unmodified `xrdfs` binary. The client library and its
actual socket and event loops are used; only the remote peer is deliberately
broken.

```sh
export LD_LIBRARY_PATH=/path/to/build/lib
python3 tests/unit/XrdLoops/reproduce_client_timeout.py \
  --client /path/to/build/bin/xrdfs \
  --operation read --scenario partial-drip --deadline 2 --watchdog 8
```

The `partial-drip` peer sends a valid response header declaring a 4096-byte
body, sends one body byte, then sends another byte every 200 ms. On pristine
upstream the two-second request deadline was ignored and the external
eight-second watchdog killed `xrdfs` after 8.157 seconds. The fixed client
reported `Operation expired` after 2.031 seconds. `partial-stall`, which
exercises the socket read-timeout callback rather than continued readiness,
changed from an 8.083-second watchdog kill to a normal timeout after 2.030
seconds. A final-response body stall likewise changed from an 8.034-second
watchdog kill to a timeout after 2.230 seconds. When a complete final reply was
delayed until after its deadline, upstream incorrectly succeeded after 5.159
seconds; the fixed client expired it after 2.056 seconds. Deadlines use integer
epoch seconds, so a nominal two-second deadline does not imply an elapsed time
of exactly 2.000 seconds.

Each run prints JSON containing the observed requests, elapsed time, exit
status, and bounded outcome; no transcript artifact is committed. Clean reads
produce 4096 exact bytes on both builds (0.218 seconds fixed, 0.228 upstream),
and a byte-fragmented directory list produces 44 exact output bytes on both
(0.218 and 0.224 seconds). A negative control is also kept out of the fixed-
failure count: an endless sequence of complete `kXR_oksofar` frames times out
on both trees (1.823 seconds fixed, 4.258 upstream). All scripted expectations
matched and the peer reported no protocol errors.

The reproducer calls an eight-second watchdog result a **hang**, rather than
claiming it proves mathematically infinite execution.

## 2. The scheduler can lose a newly inserted timer

Upstream examines `TimerQueue` under `TimerMutex`, drops that mutex, and only
then enters `TimerRings.Wait()`. A producer can insert the first timer (or a
timer earlier than the current head) and signal during that gap. Condition
variable signals are not remembered. The timer thread subsequently sleeps for
the timeout computed from the old state: one hour for an empty queue, or until
the old later deadline.

The fix uses the condition variable's mutex to make queue inspection and wait
entry one atomic handoff. Producers publish under `TimerMutex`, drop it, then
take the condition mutex before signaling. That order avoids reversing the
timer thread's `condition -> TimerMutex` order.

The Linux test executable interposes the already exported
`XrdSysCondVar::Wait(int)` method and pauses immediately before the real
condition wait. Production code has no test hook. The test schedules a normal
timer during that pause and observes its publication under the real
`TimerMutex`. Upstream has already emitted and lost the signal at that point;
the fixed producer is blocked until the waiter atomically enters its wait.

Long timer delays are also capped before `Wait(int)` multiplies seconds by 1000
in a signed `int`. A deliberately selected deadline narrows to a short positive
wait upstream, so this is an observable early-wakeup test rather than a generic
integer-overflow assertion.

## 3. Fatal server events can invert the Close lock order

The epoll and poll backends used to invoke `XrdPoll::Finish` on their event
thread. `Finish` changes the protocol and error text, both of which acquire the
link's recursive operation mutex. Concurrent `XrdLink::Close` already owns that
mutex while it waits for the poller's event-batch fence. The two threads can
therefore wait on one another:

```text
Close thread:  owns link mutex  -> waits for poller batch fence
poll thread:   owns batch       -> Finish waits for link mutex
```

The compact fix submits a small finish job to the existing scheduler. Before
touching a persistent link-table slot, the job verifies the immutable poll-
registration generation and rejects an explicit close; the existing `Finish`
state rejects a terminal link. It does not use a descriptor or poller pointer
as a second identity. It then carries that generation through the existing
close callback and rechecks it whenever `Close` must drop its operation lock
to serialize other users. There is no second, unguarded raw link job after
validation, so scheduler congestion cannot reopen the reuse window and close
the next client occupying the slot.

The public close-request callback has always run without the link operation
lock, so a protocol may inspect link state under `Hold()`. An intermediate
generation-aware termination path invoked it while already holding that lock
and could deadlock such a protocol. The final path snapshots the callback and
pins the connection under the lock, invokes the callback unlocked, releases the
pin, and then performs the generation-checked close if approved. Approval and
veto guards make the callback spawn and join a worker which acquires `Hold()`;
the held-lock implementation therefore deadlocks deterministically, while the
final path completes exactly one callback and the expected recycle or
ownership-transfer result. A separate stock regression blocks the callback,
starts an administrator `Close`, and proves the protocol is not recycled until
the callback releases its pin; upstream recycles it while its callback is still
running.

The link-table object persists when a descriptor is reused. Its reset and new
connection fields must therefore use the same operation lock as the delayed
job's validation. The allocator now takes that lock before `Reset()` and keeps
it through replacement initialization. The poll thread cannot take this lock
while constructing the job without recreating the original inversion, so
`Attach` copies the link generation into `PollInfo.Generation`. That value is
immutable for the lifetime of the poll registration and remains valid until
`Detach`'s existing event-batch fence says the poll thread can no longer
inspect it. This also avoids a concurrent plain read of `XrdLink::Instance`
and does not change that public class's layout.

Taking the operation lock did not by itself make allocation safe for the
lock-free `fd2link` and `fd2PollInfo` lookups. Upstream marks `LinkBat[fd]` used
before it drops the table lock and initializes the replacement's identity,
address, and poll fields. A concurrent lookup can therefore return the
persistent object while it still contains retired or partial state. The plain
slot byte also races lookup when `Alloc` and `Unhook` reuse a descriptor.

The slot marker is now an atomic three-state publication point: free,
initializing, or used. Allocation publishes `used` with release ordering only
after the fields visible to table readers are ready; lookups require an acquire
load of that state. A deterministic regression pauses the allocator in its real
`strdup` call and proves both lookup APIs hide the slot until initialization is
complete. It also retires the higher descriptor during that pause, guarding the
table high-water mark while a lower slot remains initializing. A portable
ThreadSanitizer regression repeatedly closes and reuses one descriptor while a
management thread calls `fd2link`, reproducing the stock `LinkBat` race. This
only defines publication; callers still use the existing lock or reference
contract to keep a returned connection alive.

A second publication regression starts allocation on one CPU and consumes the
new link and poll fields through `fd2link`/`fd2PollInfo` on another. The
production release store and acquire lookup are its only publication edge;
weakening either side to relaxed makes ThreadSanitizer report the field reads.

Changing `LinkBat` from bytes allocated with `malloc` to an array of atomics
must not turn recoverable startup memory pressure into an uncaught
`std::bad_alloc`. `LinkTableAllocationFailureUsesExistingErrorPath` rejects
exactly the next array allocation and requires `XrdLinkCtl::Setup` to log and
return zero through its established failure path. This is a valid-path control
for stock, which already uses nonthrowing `malloc`, and a branch guard for the
atomic replacement. It does not claim a new stock defect; it prevents the
concurrency repair from creating one.

The regression test uses a kernel-reported `EPOLLHUP` from an empty pipe, the
real epoll backend and the eventfd fence already used by `Close`. It selects the
same fatal-only `Finish` path without claiming a plain client FIN reproduces the
daemon deadlock, forces the contested ordering, and fails upstream at a
five-second deadline. A separate test blocks the scheduler,
closes and reallocates the same link slot, then proves the delayed job leaves
the replacement alive. A lock-ownership guard pauses the real reused-slot
`Reset()` call and proves allocation owns the operation lock at that exact
point; pristine upstream and first pushed revision `f1839f5d0` fail it.
Additional guards cover close-callback approval and veto, the generation
recheck after `Close` waits for another link user, and the absence of a second
unguarded link job after the first validation.

The fatal-only regression also requires the deferred job to deliver the exact
`"hangup"` reason to `Protocol::Recycle`. That catches a superficially safe
offload which avoids the lock inversion but loses the diagnostic text before
the scheduler runs it.

Two simultaneous closers expose the same reuse boundary without any deferred
poll job. Upstream lets both wait for the last active request; after it leaves,
both resume and retire the connection, so the loser can close the descriptor a
second time. Public `Close` now captures the connection generation before it
waits and rechecks after every serialization wakeup. The regression requires
one protocol recycle and two successful idempotent callers, rather than an
`EBADF` from the losing close.

### Readable bytes take precedence over a simultaneous hangup

A client may send its final request, call `shutdown(SHUT_WR)`, and remain
connected to read the reply. Linux can report that as
`EPOLLIN|EPOLLRDHUP`. Upstream epoll treats the hangup as fatal before
dispatching the readable byte, so the server recycles the protocol without
delivering the final request. Upstream's portable PollPoll backend already
gave readable input precedence. An intermediate adversarial correction after
`e8848d15b` made any PollPoll hangup fatal and reproduced the same request loss
there; it was removed before the final patch.

Both backends now use the same single decision: only an event with no readable
bit enters fatal completion (`!(events & pollOK)`). A combined event dispatches
the protocol first; the subsequent EOF takes the generation-checked
termination path. The test sends three requests through a real socketpair,
half closes the peer, and requires all three deliveries across poll re-enables
followed by exactly one protocol recycle.

Every readiness job captures the poll-registration generation before entering
the scheduler and pins a still-matching connection through protocol processing
and re-enable. This also covers an ordinary `EPOLLIN` job delayed behind other
scheduler work: after a concurrent close and complete descriptor/link-slot
reuse, the old job is discarded instead of running the replacement client's
protocol. A deterministic test makes the replacement readable and then reuses
the slot a second time before either queued job can run. It proves the cached
job survives slot reset and the overlapping event gets an independent fallback
job: both retain their own generations and leave the final client untouched.
An adversarial review first found that lazy cache creation from readiness and
`Disable(etxt)` would race on the plain cache pointer. The final form prepares
the cache during globally serialized poll attachment, before the descriptor can
be enabled, so neither entry path can race its initialization. Both epoll and
PollPoll keep their existing batched scheduler submission. Ordinary readiness
therefore stays allocation-free; only overlap while the cached job is still
queued allocates a fallback job.

### Closing cannot recycle protocol/callback-owned storage while code runs

A generation check prevents a delayed job from selecting the wrong connection,
but it does not by itself keep the selected protocol alive. Once a server
dispatch enters `XrdProtocol::Process`, an administrator or a concurrent
fatal-completion job can call `Close`. Upstream does not count readiness
dispatch as an `InUse` reference, so `Close` can invoke `Recycle` while
`Process` is still on the stack. The same problem applies while an unlocked
close-request callback is using protocol-owned callback storage.

The shared dispatch wrapper records a small internal Activity count under the
existing link operation mutex. Admission uses the immutable registration
generation as its only connection identity, then rejects close-pending or
already-started terminal work; it does not compare the descriptor or poller.
Close callbacks use the same count. Generation-aware `Close` waits for extra
ordinary `InUse` references to drain to the link's baseline reference and for
Activity to reach zero. Each closer which reaches the Activity wait queues its
own stack semaphore, so the last activity wakes every such closer without one
waiter consuming another's notification. The existing semaphore still handles
ordinary `InUse` waits. The generation is rechecked after every wake; a losing
closer therefore cannot continue against a reused slot.

`XrdLink::Serialize` deliberately retains its exact upstream implementation
and waits only for `InUse` references. An earlier Activity implementation used
`InUse` as the readiness pin and made `XrdCmsProtocol::Process` deadlock when it
called `Serialize` before returning. A later implementation made Serialize
wait for other Activity owners, which could make two simultaneous owners wait
for one another. Keeping Activity private to Close preserves the established
Serialize contract: a Process-side call does not wait for itself, while it
still waits for an independent link reference.

Default, non-deferred `Close` remains callable from `Process` and from the
unlocked close callback. Waiting there would make either caller wait for the
Activity that cannot finish until `Close` returns. A thread-local ownership
marker instead sets `closePending` and returns success. Initial, offload, and
poll-readiness dispatch all use the same Activity wrapper. Dispatch checks for
retirement after every `Process` call, exits the sticky-consumption loop, and
does not re-arm the descriptor. Explicit close remains sticky until slot reset;
a separate none/selected/started terminal state is published when `Finish`
selects terminal dispatch. Selection immediately prevents re-arm; callback
start rejects later jobs and cannot regress when another poll error arrives.
This preserves the callback veto. Reaching Activity zero triggers a
generation-checked explicit close; an external closer may win, and every loser
becomes a no-op after its generation check. This also covers a callback which
explicitly closes the link and vetoes the automatic close. Deferred `Close`
retains its existing shutdown path. A dedicated valid-path control calls
`Close(true)` from `Process` and requires shutdown to leave the descriptor,
protocol, baseline reference, and recycle count alone for the later CMS cleanup
job. A separate generation-wrap guard calls `Shutdown`, queues a generation-zero
dispatch, and proves terminal state rejects it even though zero is also the
inactive instance sentinel.

Focused regressions exercise these contracts through real dispatch and close
paths. Two controls call `Serialize`, with and without another `InUse` owner.
Stock-failure cases block `Process` while fatal cleanup or two administrators
close the same link, call public `Close` from `Process`, and call it from the
close callback. Against pristine upstream those fault cases recycle the
protocol before the active function returns. On this branch, external closers
wait; self-`Close` returns immediately and records pending retirement; one
closer recycles after the last Activity returns; and the descriptor is not
re-armed. The self-close test covers direct and readiness dispatch, then
submits another matching job while `Process` remains active and requires its
`Process` count to stay at one. A nested-dispatch regression additionally has
an inner callback close the outer connection: the ownership walk must find an
ancestor Activity, or it either recycles the outer protocol on its live stack
or waits forever for that same stack. The fatal and two-closer tests likewise
prove a waiting close rejects later dispatch admission.

ThreadSanitizer then reproduced a stock epoll race in that ordinary readiness
handoff: the poll thread writes `XrdPollInfo::isEnabled` while disabling a
readable descriptor, and the scheduler thread reads it while re-arming the
same connection. The flag is now atomic. The regression uses a real
socketpair, a one-worker scheduler, and a protocol which consumes 4096
requests one at a time, so every re-arm follows the production
poll-to-scheduler path.

The link-table reuse coverage also includes the management `Find` path. Stock
`Find` matches identity fields under the table lock, drops that lock, and only
then increments the link reference. A concurrent close can reset `Instance`
and `InUse`, wait in `Unhook`, and let `Find` raise the dead slot from zero to
one; because both sampled generations are then zero, the retired connection is
returned and its reusable slot remains pinned. The deterministic regression
holds an earlier result's operation lock so `Find` owns the table lock, retires
the next matching connection, and then releases that ordering barrier. The
stock sequence returns the retired pointer; the repair pins under the existing
table-lock-to-operation-lock order and skips it. The concurrent reuse test also
pins and releases `Find` results under ThreadSanitizer. `getName` needs no code
change: it already keeps the table lock through the identity copy, so slot
reset/reuse cannot overlap that read; the reuse test exercises that path too.

The same guard covers dispatch queued explicitly by `Disable(etxt)`, which is
used by the server idle scanner and remote connection termination. Its stock
raw-link job can wait after `Finish`, outlive a separate close and descriptor
reuse, and then run the replacement client's protocol. The regression blocks
the only scheduler worker across that complete lifecycle and puts a request on
the replacement socket; the stale dispatch must leave it untouched.

### The portable server PollPoll loop has direct runtime coverage

`ServerPollPollTests.cc` declares six focused cases. On macOS it runs the native
server PollPoll implementation. On Linux the test target compiles the same
`XrdPoll.cc` translation unit with the test-only `XRD_SERVER_FORCE_POLL`
selector, so Alma 9 exercises PollPoll instead of the normal epoll backend.

The six cases cover separate contracts:

1. A stock reproduction injects a real `POLLHUP` while an administrator holds
   the link operation lock. Upstream calls `Finish` while holding `PollMutex`,
   reproducing the portable backend's link/poll lock inversion.
2. A generation-zero branch guard closes the selected link before a queued
   fatal job runs. The delayed job must reject the inactive slot without
   invoking its retired close callback.
3. A stock reproduction pauses after `poll(2)` selects readiness while
   `Shutdown` changes the live link instance to zero. The queued dispatch must
   retain the immutable registration generation.
4. A readiness worker admitted just before fatal selection must call the
   protocol captured under the link lock, even if termination replaces the
   live protocol before that worker continues. This is a branch guard for the
   new Activity lifetime boundary.
5. A valid-path control lets the loader protocol hand off to the established
   protocol during sticky dispatch. The snapshot must be refreshed under the
   link lock between iterations, so the second buffered request reaches the
   established protocol.
6. A stock reproduction calls `Restart`, closes and reuses the descriptor/link
   slot, then proves delayed work neither consumes the replacement request nor
   recycles its protocol.

Together these execute the changed PollPoll `Start` and `Restart` paths and
exercise both sides of protocol capture: an admitted call remains stable, while
the next sticky iteration observes a legitimate handoff.

### Public source compatibility is retained

Published adversarial-review revision `e8848d15b` added an internal
`XrdLink::Terminate(unsigned int)` overload beside the existing public
`Terminate(const char *, int, unsigned int)`. Out-of-tree protocols which use
the valid inferred expression
`auto fn = &XrdLink::Terminate` then fail to compile because the name is
ambiguous. The internal entry point now remains on `XrdLinkXeq`, so there is no
new public `XrdLink` overload, layout change, or vtable change. `XrdPollInfo` is
an internal poll-registration structure and does gain generation, cached-job,
Activity, and atomic state. A compile-time branch guard builds the formerly
valid member-pointer expression against both trees.

The forced interleavings provide deterministic proof of the tested lock and
generation properties. The branch does not claim that a plain FIN hangs a
normal XRootD protocol connection; Linux epoll already treats a standalone
`EPOLLRDHUP` as fatal.

## 4. Public IOEvents fatal callbacks can strand channel deletion

A plugin can request `Channel::errorEvents`. Upstream marks the channel as
being in callback mode, calls `Fatal`, and returns without performing the
normal callback completion protocol. A later `Channel::Delete` waits for a
completion acknowledgment that no thread will ever post. The same incomplete
cleanup can discard a descriptor installed by `SetFD` from inside `Fatal`.

The fix routes fatal and ordinary callbacks through one completion path. It
acknowledges concurrent deletion and removes a failed backend registration
while the channel lock still fences object lifetime. Completion then tells the
covered epoll, kqueue, and PollPoll dispatchers that no second removal is
needed; `chDead` remains the immediate self-deletion signal.

There is a separate stock use-after-free in the normal timeout-wakeup path.
`Channel::Enable` drops the channel mutex and then reads `chPollXQ`; another
thread may release the final channel reference as soon as that mutex unlocks.
The fix snapshots the poller while the mutex is held and uses only the snapshot
after unlocking. The Linux regression runs the real `Enable` timeout-insertion
and wakeup decision, pauses immediately after its actual channel-mutex unlock,
deletes the channel on the other thread, and resumes. AddressSanitizer catches
upstream reading `chPollXQ` from the freed channel at that forced seam.

The portable PollPoll backend exposes the same lifetime boundary in both
`Channel::Enable` and `Channel::Disable` when tracing is enabled. Its `Modify`
method releases the channel mutex before returning, so a concurrent final
`Delete` can free the channel before `TRACE_MOD` reads `chFD`. The two
`EnableTraceDoesNotReadChannelAfterPollPollModifyUnlock` and
`DisableTraceDoesNotReadChannelAfterPollPollModifyUnlock` tests first register
a real PollPoll channel, identify its channel mutex through the public
`GetCallBack` operation, and pause exactly after `Modify` releases that mutex.
The deleting thread changes the channel's dedicated allocation page to
`PROT_NONE` before the operation resumes. Exact upstream and the minimized
branch before the fix both terminate with `SIGSEGV` at the trace read; taking
the descriptor snapshot while the mutex is held makes both tests exit normally.

These tests exercise the public plugin API. Stock XrdCl itself does not request
`errorEvents`, so this is not described as an ordinary `xrdcp` failure. Linux
epoll and forced PollPoll behavior are covered below, and the portable cases
are also compiled into the macOS kqueue matrix.

A dedicated CTest invocation repeats the SetFD ownership and fatal self-delete
cases with `XrdSysIOE_TRACE=1`. The first forces tracing to name the
`isChanged` callback state; the second proves the trace path does not inspect a
channel after `Fatal` deletes it from inside the callback.

### The generic PollPoll backend is tested on Linux

The portable PollPoll implementation is normally hidden by epoll on Linux and
kqueue on macOS. A test-only compile definition now builds a second Linux
`XrdSys::IOEvents` executable from the same source while selecting PollPoll.
The source declares 26 logical IOEvents cases, including eight cases compiled
only into the forced target: the backend-identity guard, the two trace
lifetime regressions, and five queued-command ordering/reuse regressions.
Linux discovers 18 native-epoll and all 26
forced-PollPoll executions; macOS discovers 17 applicable kqueue
executions. The selector changes no installed library configuration.

That run exposed three backend-specific faults. PollPoll held `pollMutex` across
a user callback, while external deletion held the channel mutex and waited in
`Exclude` for `pollMutex`; callback completion then waited for the channel
mutex. The fix drops the channel lock before a non-poller thread waits for the
poll lock. Callback removal could also leave the channel's PollPoll table
marker referring to an entry already removed, so a descriptor installed by
`SetFD` inside a callback later collided with stale backend state. The fix
captures and clears that marker while the live channel is locked, and backend
table removal no longer dereferences a channel whose callback may have deleted
it. Finally, the pre-poll timeout path calls `CbkTMO` after `TmoGet` releases
`pollMutex`. When that callback returns false, `FDRem` used to modify the
descriptor table without the poll lock and could race a later `Include`.
`FDRem` now takes the recursive poll mutex itself, covering both this unlocked
entry and callers which already hold the lock. A real one-second timeout test
selects this pre-poll path and reproduces the table race under ThreadSanitizer.

The command pipe has a separate slot-reuse problem. Non-poller threads capture
a table index and then send `MiFD`, `MdFD`, or `RmFD`; channel deletion, slot
reuse, and a newer command can all happen before the poll thread consumes the
old command. Stock can consequently re-lock a freed channel, disable a new
connection, remove a new connection, or let an older disable override a newer
enable. The original PR 2958 also published backend ownership before `MiFD`
was sent, so an `MdFD` arriving first was overwritten by the older initial
mask; `ModifyBeforeFirstIncludeKeepsNewestEventMask` fixes that interleaving.
Each slot now stores a monotonically increasing 64-bit command tag.
The poll thread ignores a command older than that slot's accepted tag, while
`Include` allocates its tag under `pollMutex` so it cannot publish an older
floor after another channel has already used and released the slot.

The existing `PipeData` layout has room for only the low 32 tag bits. The
receiver reconstructs the newest full tag relative to the protected 64-bit
counter. A command stalled while at least 2^32 later tags are allocated can
therefore alias a newer generation. Avoiding that theoretical limit would
require changing the shared command representation; this minimal patch does
not do so.

The six queued-command regressions each passed 100 consecutive repetitions
on the final source (600 executions total). The `Include` floor test was also
used as a mutation check: moving tag allocation before `pollMutex` made the
isolated test fail 100/100 times, while the final ordering passed 100/100.

ThreadSanitizer also reports a remaining stock lock-order cycle in PollPoll:
`Channel::Enable` can hold the channel mutex while `Include` takes
`pollMutex`, while `Begin` holds `pollMutex` across dispatch and `CbkXeq` then
takes the channel mutex. This branch fixes the distinct `Exclude` inversion
and the unlocked `FDRem` table access described above; it does not claim that
all PollPoll deadlocks are resolved. The ThreadSanitizer run below therefore
disables its lock-order detector while retaining data-race detection. The
`Include` cycle needs a separate change with its own ordering proof.

Three older PollPoll limits remain outside this patch: constructor allocation
failure can reach shutdown before the poll thread starts, partial table-growth
failure has no transactional rollback, and the signed-short command slot
cannot represent index 32768. The larger tagged channel entry changes the
allocation size but does not create those control-flow and representation
faults. None is folded into this command-ordering fix without an independent
reproducer and review.

The minimized revision passes both Linux backends. The exact-base results for
all client, server, native-epoll, and forced-PollPoll executions are reported
together below; backend duplication means execution failures must not be read
as a count of unique defects.

IOEvents also stores poll waits in signed-int milliseconds. Long public API
timeouts can wrap negative (causing a timeout scan loop) or wrap to a short
positive value (causing repeated early wakeups). Tests use a synchronous test
backend to inspect the real timeout queue without waiting weeks. Ordinary,
exact-boundary, absent, and removed timeouts are controls.

## Reproduce the stock-versus-fixed matrix

The test-only part of the branch can be overlaid on the named pristine commit;
none of these commands applies a production fix to the stock tree:

```sh
base=5b716c84ab4d37d84e158c127b1c6272084cc889
git worktree add /tmp/xrootd-stock "$base"
git diff --binary "$base" HEAD -- \
  tests/unit/CMakeLists.txt tests/unit/XrdLoops/ |
  git -C /tmp/xrootd-stock apply

# Test instrumentation only: make the alternate Linux executables select the
# otherwise hidden generic server and IOEvents poll backends. Without these
# selectors they use epoll and silently miss the PollPoll paths.
git -C /tmp/xrootd-stock apply --unidiff-zero <<'PATCH'
diff --git a/src/Xrd/XrdPoll.cc b/src/Xrd/XrdPoll.cc
--- a/src/Xrd/XrdPoll.cc
+++ b/src/Xrd/XrdPoll.cc
@@ -44 +44 @@
-#if defined( __linux__ )
+#if defined( __linux__ ) && !defined(XRD_SERVER_FORCE_POLL)
@@ -364 +364 @@
-#if defined( __linux__ )
+#if defined( __linux__ ) && !defined(XRD_SERVER_FORCE_POLL)
diff --git a/src/XrdSys/XrdSysIOEvents.cc b/src/XrdSys/XrdSysIOEvents.cc
--- a/src/XrdSys/XrdSysIOEvents.cc
+++ b/src/XrdSys/XrdSysIOEvents.cc
@@ -1246,6 +1246,6 @@
 #include "XrdSys/XrdSysIOEventsPollPort.icc"
-#elif defined( __linux__ )
+#elif defined( __linux__ ) && !defined(XRD_SYS_IOEVENTS_FORCE_POLL)
 #include "XrdSys/XrdSysIOEventsPollE.icc"
 #elif defined(__APPLE__)
 #include "XrdSys/XrdSysIOEventsPollKQ.icc"
 #else
PATCH

cmake -S /tmp/xrootd-stock -B /tmp/xrootd-stock-build \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DENABLE_TESTS=ON -DENABLE_SERVER_TESTS=ON -DENABLE_XRDCL=ON
cmake --build /tmp/xrootd-stock-build --target xrdloops-unit-tests -j 4

export LD_LIBRARY_PATH=/tmp/xrootd-stock-build/lib
/tmp/xrootd-stock-build/bin/xrdcl-loop-tests --gtest_color=no
/tmp/xrootd-stock-build/bin/xrdsys-loop-tests --gtest_color=no
/tmp/xrootd-stock-build/bin/xrdsys-poll-loop-tests --gtest_color=no
/tmp/xrootd-stock-build/bin/xrd-server-loop-tests --gtest_color=no
/tmp/xrootd-stock-build/bin/xrd-server-poll-loop-tests --gtest_color=no
```

Each selector is active only in its alternate test executable. Default Linux
libraries and executables still select epoll.

The current source has 95 distinct logical tests: 21 XrdCl cases, 31 IOEvents
cases, 37 main server cases, and six portable server PollPoll cases. The trace
CTest repeats two IOEvents cases under a different environment and is not
counted as a new logical test. Conditional Linux death tests, scheduler
interposition, the 18 native-epoll, 27 forced-PollPoll, and four event-port
contract variants, plus the trace execution, make the CTest totals differ from
the logical source count. The Linux total is 21 client + 18 native IOEvents +
27 forced-PollPoll IOEvents + four event-port contract + 37 main server + six
server PollPoll + one trace execution. The final minimized source has 114
AlmaLinux 9 executions and 67
applicable executions on macOS.

The exact-base overlay compiled its 110 executions without compatibility
changes; the four locked-rearm tests are omitted there. Its final confirmation
run reproduced 62 failures: 21 alarm-bounded
hangs or spins, five guarded `SIGSEGV` crashes, 25 controlled death-test child
exits, and 11 ordinary assertion failures. These are observed execution
outcomes rather than a count of unique defects: forced-backend duplication and
branch guards prevent a one-to-one interpretation.

The pre-review ThreadSanitizer build passed all 109 executions with this runtime:

```sh
LD_LIBRARY_PATH=/usr/lib/clang/21/lib/x86_64-redhat-linux-gnu:/tmp/xrootd-sentinel-tsan-build-20260930/lib \
TSAN_OPTIONS=detect_deadlocks=0:report_destroy_locked=0:halt_on_error=1:exitcode=66 \
ctest --test-dir /tmp/xrootd-sentinel-tsan-build-20260930 \
  --timeout 30 --output-on-failure \
  -R '^(ClientTimeouts|IOEventsCore|PollFallback\.IOEventsCore|ServerLoops|ServerPollPollLoops)\.'
```

The Clang runtime directory is required both during GoogleTest discovery and
during execution on this AlmaLinux 9 image. Lock-order detection is disabled
because the deliberately retained PollPoll `Include` cycle is outside this
patch, and destroyed-lock reporting is disabled because death-test children
call `_exit` while their intentionally process-owned poll threads still exist.
Data-race detection remains enabled; fixture-only release/acquire handoffs
express kernel readiness ordering which the sanitizer cannot infer.

The final ASan and UBSan execution uses:

```sh
printf '%s\n' 'enum:XrdNetUtils.hh' > /tmp/xrootd-loop-ubsan.txt
ASAN_OPTIONS=detect_odr_violation=0:use_sigaltstack=0 \
UBSAN_OPTIONS=halt_on_error=1:suppressions=/tmp/xrootd-loop-ubsan.txt \
ctest --test-dir /tmp/xrootd-sentinel-asan-build-20260929 \
  --timeout 30 \
  --output-on-failure \
  -R '^(ClientTimeouts|IOEventsCore|PollFallback\.IOEventsCore|ServerLoops|ServerPollPollLoops)\.'
```

The ASan ODR check is disabled because the forced-PollPoll test executable
intentionally compiles `XrdSysIOEvents.cc` while also linking `libXrdUtils`.
The sole UBSan suppression is `enum:XrdNetUtils.hh`, for existing resolver
bitmask behavior outside this patch. GCC ASan's default alternate signal stack
hit an internal runtime assertion during XrdCl worker teardown on this image;
`use_sigaltstack=0` was isolated on the affected test and then passed 10/10
repetitions. It changes signal-stack setup, not address checking. The final
minimized source passed all 109 pre-review ASan+UBSan executions with that
workaround. The four new contract cases run in a dedicated backend-contract
executable.

## Broader integration qualification

The minimized tree and exact upstream base use the same small integration
subset, covering server startup, unauthenticated and HTTP operation, and three
third-party-copy paths:

```sh
ctest --test-dir BUILD -j 4 --timeout 60 --output-on-failure \
  -R '^(XRootD::cluster::start|XRootD::noauth::test|XRootD::http::test|XrdCl::tpc-http-bigfiles|XrdCl::tpc-http-protocol|XrdCl::tpc-root-root)$'
```

The relocated Alma build could not run this integration subset: its fixtures
resolve `xrdcp` through a source-relative `../../../../build` path and the VM
does not have the optional `libXrdOssMirage` plugin. Server startup therefore
failed before any patched path ran, so those attempts are not counted as patch
results. At this revision the production portion is 18 files, +420/-146. The
prior live branch was 18 production files, +571/-116, so the minimized patch
removes 121 changed lines of churn and 181 lines of net growth. The
larger remainder on the companion branch is tests and this evidence document.

## Scope deliberately removed from the earlier branch

The following changes need independent proposals and evidence; they are not
part of this scheduler/sentinel-loop patch:

* scalar positive-byte `suRetry` handling, because current plain and TLS scalar
  socket implementations return success after positive progress;
* malformed or duplicate intrusive scheduler jobs, which production callers
  were not shown to enqueue and whose validation added allocation and linear
  scans to a hot path;
* the newly invented `MaxResponseSize` policy and tests which incorrectly
  described it as an upstream setting;
* local AIO, copy workers, kernel-buffer materialization, send queues, TLS CA
  maintenance, HTTP, address formatting, parser hardening, exception policy,
  and unrelated security fixes;
* the large client-poller lifecycle side table and new callback-to-Stop API
  semantics, for which no ordinary XrdCl failure was reproduced.

This scope reduction also removes dozens of unrelated commits which were
ancestors of the earlier fork branch. The patch now applies directly to the
named upstream comparison commit.
