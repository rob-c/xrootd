// Regressions for the portable server poll(2) loop and its queued jobs.
#include "XrdSys/XrdSysPthread.hh"
#include "Xrd/XrdPoll.hh"
#include "Xrd/XrdScheduler.hh"
#define private public
#include "Xrd/XrdPollPoll.hh"
#undef private
#include "Xrd/XrdInet.hh"
#define protected public
#include "Xrd/XrdLinkCtl.hh"
#undef protected
#include "Xrd/XrdProtocol.hh"
#include "XrdNet/XrdNetAddr.hh"
#include "XrdSys/XrdSysError.hh"

#include <gtest/gtest.h>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <dlfcn.h>
#include <fcntl.h>
#include <functional>
#include <poll.h>
#include <thread>
#include <sys/socket.h>
#include <unistd.h>

namespace XrdGlobal {
extern XrdSysError Log;
extern XrdScheduler Sched;
extern XrdInet *XrdNetTCP;
extern int devNull;
}

std::atomic<int> injectedFatalFD{-1};

// Preserve the real poll operation, then control one armed result. This gives
// the portable backend the same event and return boundary on every kernel.
#ifndef __APPLE__
thread_local bool pauseAfterMutexUnlock = false;
std::atomic<int> mutexUnlockStage{0};

extern "C" int pthread_mutex_unlock(pthread_mutex_t *mutex)
{
  using Unlock = int (*)(pthread_mutex_t *);
  static Unlock realUnlock =
    reinterpret_cast<Unlock>(dlsym(RTLD_NEXT, "pthread_mutex_unlock"));
  const int result = realUnlock(mutex);
  if (pauseAfterMutexUnlock)
  {
    pauseAfterMutexUnlock = false;
    mutexUnlockStage.store(1, std::memory_order_relaxed);
    while (mutexUnlockStage.load(std::memory_order_relaxed) != 2)
      std::this_thread::yield();
  }
  return result;
}

int TestPoll(struct pollfd *entries, nfds_t count, int timeout)
{
  using Poll = int (*)(struct pollfd *, nfds_t, int);
  static Poll realPoll = reinterpret_cast<Poll>(dlsym(RTLD_NEXT, "poll"));
  const int result = realPoll(entries, count, timeout);
  const int target = injectedFatalFD.load(std::memory_order_acquire);
  if (result > 0 && target >= 0)
    for (nfds_t index = 0; index < count; ++index)
      if (entries[index].fd == target)
      {
        int expected = target;
        if (!injectedFatalFD.compare_exchange_strong(
              expected, -1, std::memory_order_acq_rel)) break;
        for (nfds_t clear = 0; clear < count; ++clear)
          entries[clear].revents = 0;
        entries[index].revents = POLLHUP;
        return 1;
      }
  return result;
}

extern "C" int poll(struct pollfd *entries, nfds_t count, int timeout)
{
  return TestPoll(entries, count, timeout);
}
#endif

namespace {
[[maybe_unused]] bool IsEnabled(const std::atomic<bool> &enabled)
{
  return enabled.load(std::memory_order_acquire);
}

[[maybe_unused]] bool IsEnabled(const bool &enabled) { return enabled; }

template <class Info>
auto SetRegistration(Info *info, unsigned int value, int)
  -> decltype(info->Generation = value, void())
{
  info->Generation = value;
}

template <class Info>
void SetRegistration(Info *, unsigned int, long) {}

template <class Info>
auto Registration(Info *info, unsigned int, int) -> decltype(info->Generation)
{
  return info->Generation;
}

template <class Info>
unsigned int Registration(Info *, unsigned int fallback, long)
{
  return fallback;
}

template <class Link>
auto RunTerminate(Link *link, unsigned int instance, int)
  -> decltype(link->Terminate(instance), void())
{
  link->Terminate(instance);
}

template <class Link>
void RunTerminate(Link *, unsigned int, long) {}

void Check(bool ok, const char *expression, int line)
{
  if (ok) return;
  dprintf(STDERR_FILENO, "ServerPollPollTests.cc:%d: %s\n", line, expression);
  _exit(1);
}
#define REQUIRE(expression) Check((expression), #expression, __LINE__)
#define FINISH_WITHIN(seconds, ...) \
  ASSERT_EXIT(([] { alarm(seconds); __VA_ARGS__ _exit(0); }()), \
              ::testing::ExitedWithCode(0), "")

class Job : public XrdJob
{
public:
  explicit Job(std::function<void()> action)
    : XrdJob("poll loop proof"), action(action) {}
  void DoIt() override { action(); }
private:
  std::function<void()> action;
};

class ReadingProtocol : public XrdProtocol
{
public:
  ReadingProtocol() : XrdProtocol("poll loop protocol") {}
  void DoIt() override {}
  XrdProtocol *Match(XrdLink *) override { return nullptr; }
  int Process(XrdLink *link) override
  {
    char byte = 0;
    const int result = link->Recv(&byte, 1, 1000);
    REQUIRE(result == 1);
    REQUIRE(byte == 'q');
    ++processCount;
    return 1;
  }
  void Recycle(XrdLink *, int, const char *) override { ++recycleCount; }
  int Stats(char *, int, int = 0) override { return 0; }
  std::atomic<int> processCount{0}, recycleCount{0};
};

class PassiveProtocol : public ReadingProtocol
{
public:
  int Process(XrdLink *) override
  {
    ++processCount;
    return 1;
  }
};

class SwitchingProtocol : public PassiveProtocol
{
public:
  explicit SwitchingProtocol(XrdProtocol *next) : next(next) {}
  int Process(XrdLink *link) override
  {
    ++processCount;
    link->setProtocol(next);
    return 0;
  }
private:
  XrdProtocol *next;
};

template <class Link>
auto RunDispatch(Link *link, int) -> decltype(link->DoItPinned(1), void())
{
  link->DoItPinned(1);
}

template <class Link>
void RunDispatch(Link *link, long) { link->DoIt(); }

#ifndef __APPLE__
template <class Link>
auto RunProtocolSelectionOverlap(Link *link, int)
  -> decltype(link->DoItPinned(1), void())
{
  PassiveProtocol protocol;
  link->Instance = 1;
  link->LinkInfo.InUse = 1;
  link->LinkInfo.FD = link->PollInfo.FD = -1;
  SetRegistration(&link->PollInfo, 1, 0);
  link->setProtocol(&protocol, false);

  mutexUnlockStage.store(0, std::memory_order_relaxed);
  std::thread reader([&] {
    pauseAfterMutexUnlock = true;
    link->DoItPinned(1);
  });
  while (mutexUnlockStage.load(std::memory_order_relaxed) != 1)
    std::this_thread::yield();

  XrdLink *publicLink = static_cast<XrdLink *>(link);
  publicLink->Hold(true);
  REQUIRE(XrdPoll::Finish(link->PollInfo, "forced terminal overlap"));
  publicLink->Hold(false);
  mutexUnlockStage.store(2, std::memory_order_relaxed);
  reader.join();

  REQUIRE(protocol.processCount == 1);
  REQUIRE(link->Close() == 0);
}

template <class Link>
void RunProtocolSelectionOverlap(Link *, long) {}
#endif

struct CloseDecision
{
  static bool Decide(void *argument)
  {
    ++static_cast<CloseDecision *>(argument)->calls;
    return false;
  }
  std::atomic<int> calls{0};
};

XrdLink *AllocateLink(int fd, XrdNetAddr &peer, ReadingProtocol &protocol)
{
  REQUIRE(peer.Set(fd) == nullptr);
  XrdLink *link = XrdLinkCtl::Alloc(peer);
  REQUIRE(link != nullptr);
  link->setProtocol(&protocol);
  REQUIRE(link->Activate());
  return link;
}

void StartBlockedScheduler(XrdSysSemaphore &entered, XrdSysSemaphore &release)
{
  XrdGlobal::Sched.setParms(1, 1, 1, 0);
  XrdGlobal::Sched.Schedule(new Job([&] { entered.Post(); release.Wait(); }));
  XrdGlobal::Sched.Start();
  entered.Wait();
}
}

// User: a client disappears while an administrator or timeout thread closes
// the same connection. The portable poll loop used on macOS and other Unix
// systems can then stop accepting every client, because each side waits for a
// lock held by the other. Stock reproduced: POLLHUP calls Finish
// while PollMutex is held, and Finish waits for the link operation mutex.
TEST(ServerPollPollLoops, FatalEventCannotInvertTheCloseAndPollLocks)
{
  FINISH_WITHIN(5,
    XrdInet network(&XrdGlobal::Log);
    XrdGlobal::XrdNetTCP = &network;
    REQUIRE(XrdLinkCtl::Setup(1024, 0));
    REQUIRE(XrdPoll::Setup(1024));
    XrdSysSemaphore blocked(0), release(0);
    StartBlockedScheduler(blocked, release);

    int sockets[2];
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    XrdNetAddr peer;
    ReadingProtocol protocol;
    REQUIRE(peer.Set(sockets[0]) == nullptr);
    XrdLink *link = XrdLinkCtl::Alloc(peer);
    REQUIRE(link != nullptr);
    int fatalPipe[2];
    REQUIRE(pipe(fatalPipe) == 0);
    REQUIRE(dup2(fatalPipe[0], sockets[0]) == sockets[0]);
    REQUIRE(close(fatalPipe[0]) == 0);
    REQUIRE(close(sockets[1]) == 0);
    link->setProtocol(&protocol);
    REQUIRE(link->Activate());
    XrdPollInfo *info = XrdLinkCtl::fd2PollInfo(sockets[0]);
    REQUIRE(info && info->Poller->Enable(*info));
    while (!IsEnabled(info->isEnabled))
      std::this_thread::yield();

    link->Hold(true);
    injectedFatalFD.store(sockets[0], std::memory_order_release);
    REQUIRE(close(fatalPipe[1]) == 0);
    // Fatal selection disables the registration before queuing cleanup. Stock
    // cannot publish this state because its poll thread is blocked in Finish.
    while (IsEnabled(info->isEnabled))
      std::this_thread::yield();
    auto *poller = reinterpret_cast<XrdPollPoll *>(info->Poller);
    poller->PollMutex.Lock();
    poller->PollMutex.UnLock();
    REQUIRE(link->Close() == 0);
    link->Hold(false);
    REQUIRE(protocol.recycleCount == 1);
    release.Post();
  );
}

// User: after enough connections for the unsigned generation counter to wrap,
// a client disconnect can leave a fatal cleanup job waiting behind busy server
// work. An explicit close also uses zero as its inactive sentinel; the delayed
// job must not mistake that closed slot for its original generation and invoke
// a stale close callback. Branch guard: this bootstraps the generation-zero repair;
// stock has no deferred fatal job, while the branch without the closePending
// check calls the retired connection's callback after Close has returned.
TEST(ServerPollPollLoops, DeferredGenerationZeroFatalJobRejectsClosedLink)
{
  FINISH_WITHIN(10,
    XrdInet network(&XrdGlobal::Log);
    XrdGlobal::XrdNetTCP = &network;
    REQUIRE(XrdLinkCtl::Setup(1024, 0));
    REQUIRE(XrdPoll::Setup(1024));
    XrdSysSemaphore blocked(0), release(0), drained(0);
    StartBlockedScheduler(blocked, release);

    int sockets[2];
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    XrdNetAddr peer;
    REQUIRE(peer.Set(sockets[0]) == nullptr);
    XrdLink *link = XrdLinkCtl::Alloc(peer);
    REQUIRE(link != nullptr);
    auto *implementation = static_cast<XrdLinkXeq *>(link);
    implementation->Instance = 0;
    SetRegistration(&implementation->PollInfo, 0, 0);
    ReadingProtocol protocol;
    CloseDecision decision;
    link->setProtocol(&protocol);
    REQUIRE(XrdLinkCtl::RegisterCloseRequestCb(
      link, &protocol, CloseDecision::Decide, &decision));

    int fatalPipe[2];
    REQUIRE(pipe(fatalPipe) == 0);
    REQUIRE(dup2(fatalPipe[0], sockets[0]) == sockets[0]);
    REQUIRE(close(fatalPipe[0]) == 0);
    REQUIRE(close(sockets[1]) == 0);
    REQUIRE(link->Activate());
    XrdPollInfo *info = XrdLinkCtl::fd2PollInfo(sockets[0]);
    REQUIRE(info && info->Poller->Enable(*info));
    while (!IsEnabled(info->isEnabled))
      std::this_thread::yield();
    injectedFatalFD.store(sockets[0], std::memory_order_release);
    REQUIRE(close(fatalPipe[1]) == 0);
    while (IsEnabled(info->isEnabled))
      std::this_thread::yield();
    auto *poller = reinterpret_cast<XrdPollPoll *>(info->Poller);
    poller->PollMutex.Lock();
    poller->PollMutex.UnLock();

    REQUIRE(link->Close() == 0);
    REQUIRE(protocol.recycleCount == 1);
    REQUIRE(decision.calls == 0);
    XrdGlobal::Sched.Schedule(new Job([&] { drained.Post(); }));
    release.Post();
    drained.Wait();
    REQUIRE(decision.calls == 0);
    REQUIRE(protocol.recycleCount == 1);
    REQUIRE(link->getProtocol() == nullptr);
    RunTerminate(implementation, 0, 0);
    REQUIRE(decision.calls == 0);
    REQUIRE(protocol.recycleCount == 1);
    REQUIRE(link->getProtocol() == nullptr);
  );
}

#ifndef __APPLE__
// User: a server shutdown replaces an active socket while poll(2) is returning
// its readiness. A delayed job must retain the poll registration's generation,
// because Shutdown sets the live Link instance to zero before detaching it.
// Stock reproduced: upstream queues the raw embedded link; a branch
// mutation which captures Link::Inst() also dispatches the shutdown connection.
// The forced Linux build supplies the controlled poll boundary. Darwin does
// not guarantee that shutdown plus dup2 wakes an already blocked poll call.
TEST(ServerPollPollLoops, ReadinessSelectedAcrossShutdownUsesRegistrationGeneration)
{
  FINISH_WITHIN(10,
    XrdInet network(&XrdGlobal::Log);
    XrdGlobal::XrdNetTCP = &network;
    REQUIRE(XrdLinkCtl::Setup(1024, 0));
    REQUIRE(XrdPoll::Setup(1024));
    XrdSysSemaphore blocked(0), release(0), drained(0);
    StartBlockedScheduler(blocked, release);

    int sockets[2];
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    XrdNetAddr peer;
    REQUIRE(peer.Set(sockets[0]) == nullptr);
    XrdLink *link = XrdLinkCtl::Alloc(peer);
    REQUIRE(link != nullptr);
    PassiveProtocol protocol;
    link->setProtocol(&protocol);
    REQUIRE(link->Activate());
    XrdPollInfo *info = XrdLinkCtl::fd2PollInfo(sockets[0]);
    REQUIRE(info && info->Poller->Enable(*info));
    while (!IsEnabled(info->isEnabled))
      std::this_thread::yield();
    const unsigned int instance = link->Inst();
    const unsigned int registration = Registration(info, instance, 0);
    REQUIRE(registration == link->Inst());

    XrdGlobal::devNull = open("/dev/null", O_RDONLY);
    REQUIRE(XrdGlobal::devNull >= 0);
    auto *poller = reinterpret_cast<XrdPollPoll *>(info->Poller);
    poller->PollMutex.Lock();
    REQUIRE(write(sockets[1], "q", 1) == 1);
    link->Shutdown(true);
    REQUIRE(link->Inst() == 0);
    REQUIRE(Registration(info, registration, 0) == registration);
    poller->PollMutex.UnLock();
    while (IsEnabled(info->isEnabled))
      std::this_thread::yield();
    poller->PollMutex.Lock();
    poller->PollMutex.UnLock();

    XrdGlobal::Sched.Schedule(new Job([&] { drained.Post(); }));
    release.Post();
    drained.Wait();
    REQUIRE(protocol.processCount == 0);
    REQUIRE(protocol.recycleCount == 0);
    REQUIRE(link->Close() == 0);
    REQUIRE(protocol.recycleCount == 1);
    REQUIRE(close(sockets[1]) == 0);
  );
}
#endif

#ifndef __APPLE__
// User: a readable request is admitted just before an independent fatal event
// retires the connection. If that worker is descheduled after admission, it
// must still call the protocol it admitted rather than the newly installed
// termination protocol. Branch guard: this bootstraps activity-pinned capture;
// the branch without a locked snapshot silently skips the user's request.
TEST(ServerPollPollLoops, AdmittedReadinessKeepsItsProtocolAcrossFatalSelection)
{
  FINISH_WITHIN(10,
    XrdLinkXeq link;
    RunProtocolSelectionOverlap(&link, 0);
  );
}
#endif

// User: the protocol loader recognizes a connection and hands it to the real
// protocol while the scheduler's sticky optimization can consume another
// buffered request immediately. Stock upstream 5b716c84a passes by observing
// the replacement on the next iteration. Branch guard: an admitted-protocol
// snapshot must be refreshed under the link lock between sticky iterations.
TEST(ServerPollPollLoops, StickyDispatchObservesAProtocolHandoff)
{
  FINISH_WITHIN(5,
    XrdLinkXeq link;
    link.Instance = 1;
    link.LinkInfo.InUse = 1;
    link.LinkInfo.FD = link.PollInfo.FD = -1;
    SetRegistration(&link.PollInfo, 1, 0);
    PassiveProtocol established;
    SwitchingProtocol loader(&established);
    link.setProtocol(&loader, false);

    RunDispatch(&link, 0);

    REQUIRE(loader.processCount == 1);
    REQUIRE(established.processCount == 1);
  );
}

// User: poll(2) reports a process-level failure while scheduler workers are
// busy, then the affected descriptor is accepted for a new client. A raw job
// for the retired connection can consume that new client's first request or
// recycle it. Stock reproduced: Restart leaves inQ/isEnabled set
// and queues the reusable embedded link rather than a generation-pinned job.
TEST(ServerPollPollLoops, RestartCannotDispatchAReusedConnection)
{
  FINISH_WITHIN(10,
    XrdInet network(&XrdGlobal::Log);
    XrdGlobal::XrdNetTCP = &network;
    REQUIRE(XrdLinkCtl::Setup(1024, 0));
    REQUIRE(XrdPoll::Setup(1024));
    XrdSysSemaphore blocked(0), release(0), drained(0);
    StartBlockedScheduler(blocked, release);

    int first[2];
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, first) == 0);
    XrdNetAddr peer;
    ReadingProtocol oldProtocol;
    XrdLink *oldLink = AllocateLink(first[0], peer, oldProtocol);
    const int reusedFD = first[0];
    const unsigned int oldInstance = oldLink->Inst();
    XrdPollInfo *oldInfo = XrdLinkCtl::fd2PollInfo(reusedFD);
    REQUIRE(oldInfo && oldInfo->Poller->Enable(*oldInfo));
    while (!IsEnabled(oldInfo->isEnabled))
      std::this_thread::yield();

    auto *poller = reinterpret_cast<XrdPollPoll *>(oldInfo->Poller);
    poller->Restart(EIO);
    const bool queueCleared = !oldInfo->inQ;
    const bool disabled = !IsEnabled(oldInfo->isEnabled);
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
    ReadingProtocol replacementProtocol;
    XrdLink *replacementLink =
      AllocateLink(replacement[0], peer, replacementProtocol);
    REQUIRE(replacementLink == oldLink);
    REQUIRE(replacementLink->Inst() != oldInstance);
    REQUIRE(write(replacement[1], "q", 1) == 1);

    XrdGlobal::Sched.Schedule(new Job([&] { drained.Post(); }));
    release.Post();
    drained.Wait();
    REQUIRE(replacementProtocol.processCount == 0);
    REQUIRE(replacementProtocol.recycleCount == 0);
    REQUIRE(XrdLinkCtl::fd2link(reusedFD) == replacementLink);
    REQUIRE(queueCleared);
    REQUIRE(disabled);
    REQUIRE(replacementLink->Close() == 0);
    REQUIRE(close(replacement[1]) == 0);
  );
}
