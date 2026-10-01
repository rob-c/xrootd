// Progress regressions with controlled thread order, never fabricated wire data.
#include "XrdSys/XrdSysPthread.hh"
#include "Xrd/XrdPoll.hh"
#include "Xrd/XrdTrace.hh"
// Observe existing queue/fence state without adding test APIs to production.
#define private public
#include "Xrd/XrdScheduler.hh"
#ifdef __linux__
#include "Xrd/XrdPollE.hh"
#endif
#include "Xrd/XrdLinkCtl.hh"
#undef private
#include "Xrd/XrdInet.hh"
#include "Xrd/XrdLinkMatch.hh"
#include "Xrd/XrdLinkXeq.hh"
#include "Xrd/XrdProtocol.hh"
#include "XrdNet/XrdNetAddr.hh"
#include "XrdSys/XrdSysError.hh"

#include <gtest/gtest.h>
#include <atomic>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <fcntl.h>
#include <new>
#include <string>
#include <thread>
#include <vector>
#include <sys/socket.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/eventfd.h>
#include <dlfcn.h>
#endif

#ifdef __linux__
namespace {
std::atomic<bool> countPollThreadAllocations{false};
std::atomic<unsigned int> pollThreadAllocations{0};
std::atomic<pthread_t> allocationThread{};
thread_local bool failNextArrayAllocation = false;

void CountPollThreadAllocation()
{
  if (countPollThreadAllocations.load(std::memory_order_acquire) &&
      pthread_equal(pthread_self(),
                    allocationThread.load(std::memory_order_relaxed)))
    pollThreadAllocations.fetch_add(1, std::memory_order_relaxed);
}
}

void *operator new(std::size_t size)
{
  void *memory = std::malloc(size ? size : 1);
  if (!memory) throw std::bad_alloc();
  CountPollThreadAllocation();
  return memory;
}

void *operator new(std::size_t size, const std::nothrow_t &) noexcept
{
  return std::malloc(size ? size : 1);
}

void operator delete(void *memory, const std::nothrow_t &) noexcept
{
  std::free(memory);
}

void operator delete(void *memory) noexcept { std::free(memory); }
void operator delete(void *memory, std::size_t) noexcept { std::free(memory); }

void *operator new[](std::size_t size, const std::nothrow_t &) noexcept
{
  if (failNextArrayAllocation)
  {
    failNextArrayAllocation = false;
    return nullptr;
  }
  return std::malloc(size ? size : 1);
}

void operator delete[](void *memory, const std::nothrow_t &) noexcept
{
  std::free(memory);
}
#endif

namespace XrdGlobal {
extern XrdSysError Log;
extern XrdScheduler Sched;
extern XrdInet *XrdNetTCP;
extern int devNull;
}

namespace {
void Check(bool ok, const char *expression, int line)
{
  if (ok) return;
  dprintf(STDERR_FILENO, "ServerLoopTests.cc:%d: %s\n", line, expression);
  _exit(1);
}
#define REQUIRE(expression) Check((expression), #expression, __LINE__)
#define FINISH_WITHIN(seconds, ...) \
  ASSERT_EXIT(([] { alarm(seconds); __VA_ARGS__ _exit(0); }()), \
              ::testing::ExitedWithCode(0), "")

class Job : public XrdJob
{
public:
  explicit Job(std::function<void()> action) : XrdJob("loop proof"), action(action) {}
  void DoIt() override { action(); }
private:
  std::function<void()> action;
};

class MutexAccess : public XrdSysMutex
{
public:
  static pthread_mutex_t *Native(XrdSysMutex &mutex)
  {
    return &reinterpret_cast<MutexAccess &>(mutex).cs;
  }
};

class LockableLink : public XrdLinkXeq
{
public:
  using XrdLink::Activate;
  using XrdLink::Hold;
  using XrdLink::setProtocol;
  void SetInstance(unsigned int value) { Instance = value; }
  unsigned int GetInstance() const { return Instance; }
  void KeepDescriptor() { KeepFD = true; }
  pthread_mutex_t *OperationMutex() { return MutexAccess::Native(LinkInfo.opMutex); }
};

XrdSysSemaphore *closeTraceEntered = nullptr;
XrdSysSemaphore *closeTraceRelease = nullptr;

void PauseDeferredCloseTrace(const char *, const char *message, bool)
{
  if (!message || !std::strstr(message, " deferred, use count=")) return;
  closeTraceEntered->Post();
  closeTraceRelease->Wait();
}

int SerializationWaiters(XrdLink *link);

[[maybe_unused]] bool IsEnabled(const std::atomic<bool> &enabled)
{
  return enabled.load(std::memory_order_acquire);
}

[[maybe_unused]] bool IsEnabled(const bool &enabled) { return enabled; }

[[maybe_unused]] bool UsesAtomicLinkTable(std::atomic<char> *) { return true; }
[[maybe_unused]] bool UsesAtomicLinkTable(char *) { return false; }

template <class Info>
auto HasRegistration(Info *info, unsigned int instance, int)
  -> decltype(info->Generation, bool())
{
  return info->Generation == instance;
}

template <class Info>
bool HasRegistration(Info *, unsigned int, long) { return true; }

// User: an out-of-tree protocol stores the public XrdLink::Terminate member
// pointer for later dispatch. Adding an internal overload must not make that
// previously valid source fail to compile.
// Branch guard: compiles against upstream 5b716c84a; published revision
// e8848d15b made the inferred member pointer ambiguous before this executable
// could build.
TEST(ServerLoops, PublicTerminateMemberPointerRemainsUnambiguous)
{
  auto terminate = &XrdLink::Terminate;
  (void)terminate;
}

// User: an out-of-tree server component stores the public XrdLinkXeq::Close
// member pointer for later dispatch. Valid-path control: stock upstream
// 5b716c84a has one unambiguous Close(bool). Branch guard: the internal
// generation helper must keep a distinct name rather than overload that API.
TEST(ServerLoops, PublicCloseMemberPointerRemainsUnambiguous)
{
  auto close = &XrdLinkXeq::Close;
  (void)close;
}

#ifdef __linux__
// User: a server starting under allocation pressure should log setup failure
// and return it to the caller instead of terminating on std::bad_alloc.
// Valid-path control: stock uses malloc and an explicit null check. Branch
// guard: the atomic replacement must retain that nonthrowing failure path.
TEST(ServerLoops, LinkTableAllocationFailureUsesExistingErrorPath)
{
  FINISH_WITHIN(5,
    if (!UsesAtomicLinkTable(XrdLinkCtl::LinkBat)) _exit(0);
    failNextArrayAllocation = true;
    REQUIRE(XrdLinkCtl::Setup(1024, 0) == 0);
    REQUIRE(!failNextArrayAllocation);
  );
}
#endif

#ifdef __linux__
struct TimerWait
{
  XrdSysSemaphore entered{0}, release{0};
  int seconds = 0;
};
std::atomic<TimerWait *> timerWait{nullptr};

struct AllocResetWait
{
  XrdSysSemaphore entered{0}, release{0};
};
thread_local AllocResetWait *allocResetWait = nullptr;

struct LinkPublishWait
{
  XrdSysSemaphore entered{0}, release{0};
};
thread_local LinkPublishWait *linkPublishWait = nullptr;

std::atomic<bool> pauseActivityPost{false}, activityPostPaused{false};
std::atomic<bool> releaseActivityPost{false}, activityWaiterDestroyed{false};
std::atomic<bool> closerLockAttempted{false};
std::atomic<pthread_t> activityPoster{}, activityCloser{};
std::atomic<pthread_mutex_t *> activityOpMutex{nullptr};
std::atomic<sem_t *> activitySemaphore{nullptr};
#endif
}

#ifdef __linux__
namespace {
using MutexLock = int (*)(pthread_mutex_t *);
using SemPost = int (*)(sem_t *);
using SemDestroy = int (*)(sem_t *);

MutexLock RealMutexLock()
{
  static auto call = reinterpret_cast<MutexLock>(
    dlsym(RTLD_NEXT, "pthread_mutex_lock"));
  return call;
}

SemPost RealSemPost()
{
  static auto call = reinterpret_cast<SemPost>(dlsym(RTLD_NEXT, "sem_post"));
  return call;
}

SemDestroy RealSemDestroy()
{
  static auto call = reinterpret_cast<SemDestroy>(
    dlsym(RTLD_NEXT, "sem_destroy"));
  return call;
}
}

extern "C" int pthread_mutex_lock(pthread_mutex_t *mutex) noexcept
{
  if (mutex == activityOpMutex.load(std::memory_order_acquire)
      && pthread_equal(pthread_self(), activityCloser.load()))
    closerLockAttempted.store(true, std::memory_order_release);
  return RealMutexLock()(mutex);
}

extern "C" int sem_post(sem_t *semaphore) noexcept
{
  const int result = RealSemPost()(semaphore);
  if (pauseActivityPost.load(std::memory_order_acquire)
      && pthread_equal(pthread_self(), activityPoster.load()))
  {
    activitySemaphore.store(semaphore, std::memory_order_release);
    activityPostPaused.store(true, std::memory_order_release);
    while (!releaseActivityPost.load(std::memory_order_acquire))
      sched_yield();
  }
  return result;
}

extern "C" int sem_destroy(sem_t *semaphore) noexcept
{
  if (activityPostPaused.load(std::memory_order_acquire)
      && semaphore == activitySemaphore.load(std::memory_order_acquire))
    activityWaiterDestroyed.store(true, std::memory_order_release);
  return RealSemDestroy()(semaphore);
}

// ELF executable interposition pauses the existing timer wait before its actual
// condition wait. No clock, queue entry, return value or production code is
// mocked. With upstream the signal happens during this pause and is lost;
// with the fix the condition lock serializes the producer with this waiter.
// Keep the original Wait(int) forwarding behavior when no barrier is armed.
int XrdSysCondVar::Wait(int seconds)
{
  if (TimerWait *barrier = timerWait.exchange(nullptr))
  {
    barrier->seconds = seconds;
    barrier->entered.Post();
    barrier->release.Wait();
  }
  return WaitMS(seconds * 1000);
}

// Reused-link Reset calls time() while initializing LinkInfo. Pausing that
// existing call gives the test below an exact observation point without a
// production hook or a scheduler-dependent sleep.
extern "C" time_t time(time_t *result) noexcept
{
  struct timespec now;
  clock_gettime(CLOCK_REALTIME, &now);
  if (result) *result = now.tv_sec;
  if (AllocResetWait *barrier = allocResetWait)
  {
    allocResetWait = nullptr;
    barrier->entered.Post();
    barrier->release.Wait();
  }
  return now.tv_sec;
}

// Alloc calls strdup after releasing the link-table lock. Pausing that real
// call exposes whether the slot was published before its fields were ready,
// without adding a test seam to the server.
extern "C" char *strdup(const char *source) noexcept
{
  const size_t size = std::strlen(source) + 1;
  char *copy = static_cast<char *>(std::malloc(size));
  if (copy) std::memcpy(copy, source, size);
  if (LinkPublishWait *barrier = linkPublishWait)
  {
    linkPublishWait = nullptr;
    barrier->entered.Post();
    barrier->release.Wait();
  }
  return copy;
}

namespace {
void TimerWakeProof(bool existingTimer)
{
  auto *scheduler = new XrdScheduler(3, 3, 0);
  auto *done = new XrdSysSemaphore(0);
  auto *late = new Job([] { _exit(1); });
  auto *immediate = new Job([done] { done->Post(); });
  if (existingTimer) scheduler->Schedule(late, time(nullptr) + 600);

  TimerWait barrier;
  timerWait.store(&barrier);
  scheduler->Start();
  barrier.entered.Wait();
  REQUIRE(barrier.seconds > 0);
  std::thread producer([&] { scheduler->Schedule(immediate, time(nullptr)); });

  // Observing insertion under TimerMutex is the decisive ordering barrier:
  // stock signals while still owning this lock, so its signal has completed.
  // Fixed code drops this lock then waits for the condition mutex we paused.
  for (;;)
  {
    scheduler->TimerMutex.Lock();
    bool published = scheduler->TimerQueue == immediate;
    scheduler->TimerMutex.UnLock();
    if (published) break;
    std::this_thread::yield();
  }
  barrier.release.Post();
  done->Wait();
  producer.join();
  // Scheduler has no Stop API; child exit owns all scheduler/thread lifetimes.
}

bool FenceAndIsEnabled(XrdPollE &poller, XrdPollInfo &info)
{
  int rc;
  do rc = eventfd_write(poller.WaitFd, 1); while (rc < 0 && errno == EINTR);
  REQUIRE(rc == 0);
  poller.WaitFdSem.Wait();
  // The poll thread cannot mutate isEnabled until the second half is posted.
  bool enabled = info.isEnabled;
  poller.WaitFdSem2.Post();
  return enabled;
}

}

// User: an idle server schedules a timeout/retry just as its timer thread goes
// to sleep, leaving an otherwise valid operation stuck for up to an hour.
// Stock reproduced against upstream 5b716c84a: forced queue-check/wait ordering
// loses the wakeup. Input and dispatch are real; no malformed job is involved.
TEST(ServerLoops, EmptyTimerQueueCannotLoseTheFirstWakeup)
{
  FINISH_WITHIN(5, TimerWakeProof(false););
}

// User: an earlier retry/timeout is added while the server has a later deadline;
// waiting for that old deadline makes the new operation appear hung.
// Stock reproduced against upstream 5b716c84a: the same lost-wakeup window
// occurs with an existing pending timer.
TEST(ServerLoops, EarlierTimerCannotLoseItsWakeup)
{
  FINISH_WITHIN(5, TimerWakeProof(true););
}

// User: a service schedules a long retention/expiry interval. Overflowing the
// wait conversion can run it early or repeatedly attempt an expired wait.
// Stock reproduced against upstream 5b716c84a: a valid time_t deadline narrows
// before it is bounded; UINT_MAX+60 produces a deterministic short wait.
TEST(ServerLoops, FarFutureTimerUsesARepresentablePositiveWait)
{
  FINISH_WITHIN(5,
    if (sizeof(time_t) <= sizeof(int)) _exit(0);
    auto *scheduler = new XrdScheduler(3, 3, 0);
    Job future([] { _exit(1); });
    // UINT_MAX+60 narrows to a short positive int in upstream, making this a
    // deterministic early-wakeup check rather than relying on signed wrap.
    scheduler->Schedule(&future, time(nullptr) + time_t(UINT_MAX) + 60);
    TimerWait barrier;
    timerWait.store(&barrier);
    scheduler->Start();
    barrier.entered.Wait();
    REQUIRE(barrier.seconds == INT_MAX / 1000);
  );
}

// User: a socket hangs up while another server thread owns its link lock and
// waits for the poller during Close. One link then wedges the whole poll thread.
// Stock reproduced against upstream 5b716c84a: a deterministic kernel HUP
// enters the same Finish path, which waits for that lock. The existing eventfd
// fence forces the ordering; this does not claim that plain FIN hangs a daemon.
TEST(ServerLoops, FatalEventDoesNotBlockTheCloseFenceOnTheLinkLock)
{
  FINISH_WITHIN(5,
    REQUIRE(XrdLinkCtl::Setup(1024, 0));
    REQUIRE(XrdPoll::Setup(1024));
    int sockets[2];
    // An empty pipe whose writer closes produces EPOLLHUP without EPOLLIN,
    // selecting the fatal-only path rather than the readable-EOF wrapper.
    REQUIRE(pipe(sockets) == 0);
    LockableLink link;
    link.LinkInfo.FD = link.PollInfo.FD = sockets[0];
    REQUIRE(link.Activate());
    REQUIRE(link.PollInfo.Poller->Enable(link.PollInfo));
    link.Hold(true);
    REQUIRE(close(sockets[1]) == 0);
    auto *poller = static_cast<XrdPollE *>(link.PollInfo.Poller);
    while (FenceAndIsEnabled(*poller, link.PollInfo)) {}
    REQUIRE(link.Close() == 0);
    link.Hold(false);
  );
}
#endif

// User: many independent requests schedule immediate timers at once. The lost
// wakeup repair must preserve all jobs and normal timer delivery.
// Valid-path control: passes upstream 5b716c84a; no duplicate job is queued.
TEST(ServerLoops, ConcurrentIndependentTimersAllExecuteExactlyOnce)
{
  FINISH_WITHIN(10,
    auto *scheduler = new XrdScheduler(3, 6, 0);
    XrdSysSemaphore done(0);
    std::atomic<int> executed{0};
    std::vector<Job *> jobs;
    for (int i = 0; i < 200; ++i)
      jobs.push_back(new Job([&] { ++executed; done.Post(); }));
    scheduler->Start();
    std::thread first([&] {
      for (int i = 0; i < 100; ++i) scheduler->Schedule(jobs[i], time(nullptr));
    });
    std::thread second([&] {
      for (int i = 100; i < 200; ++i) scheduler->Schedule(jobs[i], time(nullptr));
    });
    first.join(); second.join();
    for (int i = 0; i < 200; ++i) done.Wait();
    REQUIRE(executed == 200);
  );
}

namespace {
class ReadingProtocol : public XrdProtocol
{
public:
  ReadingProtocol() : XrdProtocol("readiness control") {}
  void DoIt() override {}
  XrdProtocol *Match(XrdLink *) override { return nullptr; }
  int Process(XrdLink *link) override
  {
    char byte = 0;
    int rc = link->Recv(&byte, 1, 1000);
    if (rc != 1) return -1;
    REQUIRE(byte == 'q');
    ++receiveCount;
    received.Post();
    return 1;
  }
  void Recycle(XrdLink *, int, const char *reason) override
  {
    recycleReason = reason ? reason : "";
    ++recycleCount;
    recycled.Post();
  }
  int Stats(char *, int, int = 0) override { return 0; }
  std::atomic<int> receiveCount{0}, recycleCount{0};
  std::string recycleReason;
  XrdSysSemaphore received{0}, recycled{0};
};

class SerializingProtocol : public ReadingProtocol
{
public:
  int Process(XrdLink *link) override
  {
    // XrdCmsProtocol performs this same quiescence step before it returns.
    serializing.Post();
    link->Serialize();
    return ReadingProtocol::Process(link);
  }
  XrdSysSemaphore serializing{0};
};

class FatalProtocol : public ReadingProtocol
{
public:
  int Process(XrdLink *link) override
  {
    ++processCount;
    ReadingProtocol::Process(link);
    return -1;
  }
  std::atomic<int> processCount{0};
};

class EnabledRecycleProtocol : public ReadingProtocol
{
public:
  void Recycle(XrdLink *link, int seconds, const char *reason) override
  {
    // Kernel readiness orders Enable before cleanup, but race detectors cannot
    // infer that edge. Mirror it here so sanitizer reports stay actionable.
    REQUIRE(enabled.load(std::memory_order_acquire));
    ReadingProtocol::Recycle(link, seconds, reason);
  }
  std::atomic<bool> enabled{false};
};

class BlockingProtocol : public ReadingProtocol
{
public:
  explicit BlockingProtocol(bool readFirst = false) : readFirst(readFirst) {}
  int Process(XrdLink *link) override
  {
    if (++processCount != 1) return 1;
    int result = 1;
    if (readFirst) result = ReadingProtocol::Process(link);
    entered.Post();
    release.Wait();
    result = readFirst ? result : ReadingProtocol::Process(link);
    returned.Post();
    return result;
  }
  const bool readFirst;
  std::atomic<int> processCount{0};
  XrdSysSemaphore entered{0}, release{0}, returned{0};
};

class SelfClosingProtocol : public ReadingProtocol
{
public:
  int Process(XrdLink *link) override
  {
    const int call = ++processCount;
    if (call != 1) return 1;
    ReadingProtocol::Process(link);
    closeResult = link->Close();
    recycledInline = recycleCount.load() != 0;
    closeReturned.Post();
    release.Wait();
    // Zero normally lets a worker consume another buffered request directly.
    // Once Close has been requested, Dispatch must leave the sticky loop.
    return 0;
  }
  std::atomic<int> closeResult{-1}, processCount{0};
  std::atomic<bool> recycledInline{false};
  XrdSysSemaphore closeReturned{0}, release{0};
};

template <class Link>
auto RunPinned(Link *link, unsigned int instance, int fd, XrdPoll *poller, int)
  -> decltype(link->DoItPinned(instance), void())
{
  link->DoItPinned(instance);
}

template <class Link>
auto RunPinned(Link *link, unsigned int instance, int fd, XrdPoll *poller, long)
  -> decltype(link->DoItPinned(instance, fd, poller), void())
{
  link->DoItPinned(instance, fd, poller);
}

// Keep the same test source buildable as an overlay on pristine upstream,
// which has no generation-pinned dispatch entry point.
template <class Link>
void RunPinned(Link *, unsigned int, int, XrdPoll *, ...) {}

template <class Link>
auto RunNested(Link *link, unsigned int instance, int)
  -> decltype(link->DoItPinned(instance), void())
{
  link->DoItPinned(instance);
}

template <class Link>
void RunNested(Link *link, unsigned int, long) { link->DoIt(); }

class NestedClosingProtocol : public ReadingProtocol
{
public:
  explicit NestedClosingProtocol(XrdLink *target) : target(target) {}
  int Process(XrdLink *) override
  {
    ++processCount;
    closeResult = target->Close();
    return 1;
  }
  XrdLink *target;
  std::atomic<int> closeResult{-1}, processCount{0};
};

class NestedDispatchProtocol : public ReadingProtocol
{
public:
  NestedDispatchProtocol(XrdLinkXeq *nested, unsigned int instance)
    : nested(nested), instance(instance) {}
  int Process(XrdLink *) override
  {
    ++processCount;
    RunNested(nested, instance, 0);
    recycledInline = recycleCount.load() != 0;
    return 1;
  }
  XrdLinkXeq *nested;
  unsigned int instance;
  std::atomic<int> processCount{0};
  std::atomic<bool> recycledInline{false};
};
}

// User: CMS asks Close(true) to interrupt a blocked socket while retaining the
// link slot and protocol for its later cleanup job. Valid-path control: stock
// upstream 5b716c84a performs only the deferred shutdown. Branch guard: the
// generation-aware final-close path must not recycle or decrement this link.
TEST(ServerLoops, DeferredShutdownDoesNotEnterFinalRetirement)
{
  FINISH_WITHIN(5,
    class DeferredProtocol : public ReadingProtocol
    {
    public:
      int Process(XrdLink *link) override
      {
        ++processCount;
        closeResult = link->Close(true);
        return 1;
      }
      std::atomic<int> closeResult{-1}, processCount{0};
    } protocol;
    int sockets[2];
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    XrdGlobal::devNull = open("/dev/null", O_RDONLY);
    REQUIRE(XrdGlobal::devNull >= 0);
    LockableLink link;
    link.SetInstance(7);
    link.LinkInfo.InUse = 1;
    link.LinkInfo.FD = link.PollInfo.FD = sockets[0];
    link.setProtocol(&protocol, false);

    link.DoIt();
    REQUIRE(protocol.processCount == 1);
    REQUIRE(protocol.closeResult == 0);
    REQUIRE(link.GetInstance() == 0);
    REQUIRE(link.LinkInfo.InUse == 1);
    REQUIRE(link.getProtocol() == &protocol);
    REQUIRE(protocol.recycleCount == 0);
    REQUIRE(fcntl(sockets[0], F_GETFD) >= 0);

    REQUIRE(close(sockets[0]) == 0);
    REQUIRE(close(sockets[1]) == 0);
    REQUIRE(close(XrdGlobal::devNull) == 0);
    XrdGlobal::devNull = -1;
  );
}

// User: an ordinary close waits for a busy CMS connection while Close(true)
// shuts down its socket so blocked I/O will wake. The waiting close must still
// recycle the protocol and release the link-table slot after the user drains.
// Stock branch guard: #2964 head 08fcbb4d9 treats shutdown's zero Instance as
// slot reuse and returns without performing any of that final cleanup.
TEST(ServerLoops, DeferredShutdownPreservesWaitingFinalClose)
{
  FINISH_WITHIN(5,
    XrdInet network(&XrdGlobal::Log);
    XrdGlobal::XrdNetTCP = &network;
    REQUIRE(XrdLinkCtl::Setup(1024, 0));

    int sockets[2];
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    XrdGlobal::devNull = open("/dev/null", O_RDONLY);
    REQUIRE(XrdGlobal::devNull >= 0);
    XrdNetAddr peer;
    REQUIRE(peer.Set(sockets[0]) == nullptr);
    XrdLink *link = XrdLinkCtl::Alloc(peer);
    REQUIRE(link != nullptr);
    ReadingProtocol protocol;
    link->setProtocol(&protocol);
    link->setRef(1);

    std::atomic<int> result{-1};
    std::thread closer([&] { result = link->Close(); });
    while (SerializationWaiters(link) != 1) std::this_thread::yield();
    REQUIRE(link->Close(true) == 0);
    REQUIRE(link->Inst() == 0);
    link->setRef(-1);
    closer.join();

    REQUIRE(result == 0);
    REQUIRE(protocol.recycleCount == 1);
    REQUIRE(XrdLinkCtl::fd2link(sockets[0]) == nullptr);
    REQUIRE(close(sockets[1]) == 0);
    REQUIRE(close(XrdGlobal::devNull) == 0);
    XrdGlobal::devNull = -1;
  );
}

// User: deferred shutdown cannot replace the socket when the daemon's
// /dev/null descriptor is invalid, so the existing link remains responsible
// for recovery. Stock upstream 5b716c84a restores its generation and continues
// dispatching. Branch guard: the new terminal marker must roll back with that
// generation instead of silently making the live link reject every job.
TEST(ServerLoops, FailedShutdownRestoresDispatchState)
{
  FINISH_WITHIN(5,
    class CountingProtocol : public ReadingProtocol
    {
    public:
      int Process(XrdLink *) override
      {
        ++processCount;
        return -EINPROGRESS;
      }
      std::atomic<int> processCount{0};
    } protocol;
    int sockets[2];
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    LockableLink link;
    link.SetInstance(9);
    link.LinkInfo.InUse = 1;
    link.LinkInfo.FD = link.PollInfo.FD = sockets[0];
    link.setProtocol(&protocol, false);

    const int savedDevNull = XrdGlobal::devNull;
    XrdGlobal::devNull = -1;
    link.Shutdown(true);
    XrdGlobal::devNull = savedDevNull;

    REQUIRE(link.GetInstance() == 9);
    REQUIRE(fcntl(sockets[0], F_GETFD) >= 0);
    RunNested(&link, 9, 0);
    REQUIRE(protocol.processCount == 1);
    link.PollInfo.FD = -1;
    REQUIRE(link.Close() == 0);
    REQUIRE(protocol.recycleCount == 1);
    REQUIRE(close(sockets[1]) == 0);
  );
}

// User: after enough accepted connections for the generation counter to wrap,
// a generation-zero readiness job can be queued just as shutdown replaces the
// socket. Stock reproduced: upstream 5b716c84a dispatches the queued raw link
// after Shutdown; zero must not compare as a still-live generation.
TEST(ServerLoops, ShutdownRejectsGenerationZeroDispatch)
{
  FINISH_WITHIN(5,
    int sockets[2];
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    LockableLink link;
    ReadingProtocol protocol;
    link.SetInstance(0);
    link.KeepDescriptor();
    link.LinkInfo.InUse = 1;
    link.LinkInfo.FD = sockets[0];
    link.PollInfo.FD = -1;
    link.setProtocol(&protocol, false);
    REQUIRE(write(sockets[1], "q", 1) == 1);

    link.Shutdown(true);
    RunNested(&link, 0, 0);
    REQUIRE(protocol.receiveCount == 0);
    REQUIRE(link.Close() == 0);
    REQUIRE(protocol.recycleCount == 1);
    REQUIRE(close(sockets[0]) == 0);
    REQUIRE(close(sockets[1]) == 0);
  );
}

#ifdef __linux__
// User: a busy client has thousands of buffered requests, so the poll thread
// disables each readable event while a scheduler worker consumes one request
// and rearms the connection. Stock reproduced under ThreadSanitizer against
// upstream 5b716c84a: XrdPollE::Start writes isEnabled while XrdPollE::Enable
// reads it, with no synchronization between the poll and scheduler threads.
TEST(ServerLoops, ConcurrentReadinessRearmPublishesEnabledState)
{
  FINISH_WITHIN(10,
    REQUIRE(XrdLinkCtl::Setup(1024, 0));
    REQUIRE(XrdPoll::Setup(1024));
    XrdGlobal::Sched.setParms(1, 1, 1, 0);
    XrdGlobal::Sched.Start();

    int descriptors[2];
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors) == 0);
    LockableLink link;
    ReadingProtocol protocol;
    link.LinkInfo.FD = link.PollInfo.FD = descriptors[0];
    link.setProtocol(&protocol);
    REQUIRE(link.Activate());
    REQUIRE(link.PollInfo.Poller->Enable(link.PollInfo));

    constexpr int requestCount = 4096;
    std::vector<char> requests(requestCount, 'q');
    REQUIRE(write(descriptors[1], requests.data(), requests.size()) ==
            requestCount);
    for (int request = 0; request < requestCount; ++request)
      protocol.received.Wait();
    REQUIRE(protocol.receiveCount == requestCount);
  );
}

// User: a busy server dispatches an ordinary request on every poll wakeup.
// Adding one heap allocation to that hot path introduces allocator contention
// and makes otherwise healthy traffic depend on allocation success.
// Valid-path control: upstream 5b716c84a queues its embedded link without a
// poll-thread allocation. Branch guard: the generation wrapper must retain a
// reusable idle job; replacing it with a new job for every event fails here.
TEST(ServerLoops, OrdinaryReadinessKeepsThePollThreadAllocationFree)
{
  FINISH_WITHIN(10,
    // Prove that this executable's replaceable operator new is observed before
    // relying on it to measure the production poll thread.
    allocationThread.store(pthread_self(), std::memory_order_relaxed);
    pollThreadAllocations.store(0, std::memory_order_relaxed);
    countPollThreadAllocations.store(true, std::memory_order_release);
    void *probe = ::operator new(1);
    countPollThreadAllocations.store(false, std::memory_order_release);
    ::operator delete(probe);
    REQUIRE(pollThreadAllocations.load(std::memory_order_relaxed) == 1);

    REQUIRE(XrdLinkCtl::Setup(1024, 0));
    REQUIRE(XrdPoll::Setup(1024));
    XrdGlobal::Sched.setParms(1, 1, 1, 0);
    XrdGlobal::Sched.Start();

    int descriptors[2];
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors) == 0);
    LockableLink link;
    ReadingProtocol protocol;
    link.LinkInfo.FD = link.PollInfo.FD = descriptors[0];
    link.setProtocol(&protocol);
    REQUIRE(link.Activate());
    REQUIRE(link.PollInfo.Poller->Enable(link.PollInfo));

    XrdSysSemaphore warmDone(0), measuredDone(0);
    Job warmMarker([&] { warmDone.Post(); });
    Job measuredMarker([&] { measuredDone.Post(); });

    // Complete one dispatch and then a scheduler marker. The marker proves the
    // reusable wrapper has returned and published its idle state.
    REQUIRE(write(descriptors[1], "q", 1) == 1);
    protocol.received.Wait();
    XrdGlobal::Sched.Schedule(&warmMarker);
    warmDone.Wait();

    allocationThread.store(link.PollInfo.Poller->TID,
                           std::memory_order_relaxed);
    pollThreadAllocations.store(0, std::memory_order_relaxed);
    countPollThreadAllocations.store(true, std::memory_order_release);
    REQUIRE(write(descriptors[1], "q", 1) == 1);
    protocol.received.Wait();
    XrdGlobal::Sched.Schedule(&measuredMarker);
    measuredDone.Wait();
    countPollThreadAllocations.store(false, std::memory_order_release);

    REQUIRE(protocol.receiveCount == 2);
    REQUIRE(pollThreadAllocations.load(std::memory_order_relaxed) == 0);
  );
}
#endif

// User: a CMS connection drops and its protocol quiesces outstanding link work
// from inside Process before removing the node. The readiness wrapper must not
// make Serialize wait for the wrapper which can return only after Process.
// Valid-path control: upstream 5b716c84a completes this real readiness path.
// Branch guard: the first generation-pinned dispatch self-deadlocks here.
TEST(ServerLoops, ProtocolProcessCanSerializeItsOwnReadinessDispatch)
{
  FINISH_WITHIN(10,
    XrdInet network(&XrdGlobal::Log);
    XrdGlobal::XrdNetTCP = &network;
    REQUIRE(XrdLinkCtl::Setup(1024, 0));
    REQUIRE(XrdPoll::Setup(1024));
    XrdGlobal::Sched.setParms(1, 1, 1, 0);
    XrdGlobal::Sched.Start();

    int sockets[2];
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    XrdNetAddr peer;
    REQUIRE(peer.Set(sockets[0]) == nullptr);
    XrdLink *link = XrdLinkCtl::Alloc(peer);
    REQUIRE(link != nullptr);
    SerializingProtocol protocol;
    link->setProtocol(&protocol);
    REQUIRE(link->Activate());
    auto *info = XrdLinkCtl::fd2PollInfo(sockets[0]);
    REQUIRE(info && info->Poller->Enable(*info));

    REQUIRE(write(sockets[1], "q", 1) == 1);
    protocol.received.Wait();
    REQUIRE(close(sockets[1]) == 0);
    protocol.recycled.Wait();
    REQUIRE(protocol.receiveCount == 1 && protocol.recycleCount == 1);
  );
}

// User: a protocol closes its own connection from Process during initial or
// poll-readiness dispatch, while another readiness job may already be queued.
// Stock reproduced against upstream 5b716c84a: Close recycles the protocol
// before Process returns, so subsequent member access is unsafe. Branch guard:
// pending retirement rejects the queued job and waits for dispatch to return.
TEST(ServerLoops, ProtocolProcessCanCloseItsOwnDispatch)
{
  FINISH_WITHIN(10,
    XrdInet network(&XrdGlobal::Log);
    XrdGlobal::XrdNetTCP = &network;
    REQUIRE(XrdLinkCtl::Setup(1024, 0));
    REQUIRE(XrdPoll::Setup(1024));
    XrdGlobal::Sched.setParms(1, 1, 1, 0);
    XrdGlobal::Sched.Start();

    int initial[2];
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, initial) == 0);
    XrdNetAddr initialPeer;
    REQUIRE(initialPeer.Set(initial[0]) == nullptr);
    XrdLink *initialLink = XrdLinkCtl::Alloc(initialPeer);
    REQUIRE(initialLink != nullptr);
    SelfClosingProtocol initialProtocol;
    initialLink->setProtocol(&initialProtocol);
    REQUIRE(write(initial[1], "q", 1) == 1);
    std::thread initialDispatch([&] { ((XrdLinkXeq *)initialLink)->DoIt(); });
    initialProtocol.closeReturned.Wait();
    REQUIRE(!initialProtocol.recycledInline);
    initialProtocol.release.Post();
    initialDispatch.join();
    initialProtocol.recycled.Wait();
    REQUIRE(initialProtocol.processCount == 1);
    REQUIRE(initialProtocol.receiveCount == 1
            && initialProtocol.recycleCount == 1);
    REQUIRE(close(initial[1]) == 0);

    int sockets[2];
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    XrdNetAddr peer;
    REQUIRE(peer.Set(sockets[0]) == nullptr);
    XrdLink *link = XrdLinkCtl::Alloc(peer);
    REQUIRE(link != nullptr);
    SelfClosingProtocol protocol;
    link->setProtocol(&protocol);
    REQUIRE(link->Activate());
    auto *info = XrdLinkCtl::fd2PollInfo(sockets[0]);
    REQUIRE(info && info->Poller->Enable(*info));

    REQUIRE(write(sockets[1], "q", 1) == 1);
    protocol.closeReturned.Wait();
    std::thread overlap([&] {
      RunPinned((XrdLinkXeq *)link, link->Inst(), info->FD, info->Poller, 0);
    });
    overlap.join();
    REQUIRE(protocol.processCount == 1);
    protocol.release.Post();
    protocol.recycled.Wait();
    REQUIRE(protocol.closeResult == 0);
    REQUIRE(!protocol.recycledInline);
    REQUIRE(protocol.receiveCount == 1 && protocol.recycleCount == 1);
    while (XrdLinkCtl::fd2link(sockets[0]) != nullptr)
      std::this_thread::yield();
    REQUIRE(close(sockets[1]) == 0);

  );
}

// User: one protocol synchronously dispatches work on a second connection,
// whose callback closes the first connection before returning. Stock upstream
// 5b716c84a recycles the outer protocol while its Process frame is still live.
// Branch guard: self-close detection must find an ancestor activity as well as
// the current one, or the nested Close waits forever for its own call stack.
TEST(ServerLoops, NestedDispatchCanCloseAnAncestorConnection)
{
  FINISH_WITHIN(5,
    LockableLink outer, inner;
    outer.SetInstance(1);
    outer.LinkInfo.InUse = 1;
    outer.LinkInfo.FD = outer.PollInfo.FD = -1;
    inner.SetInstance(2);
    inner.LinkInfo.InUse = 1;
    inner.LinkInfo.FD = inner.PollInfo.FD = -1;

    NestedClosingProtocol innerProtocol((XrdLink *)&outer);
    NestedDispatchProtocol outerProtocol(&inner, 2);
    inner.setProtocol(&innerProtocol, false);
    outer.setProtocol(&outerProtocol, false);

    RunNested(&outer, 1, 0);

    REQUIRE(innerProtocol.processCount == 1);
    REQUIRE(innerProtocol.closeResult == 0);
    REQUIRE(outerProtocol.processCount == 1);
    REQUIRE(!outerProtocol.recycledInline);
    REQUIRE(outerProtocol.recycleCount == 1);
  );
}

// User: an administrator lists clients while one of them disconnects. The
// lookup must not return the retired link or leave a reference on its reusable
// table slot, which can make a later close wait forever.
// Stock reproduced against upstream 5b716c84a: Close resets the generation and
// use count while Find waits to pin the match; Find then raises the dead slot's
// use count and returns it because both observed generations are zero.
TEST(ServerLoops, FindPinsOnlyTheConnectionWhoseIdentityItMatched)
{
  FINISH_WITHIN(10,
    XrdInet network(&XrdGlobal::Log);
    XrdGlobal::XrdNetTCP = &network;
    REQUIRE(XrdLinkCtl::Setup(1024, 0));

    int first[2], target[2];
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, first) == 0);
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, target) == 0);
    XrdNetAddr firstPeer, targetPeer;
    REQUIRE(firstPeer.Set(first[0]) == nullptr);
    REQUIRE(targetPeer.Set(target[0]) == nullptr);
    XrdLink *firstLink = XrdLinkCtl::Alloc(firstPeer, XRDLINK_NOCLOSE);
    XrdLink *targetLink = XrdLinkCtl::Alloc(targetPeer, XRDLINK_NOCLOSE);
    REQUIRE(firstLink && targetLink && first[0] < target[0]);
    ReadingProtocol targetProtocol;
    targetLink->setProtocol(&targetProtocol);

    XrdLinkMatch match("*");
    int cursor = -1;
    REQUIRE(XrdLinkCtl::Find(cursor, &match) == firstLink);
    firstLink->Hold(true);
    XrdSysSemaphore finderStarted(0);
    XrdLink *found = nullptr;
    std::thread finder([&] {
      finderStarted.Post();
      found = XrdLinkCtl::Find(cursor, &match);
    });
    finderStarted.Wait();
    while (XrdLinkCtl::LTMutex.CondLock())
    {
      XrdLinkCtl::LTMutex.UnLock();
      std::this_thread::yield();
    }

    std::thread closer([&] { REQUIRE(targetLink->Close() == 0); });
    targetProtocol.recycled.Wait();
    firstLink->Hold(false);
    finder.join();
    closer.join();

    REQUIRE(found == nullptr);
    REQUIRE(targetLink->UseCnt() == 0);
    REQUIRE(firstLink->Close() == 0);
    REQUIRE(close(first[0]) == 0);
    REQUIRE(close(first[1]) == 0);
    REQUIRE(close(target[0]) == 0);
    REQUIRE(close(target[1]) == 0);
  );
}

#ifdef __linux__
// User: a management lookup, statistics scrape, or idle scan runs while the
// accept loop reuses a descriptor. Each must skip the initializing slot rather
// than expose stale fields or wait behind setup of the replacement client.
// Stock reproduced against upstream 5b716c84a: Alloc marks the slot used before
// strdup initializes HostName, so lookup APIs expose it while SyncAll and
// idleScan enter the partially initialized link. Branch guard: every reader
// skips LinkInit, and retiring a higher descriptor does not shrink the table
// scan's high-water mark past the replacement.
TEST(ServerLoops, LinkSlotIsVisibleOnlyAfterInitialization)
{
  FINISH_WITHIN(10,
    XrdInet network(&XrdGlobal::Log);
    XrdGlobal::XrdNetTCP = &network;
    REQUIRE(XrdLinkCtl::Setup(1024, 0));

    int sockets[2], higher[2];
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, higher) == 0);
    XrdNetAddr peer, higherPeer;
    REQUIRE(peer.Set(sockets[0]) == nullptr);
    REQUIRE(higherPeer.Set(higher[0]) == nullptr);
    XrdLink *oldLink = XrdLinkCtl::Alloc(peer, XRDLINK_NOCLOSE);
    XrdLink *highLink = XrdLinkCtl::Alloc(higherPeer, XRDLINK_NOCLOSE);
    REQUIRE(oldLink != nullptr && highLink != nullptr && higher[0] > sockets[0]);
    const unsigned int replacementInstance = oldLink->Inst() + 2;
    REQUIRE(oldLink->Close() == 0);
    REQUIRE(XrdLinkCtl::fd2link(sockets[0]) == nullptr);

    LinkPublishWait barrier;
    XrdLink *replacement = nullptr;
    std::thread allocator([&] {
      linkPublishWait = &barrier;
      replacement = XrdLinkCtl::Alloc(peer, XRDLINK_NOCLOSE);
    });
    barrier.entered.Wait();
    REQUIRE(XrdLinkCtl::fd2link(sockets[0]) == nullptr);
    REQUIRE(XrdLinkCtl::fd2link(sockets[0], replacementInstance) == nullptr);
    REQUIRE(XrdLinkCtl::fd2PollInfo(sockets[0]) == nullptr);
    REQUIRE(highLink->Close() == 0);
    int cursor = -1;
    char client[256];
    REQUIRE(XrdLinkCtl::Find(cursor) == nullptr);
    cursor = -1;
    REQUIRE(XrdLinkCtl::getName(cursor, client, sizeof(client)) == 0);

    // Alloc retains opMutex until publication. These joins therefore complete
    // only if the table-wide maintenance paths also reject LinkInit; accepting
    // it blocks here until the enclosing ten-second regression alarm fires.
    std::thread statsSync([] { XrdLinkCtl::SyncAll(); });
    statsSync.join();
    std::thread idleScanner([] { XrdLinkCtl::idleScan(); });
    idleScanner.join();

    barrier.release.Post();
    allocator.join();

    REQUIRE(replacement == oldLink);
    REQUIRE(XrdLinkCtl::fd2link(sockets[0]) == replacement);
    REQUIRE(XrdLinkCtl::fd2link(sockets[0], replacementInstance) == replacement);
    REQUIRE(XrdLinkCtl::fd2PollInfo(sockets[0]) != nullptr);
    cursor = -1;
    REQUIRE(XrdLinkCtl::Find(cursor) == replacement);
    REQUIRE(XrdLinkCtl::Find(cursor) == nullptr);
    REQUIRE(replacement->Close() == 0);
    REQUIRE(close(sockets[0]) == 0);
    REQUIRE(close(sockets[1]) == 0);
    REQUIRE(close(higher[0]) == 0);
    REQUIRE(close(higher[1]) == 0);
  );
}

// User: a monitoring lookup observes a newly accepted client while allocation
// is still running on another CPU. Stock reproduced: upstream 5b716c84a races
// the slot and can expose partially initialized identity/poll fields. The release
// publication and acquire lookup are the only synchronization in this test;
// weakening either to relaxed is reported by ThreadSanitizer.
TEST(ServerLoops, LinkPublicationMakesInitializedFieldsVisible)
{
  FINISH_WITHIN(10,
    XrdInet network(&XrdGlobal::Log);
    XrdGlobal::XrdNetTCP = &network;
    REQUIRE(XrdLinkCtl::Setup(1024, 0));
    int sockets[2];
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    XrdNetAddr peer;
    REQUIRE(peer.Set(sockets[0]) == nullptr);

    std::thread allocator([&] {
      REQUIRE(XrdLinkCtl::Alloc(peer, XRDLINK_NOCLOSE) != nullptr);
    });
    XrdLink *link;
    while (!(link = XrdLinkCtl::fd2link(sockets[0])))
      std::this_thread::yield();
    XrdPollInfo *info = XrdLinkCtl::fd2PollInfo(sockets[0]);
    REQUIRE(info != nullptr);
    REQUIRE(link->FDnum() == sockets[0]);
    REQUIRE(link->Host() && *link->Host());
    REQUIRE(info->FD == sockets[0]);
    REQUIRE(HasRegistration(info, link->Inst(), 0));
    allocator.join();

    REQUIRE(link->Close() == 0);
    REQUIRE(close(sockets[0]) == 0);
    REQUIRE(close(sockets[1]) == 0);
  );
}
#endif

// User: a monitoring lookup repeatedly checks the highest descriptor while the
// accept loop retires and reuses it. Stock reproduced under ThreadSanitizer
// against upstream 5b716c84a: fd2link/fd2PollInfo read LinkBat while Alloc and
// Unhook write it; the versioned lookup and Find also read Instance while Close
// resets it. Find can then pin a replacement after matching the old connection.
TEST(ServerLoops, ConcurrentLinkSlotReusePublishesLookupState)
{
  FINISH_WITHIN(20,
    XrdInet network(&XrdGlobal::Log);
    XrdGlobal::XrdNetTCP = &network;
    REQUIRE(XrdLinkCtl::Setup(1024, 0));

    int target[2];
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, target) == 0);
    XrdNetAddr targetPeer;
    REQUIRE(targetPeer.Set(target[0]) == nullptr);
    XrdLink *link = XrdLinkCtl::Alloc(targetPeer, XRDLINK_NOCLOSE);
    REQUIRE(link != nullptr);

    std::atomic<bool> begin{false}, stop{false};
    std::atomic<unsigned int> lookups{0};
    XrdSysSemaphore started(0);
    std::thread lookup([&] {
      started.Post();
      while (!begin.load(std::memory_order_acquire)) std::this_thread::yield();
      do
      {
        XrdLink *seen = XrdLinkCtl::fd2link(target[0]);
        XrdLink *versioned = XrdLinkCtl::fd2link(target[0], UINT_MAX);
        XrdPollInfo *info = XrdLinkCtl::fd2PollInfo(target[0]);
        int cursor = -1;
        XrdLink *matched = XrdLinkCtl::Find(cursor);
        if (matched) REQUIRE(XrdLinkCtl::Find(cursor) == nullptr);
        int nameCursor = -1;
        char client[256];
        int nameLength = XrdLinkCtl::getName(nameCursor, client,
                                              sizeof(client));
        lookups.fetch_add((seen ? 2u : 1u) + (versioned ? 2u : 1u) +
                          (info ? 2u : 1u) + (matched ? 2u : 1u) +
                          (nameLength ? 2u : 1u),
                          std::memory_order_relaxed);
      }
      while (!stop.load(std::memory_order_acquire));
    });
    started.Wait();
    begin.store(true, std::memory_order_release);
    for (int reuse = 0; reuse < 4096; ++reuse)
    {
      REQUIRE(link->Close() == 0);
      link = XrdLinkCtl::Alloc(targetPeer, XRDLINK_NOCLOSE);
      REQUIRE(link != nullptr);
    }
    stop.store(true, std::memory_order_release);
    lookup.join();
    REQUIRE(lookups.load() != 0);

    REQUIRE(link->Close() == 0);
    REQUIRE(close(target[0]) == 0);
    REQUIRE(close(target[1]) == 0);
  );
}

#ifdef __linux__
// User: a client disappears before sending any bytes, so epoll reports a
// terminal event with no readable request. Deferred cleanup must still run and
// recycle its protocol rather than leaving the disabled connection stranded.
// Branch guard: upstream 5b716c84a passes synchronously; this proves the new
// no-readable XrdPollFinishJob reaches cleanup. The standalone stack fixture
// isolates this fatal path without relying on link-table reuse.
TEST(ServerLoops, PureFatalEventRunsDeferredCleanup)
{
  FINISH_WITHIN(10,
    REQUIRE(XrdLinkCtl::Setup(1024, 0));
    REQUIRE(XrdPoll::Setup(1024));
    XrdGlobal::Sched.setParms(1, 1, 1, 0);
    XrdGlobal::Sched.Start();

    int descriptors[2];
    REQUIRE(pipe(descriptors) == 0);
    LockableLink link;
    EnabledRecycleProtocol protocol;
    link.LinkInfo.FD = link.PollInfo.FD = descriptors[0];
    link.setProtocol(&protocol);
    REQUIRE(link.Activate());
    REQUIRE(link.PollInfo.Poller->Enable(link.PollInfo));
    protocol.enabled.store(true, std::memory_order_release);
    REQUIRE(close(descriptors[1]) == 0);
    protocol.recycled.Wait();
    XrdSysSemaphore drained(0);
    Job marker([&] { drained.Post(); });
    XrdGlobal::Sched.Schedule(&marker);
    drained.Wait();
    REQUIRE(protocol.recycleCount == 1);
    REQUIRE(protocol.recycleReason == "hangup");
    REQUIRE(link.PollInfo.Poller == nullptr && link.PollInfo.FD == -1);
  );
}
#endif

// User: a peer disconnects while an unrelated client sends another request.
// Valid-path control: passes upstream 5b716c84a. Real readable data reaches
// its protocol and fatal cleanup recycles each protocol exactly once.
TEST(ServerLoops, ReadableRequestsAndPeerDisconnectsBothMakeProgress)
{
  FINISH_WITHIN(10,
    XrdInet network(&XrdGlobal::Log);
    XrdGlobal::XrdNetTCP = &network;
    REQUIRE(XrdLinkCtl::Setup(1024, 0));
    REQUIRE(XrdPoll::Setup(1024));
    XrdGlobal::Sched.setParms(3, 6, 3, 0);
    XrdGlobal::Sched.Start();
    int sockets[2][2];
    ReadingProtocol protocols[2];
    for (int i = 0; i < 2; ++i)
    {
      REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets[i]) == 0);
      XrdNetAddr peer;
      REQUIRE(peer.Set(sockets[i][0]) == nullptr);
      XrdLink *link = XrdLinkCtl::Alloc(peer);
      REQUIRE(link != nullptr);
      link->setProtocol(&protocols[i]);
      REQUIRE(link->Activate());
      auto *info = XrdLinkCtl::fd2PollInfo(sockets[i][0]);
      REQUIRE(info && info->Poller->Enable(*info));
      REQUIRE(write(sockets[i][1], "q", 1) == 1);
      protocols[i].received.Wait();
    }
    REQUIRE(close(sockets[0][1]) == 0);
    protocols[0].recycled.Wait();
    REQUIRE(write(sockets[1][1], "q", 1) == 1);
    protocols[1].received.Wait();
    REQUIRE(close(sockets[1][1]) == 0);
    protocols[1].recycled.Wait();
    REQUIRE(protocols[0].recycled.CondWait() == 0);
    REQUIRE(protocols[1].recycled.CondWait() == 0);
  );
}

// User: a client sends its final request, half-closes its write side, and stays
// connected to receive the reply. Every buffered request must be dispatched
// across re-enables before the poller handles EOF. Stock reproduced on Linux
// epoll; upstream PollPoll passes.
// Branch guard: an intermediate adversarial edit also made PollPoll treat a
// readable hangup as fatal and discard the buffered request; it was removed.
TEST(ServerLoops, BufferedRequestPrecedesPeerHalfClose)
{
  FINISH_WITHIN(10,
    XrdInet network(&XrdGlobal::Log);
    XrdGlobal::XrdNetTCP = &network;
    REQUIRE(XrdLinkCtl::Setup(1024, 0));
    REQUIRE(XrdPoll::Setup(1024));
    XrdGlobal::Sched.setParms(3, 6, 3, 0);
    XrdGlobal::Sched.Start();

    int sockets[2];
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    XrdNetAddr peer;
    REQUIRE(peer.Set(sockets[0]) == nullptr);
    XrdLink *link = XrdLinkCtl::Alloc(peer);
    REQUIRE(link != nullptr);
    ReadingProtocol protocol;
    link->setProtocol(&protocol);
    REQUIRE(link->Activate());
    auto *info = XrdLinkCtl::fd2PollInfo(sockets[0]);
    REQUIRE(info && info->Poller->Enable(*info));

    REQUIRE(write(sockets[1], "qqq", 3) == 3);
    REQUIRE(shutdown(sockets[1], SHUT_WR) == 0);
    protocol.recycled.Wait();
    REQUIRE(protocol.receiveCount == 3);
    REQUIRE(protocol.recycleCount == 1);
    REQUIRE(close(sockets[1]) == 0);
  );
}

#ifdef __linux__
// User: under scheduler backlog and descriptor churn, delayed fatal cleanup
// still points at a slot reused by an accept thread. Resetting the replacement
// outside its operation lock can corrupt or disconnect the newly accepted
// client.
// Branch guard: pristine upstream and first pushed revision f1839f5d0 enter Reset
// without owning that lock. Upstream has no deferred finish job, so this
// bootstraps the synchronization required by this branch's fix.
TEST(ServerLoops, ReusedLinkInitializationHoldsOperationLock)
{
  FINISH_WITHIN(10,
    XrdInet network(&XrdGlobal::Log);
    XrdGlobal::XrdNetTCP = &network;
    REQUIRE(XrdLinkCtl::Setup(1024, 0));

    int first[2];
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, first) == 0);
    XrdNetAddr peer;
    REQUIRE(peer.Set(first[0]) == nullptr);
    XrdLink *oldLink = XrdLinkCtl::Alloc(peer);
    REQUIRE(oldLink != nullptr);
    const int reusedFD = first[0];
    REQUIRE(oldLink->Close() == 0);
    REQUIRE(close(first[1]) == 0);

    int replacement[2];
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, replacement) == 0);
    if (replacement[1] == reusedFD)
      std::swap(replacement[0], replacement[1]);
    if (replacement[0] != reusedFD)
    {
      REQUIRE(dup2(replacement[0], reusedFD) == reusedFD);
      REQUIRE(close(replacement[0]) == 0);
      replacement[0] = reusedFD;
    }
    REQUIRE(peer.Set(replacement[0]) == nullptr);

    AllocResetWait reset;
    XrdLink *newLink = nullptr;
    std::thread allocator([&] {
      allocResetWait = &reset;
      newLink = XrdLinkCtl::Alloc(peer);
    });

    reset.entered.Wait();
    auto *implementation = (XrdLinkXeq *)oldLink;
    const bool resetOwnsLock = !implementation->LinkInfo.opMutex.CondLock();
    if (!resetOwnsLock) implementation->LinkInfo.opMutex.UnLock();
    REQUIRE(resetOwnsLock);
    reset.release.Post();
    allocator.join();
    REQUIRE(newLink == oldLink);
    REQUIRE(newLink->Close() == 0);
    REQUIRE(close(replacement[1]) == 0);
  );
}
#endif

namespace {
struct CloseDecision
{
  CloseDecision(bool approve, XrdLink *link, bool block = false) :
    approve(approve), link(link), block(block) {}
  static bool Decide(void *argument)
  {
    auto *decision = static_cast<CloseDecision *>(argument);
    REQUIRE(decision->enabled.load(std::memory_order_acquire));
    // The public callback contract is unlocked. Model a protocol waiting for
    // another teardown worker which needs the link operation lock.
    std::thread inspector([decision] {
      decision->link->Hold(true);
      decision->link->Hold(false);
    });
    inspector.join();
    // Serialize is also part of the public unlocked callback contract. A
    // lifetime guard owned by this callback must not wait for itself.
    decision->link->Serialize();
    if (decision->block)
    {
      decision->entered.Post();
      decision->release.Wait();
    }
    ++decision->calls;
    decision->called.Post();
    return decision->approve;
  }
  const bool approve;
  XrdLink *link;
  const bool block;
  std::atomic<bool> enabled{false};
  std::atomic<int> calls{0};
  XrdSysSemaphore entered{0}, release{0}, called{0};
};

struct SelfClosingDecision
{
  SelfClosingDecision(XrdLink *link, ReadingProtocol *protocol) :
    link(link), protocol(protocol) {}
  static bool Decide(void *argument)
  {
    auto *decision = static_cast<SelfClosingDecision *>(argument);
    REQUIRE(decision->enabled.load(std::memory_order_acquire));
    decision->closeResult = decision->link->Close();
    decision->recycledInline = decision->protocol->recycleCount.load() != 0;
    decision->closeReturned.Post();
    return false;
  }
  XrdLink *link;
  ReadingProtocol *protocol;
  std::atomic<bool> enabled{false};
  std::atomic<int> closeResult{-1};
  std::atomic<bool> recycledInline{false};
  XrdSysSemaphore closeReturned{0};
};

void CloseCallbackProof(bool approve)
{
  XrdInet network(&XrdGlobal::Log);
  XrdGlobal::XrdNetTCP = &network;
  REQUIRE(XrdLinkCtl::Setup(1024, 0));
  REQUIRE(XrdPoll::Setup(1024));
  XrdGlobal::Sched.setParms(1, 1, 1, 0);
  XrdGlobal::Sched.Start();

  int sockets[2];
  REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
  XrdNetAddr peer;
  REQUIRE(peer.Set(sockets[0]) == nullptr);
  XrdLink *link = XrdLinkCtl::Alloc(peer);
  REQUIRE(link != nullptr);
  ReadingProtocol protocol;
  CloseDecision decision(approve, link);
  link->setProtocol(&protocol);
  REQUIRE(XrdLinkCtl::RegisterCloseRequestCb(
    link, &protocol, CloseDecision::Decide, &decision));
  REQUIRE(link->Activate());
  auto *info = XrdLinkCtl::fd2PollInfo(sockets[0]);
  REQUIRE(info && info->Poller->Enable(*info));
  decision.enabled.store(true, std::memory_order_release);
  REQUIRE(close(sockets[1]) == 0);

  decision.called.Wait();
  auto *drained = new XrdSysSemaphore(0);
  auto *marker = new Job([drained] { drained->Post(); });
  XrdGlobal::Sched.Schedule(marker);
  drained->Wait();
  REQUIRE(decision.calls == 1);
  if (approve)
  {
    REQUIRE(protocol.recycleCount == 1);
    REQUIRE(XrdLinkCtl::fd2link(sockets[0]) == nullptr);
  }
  else
  {
    REQUIRE(protocol.recycleCount == 0);
    REQUIRE(XrdLinkCtl::fd2link(sockets[0]) == link);
    REQUIRE(link->Close() == 0);
    REQUIRE(protocol.recycleCount == 1);
    REQUIRE(XrdLinkCtl::fd2link(sockets[0]) == nullptr);
  }
}

int SerializationWaiters(XrdLink *link)
{
  // XrdLinkCtl stores an XrdLinkXeq-derived object and uses the same downcast
  // for its public callback registration entry point.
  auto *implementation = (XrdLinkXeq *)link;
  link->Hold(true);
  const int waiters = implementation->LinkInfo.doPost;
  link->Hold(false);
  return waiters;
}

template<class Link>
auto ActivityWaitHead(Link *link, XrdLink *publicLink, int)
  -> decltype((void)link->PollInfo.ActivityWaitQ, (const void *)nullptr)
{
  publicLink->Hold(true);
  const void *head = link->PollInfo.ActivityWaitQ;
  publicLink->Hold(false);
  return head;
}

// The exact-base overlay has no activity fence. Keep waiting until stock Close
// finishes, then the caller deterministically observes its premature recycle.
template<class Link>
const void *ActivityWaitHead(Link *, XrdLink *, long) { return nullptr; }

const void *ActivityWaitHead(XrdLink *link)
{
  return ActivityWaitHead((XrdLinkXeq *)link, link, 0);
}
}

#ifdef __linux__
// User: one server worker finishes a request while another thread closes the
// same connection. The close waiter must remain alive until the worker has
// completely returned from waking it. Branch regression: the first #2965
// revision destroys the stack semaphore while sem_post is still in progress;
// this bootstraps the corrected waiter lifetime rather than a stock failure.
TEST(ServerLoops, ActivityWaiterOutlivesThePostingCall)
{
  FINISH_WITHIN(5,
    class AtomicProtocol : public ReadingProtocol
    {
    public:
      int Process(XrdLink *) override
      {
        worker.store(pthread_self(), std::memory_order_release);
        entered.store(true, std::memory_order_release);
        while (!release.load(std::memory_order_acquire)) sched_yield();
        return -EINPROGRESS;
      }
      std::atomic<bool> entered{false}, release{false};
      std::atomic<pthread_t> worker{};
    } protocol;

    LockableLink link;
    link.SetInstance(29);
    link.LinkInfo.InUse = 1;
    link.LinkInfo.FD = link.PollInfo.FD = -1;
    link.setProtocol(&protocol, false);

    activityPostPaused.store(false);
    releaseActivityPost.store(false);
    activityWaiterDestroyed.store(false);
    closerLockAttempted.store(false);
    activitySemaphore.store(nullptr);
    activityOpMutex.store(nullptr);
    pauseActivityPost.store(false);

    std::thread running([&] { RunNested(&link, 29, 0); });
    while (!protocol.entered.load(std::memory_order_acquire)) sched_yield();

    std::thread closer([&] {
      activityCloser.store(pthread_self(), std::memory_order_release);
      link.Close();
    });
    while (ActivityWaitHead((XrdLink *)&link) == nullptr) sched_yield();

    activityPoster.store(protocol.worker.load(std::memory_order_acquire));
    activityOpMutex.store(link.OperationMutex(), std::memory_order_release);
    pauseActivityPost.store(true, std::memory_order_release);
    protocol.release.store(true, std::memory_order_release);

    while (!activityPostPaused.load(std::memory_order_acquire)) sched_yield();
    while (!closerLockAttempted.load(std::memory_order_acquire)) sched_yield();
    REQUIRE(!activityWaiterDestroyed.load(std::memory_order_acquire));

    releaseActivityPost.store(true, std::memory_order_release);
    running.join();
    closer.join();
  );
}
#endif

// User: two scheduler workers overlap on one connection and one reports a
// fatal protocol error. While final close waits for the other worker, another
// queued readiness job must not enter the protocol and prolong retirement.
// Branch regression: the first #2965 revision leaves closePending clear and
// admits the third dispatch; this bootstraps the fatal-retirement fence fix.
TEST(ServerLoops, FatalResultFencesDispatchBeforeWaitingForActivity)
{
  FINISH_WITHIN(5,
    class OverlappingFatalProtocol : public ReadingProtocol
    {
    public:
      int Process(XrdLink *) override
      {
        const int call = ++processCount;
        if (call == 1)
        {
          firstEntered.Post();
          releaseFirst.Wait();
          return 1;
        }
        return call == 2 ? -1 : -EINPROGRESS;
      }
      std::atomic<int> processCount{0};
      XrdSysSemaphore firstEntered{0}, releaseFirst{0};
    } protocol;

    LockableLink link;
    link.SetInstance(17);
    link.LinkInfo.InUse = 1;
    link.LinkInfo.FD = link.PollInfo.FD = -1;
    link.setProtocol(&protocol, false);

    std::thread running([&] { RunNested(&link, 17, 0); });
    protocol.firstEntered.Wait();
    std::atomic<bool> fatalDone{false};
    std::thread fatal([&] {
      RunNested(&link, 17, 0);
      fatalDone.store(true, std::memory_order_release);
    });
    while (!fatalDone.load(std::memory_order_acquire)
           && ActivityWaitHead((XrdLink *)&link) == nullptr)
      std::this_thread::yield();

    RunNested(&link, 17, 0);
    REQUIRE(protocol.processCount == 2);

    protocol.releaseFirst.Post();
    running.join();
    fatal.join();
    REQUIRE(protocol.recycleCount == 1);
  );
}

// User: fatal cleanup is scheduled while a protocol is still handling an
// earlier readable request. Its close callback may serialize link work, but
// final cleanup must not recycle the running protocol after that callback.
// Stock reproduced against upstream 5b716c84a: Disable queues the embedded
// link again, and its concurrent terminal dispatch recycles before Process
// returns. Branch guard: terminal state rejects another queued dispatch, blocks
// re-arm during the callback, never regresses on a later poll error, and waits
// for remaining protocol activity. TSan also proves that close must snapshot
// KeepFD before Unhook lets the slot be reset.
TEST(ServerLoops, ConcurrentFatalDispatchWaitsForRunningProtocol)
{
  FINISH_WITHIN(10,
    XrdInet network(&XrdGlobal::Log);
    XrdGlobal::XrdNetTCP = &network;
    REQUIRE(XrdLinkCtl::Setup(1024, 0));
    REQUIRE(XrdPoll::Setup(1024));
    XrdGlobal::Sched.setParms(2, 2, 2, 0);
    XrdGlobal::Sched.Start();

    int sockets[2];
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    XrdNetAddr peer;
    REQUIRE(peer.Set(sockets[0]) == nullptr);
    XrdLink *link = XrdLinkCtl::Alloc(peer);
    REQUIRE(link != nullptr);
    BlockingProtocol protocol(true);
    CloseDecision decision(true, link, true);
    link->setProtocol(&protocol);
    REQUIRE(XrdLinkCtl::RegisterCloseRequestCb(
      link, &protocol, CloseDecision::Decide, &decision));
    REQUIRE(link->Activate());
    auto *info = XrdLinkCtl::fd2PollInfo(sockets[0]);
    REQUIRE(info && info->Poller->Enable(*info));
    decision.enabled.store(true, std::memory_order_release);

    REQUIRE(write(sockets[1], "q", 1) == 1);
    protocol.entered.Wait();
    // The request has been consumed, so rearming cannot queue another readable
    // dispatch while we force the independent terminal dispatch below.
    REQUIRE(info->Poller->Enable(*info));
    while (!IsEnabled(info->isEnabled))
      std::this_thread::yield();
    link->Hold(true);
    info->Poller->Disable(*info, "forced terminal overlap");
    link->Hold(false);
    decision.entered.Wait();
    std::thread overlap([&] {
      RunPinned((XrdLinkXeq *)link, link->Inst(), info->FD, info->Poller, 0);
    });
    overlap.join();
    REQUIRE(protocol.processCount == 1);
    REQUIRE(decision.calls == 0);
    decision.release.Post();
    decision.called.Wait();
    while (protocol.recycleCount == 0 && ActivityWaitHead(link) == nullptr)
      std::this_thread::yield();
    REQUIRE(decision.calls == 1);
    REQUIRE(ActivityWaitHead(link) != nullptr);
    REQUIRE(protocol.recycleCount == 0);

    protocol.release.Post();
    protocol.received.Wait();
    protocol.recycled.Wait();
    REQUIRE(decision.calls == 1);
    REQUIRE(protocol.receiveCount == 1 && protocol.recycleCount == 1);
    while (XrdLinkCtl::fd2link(sockets[0]) != nullptr)
      std::this_thread::yield();
    REQUIRE(close(sockets[1]) == 0);

    // Reverse the completion order on a second link: Process returns while the
    // terminal callback is still blocked. The old dispatch must not re-arm or
    // admit work during that callback window.
    int rearmSockets[2];
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, rearmSockets) == 0);
    XrdNetAddr rearmPeer;
    REQUIRE(rearmPeer.Set(rearmSockets[0]) == nullptr);
    XrdLink *rearmLink = XrdLinkCtl::Alloc(rearmPeer);
    REQUIRE(rearmLink != nullptr);
    BlockingProtocol rearmProtocol(true);
    CloseDecision rearmDecision(true, rearmLink, true);
    rearmLink->setProtocol(&rearmProtocol);
    REQUIRE(XrdLinkCtl::RegisterCloseRequestCb(
      rearmLink, &rearmProtocol, CloseDecision::Decide, &rearmDecision));
    REQUIRE(rearmLink->Activate());
    auto *rearmInfo = XrdLinkCtl::fd2PollInfo(rearmSockets[0]);
    REQUIRE(rearmInfo && rearmInfo->Poller->Enable(*rearmInfo));
    rearmDecision.enabled.store(true, std::memory_order_release);

    REQUIRE(write(rearmSockets[1], "q", 1) == 1);
    rearmProtocol.entered.Wait();
    REQUIRE(rearmInfo->Poller->Enable(*rearmInfo));
    while (!IsEnabled(rearmInfo->isEnabled)) std::this_thread::yield();
    rearmLink->Hold(true);
    rearmInfo->Poller->Disable(*rearmInfo, "forced terminal rearm overlap");
    rearmLink->Hold(false);
    rearmDecision.entered.Wait();
    rearmProtocol.release.Post();
    rearmProtocol.returned.Wait();
    XrdSysSemaphore drained(0);
    Job marker([&] { drained.Post(); });
    XrdGlobal::Sched.Schedule(&marker);
    drained.Wait();
    REQUIRE(!IsEnabled(rearmInfo->isEnabled));
    REQUIRE(rearmProtocol.processCount == 1 && rearmDecision.calls == 0);

    rearmDecision.release.Post();
    rearmDecision.called.Wait();
    rearmProtocol.recycled.Wait();
    REQUIRE(rearmDecision.calls == 1);
    REQUIRE(rearmProtocol.receiveCount == 1
            && rearmProtocol.recycleCount == 1);
    while (XrdLinkCtl::fd2link(rearmSockets[0]) != nullptr)
      std::this_thread::yield();
    REQUIRE(close(rearmSockets[1]) == 0);

    // A protocol can return a fatal result before a separate poll error arrives.
    // Once its callback starts, Finish must not regress terminal state and admit
    // a second LinkEnd dispatch or callback.
    int callbackSockets[2];
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, callbackSockets) == 0);
    XrdNetAddr callbackPeer;
    REQUIRE(callbackPeer.Set(callbackSockets[0]) == nullptr);
    XrdLink *callbackLink = XrdLinkCtl::Alloc(callbackPeer);
    REQUIRE(callbackLink != nullptr);
    FatalProtocol fatalProtocol;
    CloseDecision callbackDecision(true, callbackLink, true);
    callbackLink->setProtocol(&fatalProtocol);
    REQUIRE(XrdLinkCtl::RegisterCloseRequestCb(
      callbackLink, &fatalProtocol, CloseDecision::Decide, &callbackDecision));
    REQUIRE(callbackLink->Activate());
    auto *callbackInfo = XrdLinkCtl::fd2PollInfo(callbackSockets[0]);
    REQUIRE(callbackInfo && callbackInfo->Poller->Enable(*callbackInfo));
    callbackDecision.enabled.store(true, std::memory_order_release);

    REQUIRE(write(callbackSockets[1], "q", 1) == 1);
    callbackDecision.entered.Wait();
    callbackLink->Hold(true);
    const int duplicate = XrdPoll::Finish(*callbackInfo, "later poll error");
    callbackLink->Hold(false);
    REQUIRE(duplicate == 0);
    std::thread callbackOverlap([&] {
      RunPinned((XrdLinkXeq *)callbackLink, callbackLink->Inst(),
                callbackInfo->FD, callbackInfo->Poller, 0);
    });
    callbackOverlap.join();
    REQUIRE(fatalProtocol.processCount == 1 && callbackDecision.calls == 0);
    callbackDecision.release.Post();
    callbackDecision.called.Wait();
    fatalProtocol.recycled.Wait();
    while (XrdLinkCtl::fd2link(callbackSockets[0]) != nullptr)
      std::this_thread::yield();
    REQUIRE(callbackDecision.calls == 1 && fatalProtocol.recycleCount == 1);
    REQUIRE(close(callbackSockets[1]) == 0);
  );
}

// User: CMS quiesces a connection while a separate request or administrative
// task still owns a link reference. Serialize must retain upstream's InUse-only
// contract: readiness activity adds no self-wait, while that independent user
// must still finish.
// Valid-path control: upstream 5b716c84a waits for the independent reference.
// Branch guard: the first branch revision counted readiness in InUse and made
// Serialize wait for its own dispatch wrapper.
TEST(ServerLoops, ProtocolSerializeStillWaitsForOtherLinkUsers)
{
  FINISH_WITHIN(10,
    XrdInet network(&XrdGlobal::Log);
    XrdGlobal::XrdNetTCP = &network;
    REQUIRE(XrdLinkCtl::Setup(1024, 0));
    REQUIRE(XrdPoll::Setup(1024));
    XrdGlobal::Sched.setParms(1, 1, 1, 0);
    XrdGlobal::Sched.Start();

    int sockets[2];
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    XrdNetAddr peer;
    REQUIRE(peer.Set(sockets[0]) == nullptr);
    XrdLink *link = XrdLinkCtl::Alloc(peer);
    REQUIRE(link != nullptr);
    SerializingProtocol protocol;
    link->setProtocol(&protocol);
    REQUIRE(link->Activate());
    auto *info = XrdLinkCtl::fd2PollInfo(sockets[0]);
    REQUIRE(info && info->Poller->Enable(*info));

    link->setRef(1);
    REQUIRE(write(sockets[1], "q", 1) == 1);
    protocol.serializing.Wait();
    while (SerializationWaiters(link) != 1) std::this_thread::yield();
    REQUIRE(protocol.receiveCount == 0);
    link->setRef(-1);
    protocol.received.Wait();
    REQUIRE(close(sockets[1]) == 0);
    protocol.recycled.Wait();
    REQUIRE(protocol.receiveCount == 1 && protocol.recycleCount == 1);
  );
}

// User: two administrators close a client while its protocol is handling a
// readable request. Neither close may recycle the protocol beneath Process,
// and both must complete after the one connection is retired exactly once.
// Stock reproduced against upstream 5b716c84a: Close recycles the blocked
// protocol immediately. Branch guard: both waiters share the activity fence.
TEST(ServerLoops, ConcurrentClosesWaitForRunningProtocol)
{
  FINISH_WITHIN(10,
    XrdInet network(&XrdGlobal::Log);
    XrdGlobal::XrdNetTCP = &network;
    REQUIRE(XrdLinkCtl::Setup(1024, 0));
    REQUIRE(XrdPoll::Setup(1024));
    XrdGlobal::Sched.setParms(1, 1, 1, 0);
    XrdGlobal::Sched.Start();

    int sockets[2];
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    XrdNetAddr peer;
    REQUIRE(peer.Set(sockets[0]) == nullptr);
    XrdLink *link = XrdLinkCtl::Alloc(peer);
    REQUIRE(link != nullptr);
    BlockingProtocol protocol;
    link->setProtocol(&protocol);
    REQUIRE(link->Activate());
    auto *info = XrdLinkCtl::fd2PollInfo(sockets[0]);
    REQUIRE(info && info->Poller->Enable(*info));

    REQUIRE(write(sockets[1], "q", 1) == 1);
    protocol.entered.Wait();
    std::atomic<int> first{-1}, second{-1};
    std::thread close1([&] { first = link->Close(); });
    while (first == -1 && ActivityWaitHead(link) == nullptr)
      std::this_thread::yield();
    REQUIRE(first == -1);
    const void *firstWaiter = ActivityWaitHead(link);
    REQUIRE(firstWaiter != nullptr);
    std::thread overlap([&] {
      RunPinned((XrdLinkXeq *)link, link->Inst(), info->FD, info->Poller, 0);
    });
    overlap.join();
    REQUIRE(protocol.processCount == 1);
    std::thread close2([&] { second = link->Close(); });
    while (second == -1 && ActivityWaitHead(link) == firstWaiter)
      std::this_thread::yield();
    REQUIRE(first == -1 && second == -1);
    REQUIRE(ActivityWaitHead(link) != firstWaiter);
    REQUIRE(protocol.recycleCount == 0);

    protocol.release.Post();
    protocol.received.Wait();
    close1.join();
    close2.join();
    REQUIRE(first == 0 && second == 0);
    REQUIRE(protocol.receiveCount == 1 && protocol.recycleCount == 1);
    REQUIRE(XrdLinkCtl::fd2link(sockets[0]) == nullptr);
    REQUIRE(close(sockets[1]) == 0);
  );
}

// User: two independent teardown workers close the same busy connection at
// once. Both closers wait for its outstanding link user; only one may recycle
// the protocol and file descriptor when that request releases the link.
// Stock reproduced: upstream 5b716c84a wakes both unguarded Close calls, so the
// loser closes the descriptor a second time and returns EBADF.
TEST(ServerLoops, ConcurrentClosesRetireOneConnectionOnce)
{
  FINISH_WITHIN(10,
    XrdInet network(&XrdGlobal::Log);
    XrdGlobal::XrdNetTCP = &network;
    REQUIRE(XrdLinkCtl::Setup(1024, 0));

    int sockets[2];
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    XrdNetAddr peer;
    REQUIRE(peer.Set(sockets[0]) == nullptr);
    XrdLink *link = XrdLinkCtl::Alloc(peer);
    REQUIRE(link != nullptr);
    ReadingProtocol protocol;
    link->setProtocol(&protocol);
    link->setRef(1);

    std::atomic<int> first{-1}, second{-1};
    std::thread close1([&] { first = link->Close(); });
    std::thread close2([&] { second = link->Close(); });
    while (SerializationWaiters(link) != 2) std::this_thread::yield();
    link->setRef(-1);
    close1.join();
    close2.join();

    REQUIRE(first == 0 && second == 0);
    REQUIRE(protocol.recycleCount == 1);
    REQUIRE(XrdLinkCtl::fd2link(sockets[0]) == nullptr);
    REQUIRE(close(sockets[1]) == 0);
  );
}

// User: one close waits for a busy connection while another close can retire
// and reuse its persistent slot. The first close must register its wait before
// releasing the generation lock, or it can sleep on the replacement client.
// Stock branch guard: 84b24428e reaches the deferred-close trace with no waiter
// registered; the corrected branch has already registered exactly one waiter.
TEST(ServerLoops, CloseRegistersGenerationWaitBeforeUnlock)
{
  FINISH_WITHIN(10,
    XrdInet network(&XrdGlobal::Log);
    XrdGlobal::XrdNetTCP = &network;
    REQUIRE(XrdLinkCtl::Setup(1024, 0));

    int sockets[2];
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    XrdNetAddr peer;
    REQUIRE(peer.Set(sockets[0]) == nullptr);
    XrdLink *link = XrdLinkCtl::Alloc(peer);
    REQUIRE(link != nullptr);
    ReadingProtocol protocol;
    link->setProtocol(&protocol);
    link->setRef(1);

    XrdSysSemaphore entered(0), release(0);
    closeTraceEntered = &entered;
    closeTraceRelease = &release;
    XrdGlobal::XrdTrace.SetLogger(static_cast<XrdSysLogger *>(nullptr));
    XrdGlobal::XrdTrace.SetLogger(PauseDeferredCloseTrace);
    XrdGlobal::XrdTrace.What = TRACE_DEBUG;

    std::atomic<int> result{-1};
    std::thread closer([&] { result = link->Close(); });
    entered.Wait();
    auto *implementation = (XrdLinkXeq *)link;
    REQUIRE(implementation->LinkInfo.doPost == 1);
    release.Post();
    link->setRef(-1);
    closer.join();

    REQUIRE(result == 0);
    REQUIRE(protocol.recycleCount == 1);
    REQUIRE(XrdLinkCtl::fd2link(sockets[0]) == nullptr);
    REQUIRE(close(sockets[1]) == 0);
  );
}

// User: a protocol inspects and serializes link state while approving fatal
// connection cleanup. Branch guard: pristine upstream calls it unlocked;
// deferred cleanup must do the same without waiting for its own lifetime guard,
// invoke it once, and recycle the protocol once.
TEST(ServerLoops, FatalTerminationHonorsCloseCallbackApproval)
{
  FINISH_WITHIN(10, CloseCallbackProof(true););
}

// User: a protocol inspects and serializes link state while vetoing automatic
// fatal cleanup and transferring ownership to an external closer. Branch guard:
// pristine upstream calls it unlocked; deferred cleanup must preserve
// that contract without waiting for its own lifetime guard.
TEST(ServerLoops, FatalTerminationHonorsCloseCallbackVeto)
{
  FINISH_WITHIN(10, CloseCallbackProof(false););
}

// User: a fatal-close callback takes ownership of cleanup by calling Close
// itself, then vetoes the caller's automatic close. Stock reproduced against
// upstream 5b716c84a: Close recycles its protocol storage while that callback
// is still executing. Branch guard: retirement waits for callback return.
TEST(ServerLoops, CloseCallbackCanCloseItsOwnLink)
{
  FINISH_WITHIN(10,
    XrdInet network(&XrdGlobal::Log);
    XrdGlobal::XrdNetTCP = &network;
    REQUIRE(XrdLinkCtl::Setup(1024, 0));
    REQUIRE(XrdPoll::Setup(1024));
    XrdGlobal::Sched.setParms(1, 1, 1, 0);
    XrdGlobal::Sched.Start();

    int sockets[2];
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    XrdNetAddr peer;
    REQUIRE(peer.Set(sockets[0]) == nullptr);
    XrdLink *link = XrdLinkCtl::Alloc(peer);
    REQUIRE(link != nullptr);
    ReadingProtocol protocol;
    SelfClosingDecision decision(link, &protocol);
    link->setProtocol(&protocol);
    REQUIRE(XrdLinkCtl::RegisterCloseRequestCb(
      link, &protocol, SelfClosingDecision::Decide, &decision));
    REQUIRE(link->Activate());
    auto *info = XrdLinkCtl::fd2PollInfo(sockets[0]);
    REQUIRE(info && info->Poller->Enable(*info));
    decision.enabled.store(true, std::memory_order_release);
    REQUIRE(close(sockets[1]) == 0);

    decision.closeReturned.Wait();
    protocol.recycled.Wait();
    REQUIRE(decision.closeResult == 0);
    REQUIRE(!decision.recycledInline);
    REQUIRE(protocol.recycleCount == 1);
    while (XrdLinkCtl::fd2link(sockets[0]) != nullptr)
      std::this_thread::yield();
  );
}

// User: a protocol's fatal-close callback is still inspecting its state when
// an administrator closes the same connection. Stock reproduced against
// upstream 5b716c84a: Close recycles the protocol before its callback returns.
// The deferred job must pin that callback storage until the decision completes.
TEST(ServerLoops, CloseCallbackStorageSurvivesAConcurrentClose)
{
  FINISH_WITHIN(10,
    XrdInet network(&XrdGlobal::Log);
    XrdGlobal::XrdNetTCP = &network;
    REQUIRE(XrdLinkCtl::Setup(1024, 0));
    REQUIRE(XrdPoll::Setup(1024));
    XrdGlobal::Sched.setParms(1, 1, 1, 0);
    XrdGlobal::Sched.Start();

    int sockets[2];
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    XrdNetAddr peer;
    REQUIRE(peer.Set(sockets[0]) == nullptr);
    XrdLink *link = XrdLinkCtl::Alloc(peer);
    REQUIRE(link != nullptr);
    ReadingProtocol protocol;
    CloseDecision decision(true, link, true);
    link->setProtocol(&protocol);
    REQUIRE(XrdLinkCtl::RegisterCloseRequestCb(
      link, &protocol, CloseDecision::Decide, &decision));
    REQUIRE(link->Activate());
    auto *info = XrdLinkCtl::fd2PollInfo(sockets[0]);
    REQUIRE(info && info->Poller->Enable(*info));
    decision.enabled.store(true, std::memory_order_release);
    REQUIRE(close(sockets[1]) == 0);
    decision.entered.Wait();

    std::atomic<int> closeResult{-1};
    std::thread closer([&] { closeResult = link->Close(); });
    while (closeResult == -1)
    {
      if (ActivityWaitHead(link) != nullptr) break;
      std::this_thread::yield();
    }
    REQUIRE(closeResult == -1);
    REQUIRE(protocol.recycleCount == 0);

    decision.release.Post();
    decision.called.Wait();
    closer.join();
    auto *drained = new XrdSysSemaphore(0);
    XrdGlobal::Sched.Schedule(new Job([drained] { drained->Post(); }));
    drained->Wait();
    REQUIRE(closeResult == 0);
    REQUIRE(decision.calls == 1);
    REQUIRE(protocol.recycleCount == 1);
    REQUIRE(XrdLinkCtl::fd2link(sockets[0]) == nullptr);
  );
}

// User: fatal cleanup waits for another user of a connection while a competing
// closer releases and reuses its descriptor slot. Branch guard: pristine
// upstream fails this forced seam: its raw Close resumes after Serialize and
// recycles the replacement. The deferred path must recheck its generation.
// The harness performs the winning close/unhook/reallocation steps under the
// same operation lock to force this seam; it is not a separate field repro.
TEST(ServerLoops, FatalTerminationRechecksGenerationAfterSerialization)
{
  FINISH_WITHIN(10,
    XrdInet network(&XrdGlobal::Log);
    XrdGlobal::XrdNetTCP = &network;
    REQUIRE(XrdLinkCtl::Setup(1024, 0));
    REQUIRE(XrdPoll::Setup(1024));
    XrdGlobal::Sched.setParms(1, 1, 1, 0);
    XrdGlobal::Sched.Start();

    int first[2];
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, first) == 0);
    XrdNetAddr peer;
    REQUIRE(peer.Set(first[0]) == nullptr);
    XrdLink *oldLink = XrdLinkCtl::Alloc(peer);
    REQUIRE(oldLink != nullptr);
    const unsigned int oldInstance = oldLink->Inst();
    ReadingProtocol oldProtocol;
    oldLink->setProtocol(&oldProtocol);
    REQUIRE(oldLink->Activate());
    auto *oldInfo = XrdLinkCtl::fd2PollInfo(first[0]);
    REQUIRE(oldInfo && oldInfo->Poller->Enable(*oldInfo));
    oldLink->setRef(1);
    REQUIRE(close(first[1]) == 0);

    while (!SerializationWaiters(oldLink)) std::this_thread::yield();
    const int reusedFD = first[0];
    oldLink->Hold(true);
    // Wake the fatal Close, but retain opMutex until this slot represents the
    // replacement connection. It must validate again after reacquiring it.
    oldLink->setRef(-1);
    REQUIRE(close(reusedFD) == 0);
    XrdLinkCtl::Unhook(reusedFD);

    int replacement[2];
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, replacement) == 0);
    if (replacement[1] == reusedFD)
      std::swap(replacement[0], replacement[1]);
    if (replacement[0] != reusedFD)
    {
      REQUIRE(dup2(replacement[0], reusedFD) == reusedFD);
      REQUIRE(close(replacement[0]) == 0);
      replacement[0] = reusedFD;
    }
    REQUIRE(peer.Set(replacement[0]) == nullptr);
    XrdLink *newLink = XrdLinkCtl::Alloc(peer);
    REQUIRE(newLink == oldLink);
    const unsigned int newInstance = newLink->Inst();
    REQUIRE(newInstance != oldInstance);
    ReadingProtocol newProtocol;
    newLink->setProtocol(&newProtocol);
    oldLink->Hold(false);

    REQUIRE(newLink->Activate());
    auto *newInfo = XrdLinkCtl::fd2PollInfo(replacement[0]);
    REQUIRE(newInfo && newInfo->Poller);

    XrdSysSemaphore drained(0);
    Job marker([&] { drained.Post(); });
    XrdGlobal::Sched.Schedule(&marker);
    drained.Wait();
    REQUIRE(newLink->Inst() == newInstance);
    REQUIRE(newProtocol.recycleCount == 0);
    REQUIRE(XrdLinkCtl::fd2link(replacement[0]) == newLink);
    REQUIRE(newLink->Close() == 0);
    REQUIRE(newProtocol.recycleCount == 1);
    REQUIRE(XrdLinkCtl::fd2link(replacement[0]) == nullptr);
    REQUIRE(close(replacement[1]) == 0);
  );
}

#ifdef __linux__
// User: an ordinary readable event waits behind scheduler work while an
// administrator closes that connection and the OS reuses its descriptor for a
// new client. The delayed job must not consume or close the replacement.
// Stock reproduced against upstream 5b716c84a and the mixed-terminal-only
// branch revision: the raw embedded-link job processes the replacement.
// The replacement becomes readable and is itself replaced before either job
// runs. The cached and one-off jobs must retain their separate generations;
// neither may dispatch the final client occupying the same slot.
TEST(ServerLoops, DeferredReadableEventCannotRunAgainstReplacementConnection)
{
  FINISH_WITHIN(10,
    XrdInet network(&XrdGlobal::Log);
    XrdGlobal::XrdNetTCP = &network;
    REQUIRE(XrdLinkCtl::Setup(1024, 0));
    REQUIRE(XrdPoll::Setup(1024));
    XrdSysSemaphore blocked(0), release(0), drained(0);
    Job blocker([&] { blocked.Post(); release.Wait(); });
    Job marker([&] { drained.Post(); });
    XrdGlobal::Sched.setParms(1, 1, 1, 0);
    XrdGlobal::Sched.Schedule(&blocker);
    XrdGlobal::Sched.Start();
    blocked.Wait();

    int first[2];
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, first) == 0);
    XrdNetAddr peer;
    REQUIRE(peer.Set(first[0]) == nullptr);
    XrdLink *oldLink = XrdLinkCtl::Alloc(peer);
    REQUIRE(oldLink != nullptr);
    const unsigned int oldInstance = oldLink->Inst();
    ReadingProtocol oldProtocol;
    oldLink->setProtocol(&oldProtocol);
    REQUIRE(oldLink->Activate());
    auto *oldInfo = XrdLinkCtl::fd2PollInfo(first[0]);
    REQUIRE(oldInfo && oldInfo->Poller->Enable(*oldInfo));
    auto *poller = static_cast<XrdPollE *>(oldInfo->Poller);
    REQUIRE(write(first[1], "q", 1) == 1);
    while (FenceAndIsEnabled(*poller, *oldInfo)) {}

    const int reusedFD = first[0];
    REQUIRE(oldLink->Close() == 0);
    REQUIRE(oldProtocol.recycleCount == 1);
    REQUIRE(close(first[1]) == 0);

    int overlap[2];
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, overlap) == 0);
    if (overlap[1] == reusedFD)
      std::swap(overlap[0], overlap[1]);
    if (overlap[0] != reusedFD)
    {
      REQUIRE(dup2(overlap[0], reusedFD) == reusedFD);
      REQUIRE(close(overlap[0]) == 0);
      overlap[0] = reusedFD;
    }
    REQUIRE(peer.Set(overlap[0]) == nullptr);
    XrdLink *overlapLink = XrdLinkCtl::Alloc(peer);
    REQUIRE(overlapLink == oldLink && overlapLink->Inst() != oldInstance);
    const unsigned int overlapInstance = overlapLink->Inst();
    ReadingProtocol overlapProtocol;
    overlapLink->setProtocol(&overlapProtocol);
    REQUIRE(overlapLink->Activate());
    auto *overlapInfo = XrdLinkCtl::fd2PollInfo(overlap[0]);
    REQUIRE(overlapInfo && overlapInfo->Poller == poller);
    REQUIRE(overlapInfo->Poller->Enable(*overlapInfo));
    REQUIRE(write(overlap[1], "q", 1) == 1);
    while (FenceAndIsEnabled(*poller, *overlapInfo)) {}
    REQUIRE(overlapLink->Close() == 0);
    REQUIRE(overlapProtocol.recycleCount == 1);
    REQUIRE(close(overlap[1]) == 0);

    int replacement[2];
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, replacement) == 0);
    if (replacement[1] == reusedFD)
      std::swap(replacement[0], replacement[1]);
    if (replacement[0] != reusedFD)
    {
      REQUIRE(dup2(replacement[0], reusedFD) == reusedFD);
      REQUIRE(close(replacement[0]) == 0);
      replacement[0] = reusedFD;
    }
    REQUIRE(peer.Set(replacement[0]) == nullptr);
    XrdLink *newLink = XrdLinkCtl::Alloc(peer);
    REQUIRE(newLink == oldLink && newLink->Inst() != overlapInstance);
    ReadingProtocol newProtocol;
    newLink->setProtocol(&newProtocol);
    REQUIRE(newLink->Activate());
    REQUIRE(write(replacement[1], "q", 1) == 1);

    XrdGlobal::Sched.Schedule(&marker);
    release.Post();
    drained.Wait();
    REQUIRE(newProtocol.receiveCount == 0);
    REQUIRE(newProtocol.recycleCount == 0);
    REQUIRE(XrdLinkCtl::fd2link(replacement[0]) == newLink);
    REQUIRE(newLink->Close() == 0);
    REQUIRE(close(replacement[1]) == 0);
  );
}
#endif

// User: the idle scanner or a remote termination request disables an old
// connection, but its terminating protocol waits behind busy scheduler work.
// If another closer retires that connection and the descriptor is reused, the
// delayed dispatch must not consume a request from the replacement client.
// Stock reproduced against upstream 5b716c84a: Disable(etxt) queues the raw
// embedded link, which processes the replacement after the slot is reused.
TEST(ServerLoops, DeferredDisableCannotDispatchReplacementConnection)
{
  FINISH_WITHIN(10,
    XrdInet network(&XrdGlobal::Log);
    XrdGlobal::XrdNetTCP = &network;
    REQUIRE(XrdLinkCtl::Setup(1024, 0));
    REQUIRE(XrdPoll::Setup(1024));
    XrdSysSemaphore blocked(0), release(0), drained(0);
    Job blocker([&] { blocked.Post(); release.Wait(); });
    Job marker([&] { drained.Post(); });
    XrdGlobal::Sched.setParms(1, 1, 1, 0);
    XrdGlobal::Sched.Schedule(&blocker);
    XrdGlobal::Sched.Start();
    blocked.Wait();

    int first[2];
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, first) == 0);
    XrdNetAddr peer;
    REQUIRE(peer.Set(first[0]) == nullptr);
    XrdLink *oldLink = XrdLinkCtl::Alloc(peer);
    REQUIRE(oldLink != nullptr);
    const unsigned int oldInstance = oldLink->Inst();
    ReadingProtocol oldProtocol;
    oldLink->setProtocol(&oldProtocol);
    REQUIRE(oldLink->Activate());
    auto *oldInfo = XrdLinkCtl::fd2PollInfo(first[0]);
    REQUIRE(oldInfo && oldInfo->Poller->Enable(*oldInfo));

    oldLink->Hold(true);
    oldInfo->Poller->Disable(*oldInfo, "idle timeout");
    oldLink->Hold(false);

    const int reusedFD = first[0];
    REQUIRE(oldLink->Close() == 0);
    REQUIRE(oldProtocol.recycleCount == 1);
    REQUIRE(close(first[1]) == 0);

    int replacement[2];
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, replacement) == 0);
    if (replacement[1] == reusedFD)
      std::swap(replacement[0], replacement[1]);
    if (replacement[0] != reusedFD)
    {
      REQUIRE(dup2(replacement[0], reusedFD) == reusedFD);
      REQUIRE(close(replacement[0]) == 0);
      replacement[0] = reusedFD;
    }
    REQUIRE(peer.Set(replacement[0]) == nullptr);
    XrdLink *newLink = XrdLinkCtl::Alloc(peer);
    REQUIRE(newLink == oldLink && newLink->Inst() != oldInstance);
    ReadingProtocol newProtocol;
    newLink->setProtocol(&newProtocol);
    REQUIRE(newLink->Activate());
    REQUIRE(write(replacement[1], "q", 1) == 1);

    XrdGlobal::Sched.Schedule(&marker);
    release.Post();
    drained.Wait();
    REQUIRE(newProtocol.receiveCount == 0);
    REQUIRE(newProtocol.recycleCount == 0);
    REQUIRE(XrdLinkCtl::fd2link(replacement[0]) == newLink);
    REQUIRE(newLink->Close() == 0);
    REQUIRE(close(replacement[1]) == 0);
  );
}

#ifdef __linux__
// User: a connection is replaced while its deferred fatal-event job waits for
// a scheduler worker. That stale job must not close the next client occupying
// the same descriptor/link-table slot.
// Stock reproduced against upstream 5b716c84a: the forced lifecycle ordering
// closes the replacement.
// An empty pipe makes epoll report the same fatal-only mask deterministically;
// all scheduler, Close and link-slot reuse paths remain the production ones.
TEST(ServerLoops, DeferredFatalEventCannotCloseAReplacementConnection)
{
  FINISH_WITHIN(10,
    XrdInet network(&XrdGlobal::Log);
    XrdGlobal::XrdNetTCP = &network;
    REQUIRE(XrdLinkCtl::Setup(1024, 0));
    REQUIRE(XrdPoll::Setup(1024));
    XrdSysSemaphore blocked(0), release(0), drained(0);
    Job blocker([&] { blocked.Post(); release.Wait(); });
    Job marker([&] { drained.Post(); });
    XrdGlobal::Sched.setParms(1, 1, 1, 0);
    XrdGlobal::Sched.Schedule(&blocker);
    XrdGlobal::Sched.Start();
    blocked.Wait();

    int first[2];
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, first) == 0);
    XrdNetAddr peer;
    REQUIRE(peer.Set(first[0]) == nullptr);
    XrdLink *oldLink = XrdLinkCtl::Alloc(peer);
    REQUIRE(oldLink != nullptr);
    const int reusedFD = first[0];
    int fatalPipe[2];
    REQUIRE(pipe(fatalPipe) == 0);
    REQUIRE(dup2(fatalPipe[0], reusedFD) == reusedFD);
    REQUIRE(close(fatalPipe[0]) == 0);
    REQUIRE(close(first[1]) == 0);
    unsigned int oldInstance = oldLink->Inst();
    ReadingProtocol oldProtocol;
    oldLink->setProtocol(&oldProtocol);
    REQUIRE(oldLink->Activate());
    auto *oldInfo = XrdLinkCtl::fd2PollInfo(first[0]);
    REQUIRE(oldInfo && oldInfo->Poller->Enable(*oldInfo));
    auto *poller = static_cast<XrdPollE *>(oldInfo->Poller);
    REQUIRE(close(fatalPipe[1]) == 0);
    while (FenceAndIsEnabled(*poller, *oldInfo)) {}
    REQUIRE(oldLink->Close() == 0);
    oldProtocol.recycled.Wait();

    int replacement[2];
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, replacement) == 0);
    // Force the same descriptor even if a background thread opened another fd.
    if (replacement[1] == reusedFD) std::swap(replacement[0], replacement[1]);
    if (replacement[0] != reusedFD)
    {
      REQUIRE(dup2(replacement[0], reusedFD) == reusedFD);
      REQUIRE(close(replacement[0]) == 0);
      replacement[0] = reusedFD;
    }
    REQUIRE(peer.Set(replacement[0]) == nullptr);
    XrdLink *newLink = XrdLinkCtl::Alloc(peer);
    REQUIRE(newLink == oldLink && newLink->Inst() != oldInstance);
    ReadingProtocol newProtocol;
    newLink->setProtocol(&newProtocol);
    REQUIRE(newLink->Activate());
    auto *newInfo = XrdLinkCtl::fd2PollInfo(replacement[0]);
    REQUIRE(newInfo && newInfo->Poller == poller);

    // One worker preserves FIFO execution: marker completion proves the stale
    // fatal job ran, rather than merely showing that the new client got lucky.
    XrdGlobal::Sched.Schedule(&marker);
    release.Post();
    drained.Wait();
    REQUIRE(newProtocol.recycled.CondWait() == 0);
    REQUIRE(XrdLinkCtl::fd2link(replacement[0]) == newLink);
    REQUIRE(newInfo->Poller->Enable(*newInfo));
    REQUIRE(write(replacement[1], "q", 1) == 1);
    newProtocol.received.Wait();
    REQUIRE(close(replacement[1]) == 0);
    newProtocol.recycled.Wait();
  );
}

// User: fatal cleanup validates the old connection, but scheduler congestion
// delays its final close until after that descriptor is reused by a new client.
// Branch guard: pristine upstream passes because it has one queued link job;
// this catches an implementation which adds a validated wrapper but then puts
// the unguarded raw link back on the scheduler behind another blocked job.
TEST(ServerLoops, FatalCompletionCannotQueueAnUnguardedLinkAfterValidation)
{
  FINISH_WITHIN(10,
    XrdInet network(&XrdGlobal::Log);
    XrdGlobal::XrdNetTCP = &network;
    REQUIRE(XrdLinkCtl::Setup(1024, 0));
    REQUIRE(XrdPoll::Setup(1024));
    XrdSysSemaphore firstEntered(0), releaseFirst(0);
    XrdSysSemaphore secondEntered(0), releaseSecond(0), drained(0);
    Job firstBlocker([&] { firstEntered.Post(); releaseFirst.Wait(); });
    Job secondBlocker([&] { secondEntered.Post(); releaseSecond.Wait(); });
    Job marker([&] { drained.Post(); });
    XrdGlobal::Sched.setParms(1, 1, 1, 0);
    XrdGlobal::Sched.Schedule(&firstBlocker);
    XrdGlobal::Sched.Start();
    firstEntered.Wait();

    int first[2];
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, first) == 0);
    XrdNetAddr peer;
    REQUIRE(peer.Set(first[0]) == nullptr);
    XrdLink *oldLink = XrdLinkCtl::Alloc(peer);
    REQUIRE(oldLink != nullptr);
    const unsigned int oldInstance = oldLink->Inst();
    ReadingProtocol oldProtocol;
    oldLink->setProtocol(&oldProtocol);
    REQUIRE(oldLink->Activate());
    auto *oldInfo = XrdLinkCtl::fd2PollInfo(first[0]);
    REQUIRE(oldInfo && oldInfo->Poller->Enable(*oldInfo));
    auto *poller = static_cast<XrdPollE *>(oldInfo->Poller);
    REQUIRE(close(first[1]) == 0);
    while (FenceAndIsEnabled(*poller, *oldInfo)) {}

    // The fatal job precedes this blocker. A wrapper-plus-raw implementation
    // runs its wrapper first, then puts the raw link behind this blocker.
    XrdGlobal::Sched.Schedule(&secondBlocker);
    releaseFirst.Post();
    secondEntered.Wait();
    if (oldLink->Inst() == oldInstance) REQUIRE(oldLink->Close() == 0);
    oldProtocol.recycled.Wait();

    int replacement[2];
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, replacement) == 0);
    if (replacement[1] == first[0]) std::swap(replacement[0], replacement[1]);
    if (replacement[0] != first[0])
    {
      REQUIRE(dup2(replacement[0], first[0]) == first[0]);
      REQUIRE(close(replacement[0]) == 0);
      replacement[0] = first[0];
    }
    REQUIRE(peer.Set(replacement[0]) == nullptr);
    XrdLink *newLink = XrdLinkCtl::Alloc(peer);
    REQUIRE(newLink == oldLink && newLink->Inst() != oldInstance);
    ReadingProtocol newProtocol;
    newLink->setProtocol(&newProtocol);
    REQUIRE(newLink->Activate());
    auto *newInfo = XrdLinkCtl::fd2PollInfo(replacement[0]);
    REQUIRE(newInfo && newInfo->Poller == poller);

    XrdGlobal::Sched.Schedule(&marker);
    releaseSecond.Post();
    drained.Wait();
    REQUIRE(newProtocol.recycled.CondWait() == 0);
    REQUIRE(XrdLinkCtl::fd2link(replacement[0]) == newLink);
    REQUIRE(newInfo->Poller->Enable(*newInfo));
    REQUIRE(write(replacement[1], "q", 1) == 1);
    newProtocol.received.Wait();
    REQUIRE(close(replacement[1]) == 0);
    newProtocol.recycled.Wait();
  );
}
#endif
