// Public IOEvents regressions. These tests also compile against upstream;
// no private layout changes or production test hooks are required.
#include "XrdSys/XrdSysIOEvents.hh"

#include <gtest/gtest.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <climits>
#include <ctime>
#include <memory>
#include <new>
#include <thread>
#include <vector>
#include <cstdlib>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>
#ifdef __linux__
#include <dlfcn.h>

thread_local bool recordNextUnlock = false;
thread_local pthread_mutex_t *recordedMutex = nullptr;
thread_local pthread_mutex_t *pauseAfterUnlock = nullptr;
thread_local pthread_mutex_t *pauseBeforeMutex = nullptr;
thread_local bool recordMutexLocks = false;
thread_local pthread_mutex_t *recordedLocks[8];
thread_local int recordedLockCount = 0;
thread_local bool recordedLockOverflow = false;
std::atomic<int> enableUnlockStage{0};
std::atomic<int> mutexLockStage{0};

extern "C" int pthread_mutex_lock(pthread_mutex_t *mutex)
{
  using Lock = int (*)(pthread_mutex_t *);
  static Lock realLock =
    reinterpret_cast<Lock>(dlsym(RTLD_NEXT, "pthread_mutex_lock"));
  if (recordMutexLocks)
  {
    int i = 0;
    while (i < recordedLockCount && recordedLocks[i] != mutex) ++i;
    if (i == recordedLockCount)
    {
      if (recordedLockCount < 8) recordedLocks[recordedLockCount++] = mutex;
      else recordedLockOverflow = true;
    }
  }
  if (pauseBeforeMutex == mutex)
  {
    pauseBeforeMutex = nullptr;
    mutexLockStage.store(1, std::memory_order_release);
    while (mutexLockStage.load(std::memory_order_acquire) != 2)
      std::this_thread::yield();
  }
  return realLock(mutex);
}

extern "C" int pthread_mutex_unlock(pthread_mutex_t *mutex)
{
  using Unlock = int (*)(pthread_mutex_t *);
  static Unlock realUnlock =
    reinterpret_cast<Unlock>(dlsym(RTLD_NEXT, "pthread_mutex_unlock"));
  const int result = realUnlock(mutex);
  if (recordNextUnlock)
  {
    recordedMutex = mutex;
    recordNextUnlock = false;
  }
  if (pauseAfterUnlock == mutex)
  {
    pauseAfterUnlock = nullptr;
    enableUnlockStage.store(1, std::memory_order_release);
    while (enableUnlockStage.load(std::memory_order_acquire) != 2)
      std::this_thread::yield();
  }
  return result;
}
#endif

#if defined(__linux__) && defined(XRD_SYS_IOEVENTS_FORCE_POLL)
namespace {
thread_local bool guardNextChannelAllocation = false;
std::atomic<void *> guardedChannelAllocation{nullptr};
thread_local bool pauseBeforeNextWrite = false;
std::atomic<int> pipeWriteStage{0};
}

extern "C" ssize_t write(int fd, const void *buffer, size_t size)
{
  using Write = ssize_t (*)(int, const void *, size_t);
  static Write realWrite = reinterpret_cast<Write>(dlsym(RTLD_NEXT, "write"));
  if (pauseBeforeNextWrite)
  {
    pauseBeforeNextWrite = false;
    pipeWriteStage.store(1, std::memory_order_release);
    while (pipeWriteStage.load(std::memory_order_acquire) != 2)
      std::this_thread::yield();
  }
  return realWrite(fd, buffer, size);
}

void *operator new(std::size_t size)
{
  if (guardNextChannelAllocation)
  {
    guardNextChannelAllocation = false;
    const std::size_t page = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
    if (size > page) throw std::bad_alloc();
    void *memory = mmap(nullptr, page, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (memory == MAP_FAILED) throw std::bad_alloc();
    guardedChannelAllocation.store(memory, std::memory_order_relaxed);
    return memory;
  }
  if (void *memory = std::malloc(size)) return memory;
  throw std::bad_alloc();
}

void *operator new(std::size_t size, const std::nothrow_t &) noexcept
{
  return std::malloc(size ? size : 1);
}

void operator delete(void *memory, const std::nothrow_t &) noexcept
{
  std::free(memory);
}

void operator delete(void *memory) noexcept
{
  if (memory && memory == guardedChannelAllocation.load(std::memory_order_relaxed))
  {
    if (mprotect(memory, static_cast<std::size_t>(sysconf(_SC_PAGESIZE)),
                 PROT_NONE)) _exit(90);
    return;
  }
  std::free(memory);
}

void operator delete(void *memory, std::size_t) noexcept
{
  operator delete(memory);
}
#endif

namespace {
using namespace XrdSys::IOEvents;

void Check(bool condition) { if (!condition) _exit(1); }

class SocketPair
{
public:
  SocketPair() { Check(socketpair(AF_UNIX, SOCK_STREAM, 0, fd) == 0); }
  ~SocketPair() { for (int value : fd) if (value >= 0) close(value); }
  void Write() { Check(write(fd[1], "x", 1) == 1); }
  void Hangup() { close(fd[1]); fd[1] = -1; }
  int fd[2];
};

Poller *MakePoller()
{
  int error = 0;
  Poller *poller = Poller::Create(error);
  Check(poller && !error);
  return poller;
}

struct RecordingCallback : CallBack
{
  XrdSysSemaphore done{0};
  std::atomic<int> events{0}, errors{0};
  bool Event(Channel *, void *, int flags) override
  {
    events = flags;
    done.Post();
    return false;
  }
  void Fatal(Channel *, void *, int error, const char *) override
  {
    errors = error;
    done.Post();
  }
};

#if defined(__linux__) && defined(XRD_SYS_IOEVENTS_FORCE_POLL)
enum class ModifyOperation { EnableWrite, DisableRead };
int InspectPollEntry(Channel *channel);

void RunModifyDeleteRace(ModifyOperation operation)
{
  alarm(5);
  Poller *poller = MakePoller();
  SocketPair socket;
  RecordingCallback callback;
  guardNextChannelAllocation = true;
  Channel *channel = new Channel(poller, socket.fd[0], &callback);
  Check(guardedChannelAllocation.load(std::memory_order_relaxed) == channel);
  Check(channel->Enable(Channel::readEvents));
  CallBack *savedCallback;
  void *savedArgument;
  recordNextUnlock = true;
  channel->GetCallBack(&savedCallback, &savedArgument);
  Check(recordedMutex && savedCallback == &callback);
  pthread_mutex_t *channelMutex = recordedMutex;
  enableUnlockStage.store(0, std::memory_order_release);

  std::thread modifier([&] {
    // PollPoll::Modify drops chMutex before sending its command. Pause at that
    // exact unlock so Delete can revoke and guard the Channel allocation.
    pauseAfterUnlock = channelMutex;
    const bool result = operation == ModifyOperation::EnableWrite
      ? channel->Enable(Channel::writeEvents)
      : channel->Disable(Channel::readEvents);
    Check(result);
  });
  while (enableUnlockStage.load(std::memory_order_acquire) != 1)
    std::this_thread::yield();
  channel->Delete();
  enableUnlockStage.store(2, std::memory_order_release);
  modifier.join();
  delete poller;
  _exit(0);
}

void RunStaleModifyReplacementRace()
{
  alarm(5);
  Poller *poller = MakePoller();
  SocketPair originalSocket, replacementSocket, barrierSocket;
  RecordingCallback originalCallback, replacementCallback, barrierCallback;
  guardNextChannelAllocation = true;
  Channel *original = new Channel(
    poller, originalSocket.fd[0], &originalCallback);
  Check(original->Enable(Channel::readEvents));
  const int originalEntry = InspectPollEntry(original);
  CallBack *savedCallback;
  void *savedArgument;
  recordNextUnlock = true;
  original->GetCallBack(&savedCallback, &savedArgument);
  Check(recordedMutex && savedCallback == &originalCallback);
  pthread_mutex_t *channelMutex = recordedMutex;
  enableUnlockStage.store(0, std::memory_order_release);

  std::thread modifier([&] {
    // Hold MdFD after it has captured this Channel's slot and event mask but
    // before it enters the command pipe.
    pauseAfterUnlock = channelMutex;
    Check(original->Disable(Channel::readEvents));
  });
  while (enableUnlockStage.load(std::memory_order_acquire) != 1)
    std::this_thread::yield();
  original->Delete();

  Channel *replacement = new Channel(
    poller, replacementSocket.fd[0], &replacementCallback);
  Check(replacement->Enable(Channel::readEvents));
  Check(InspectPollEntry(replacement) == originalEntry);
  enableUnlockStage.store(2, std::memory_order_release);
  modifier.join();

  // A synchronous Include is a pipe-order barrier for the delayed MdFD. On
  // stock it has now disabled the replacement's reused slot.
  Channel *barrier = new Channel(poller, barrierSocket.fd[0], &barrierCallback);
  Check(barrier->Enable(Channel::readEvents));
  replacementSocket.Write();
  replacementCallback.done.Wait();
  Check(replacementCallback.events & CallBack::ReadyToRead);

  replacement->Delete();
  barrier->Delete();
  delete poller;
  _exit(0);
}

void RunFirstEnableDeleteRace()
{
  alarm(5);
  Poller *poller = MakePoller();
  SocketPair socket, replacementSocket, fenceSocket;
  RecordingCallback callback, replacementCallback, fenceCallback;
  guardNextChannelAllocation = true;
  Channel *channel = new Channel(poller, socket.fd[0], &callback);
  Check(guardedChannelAllocation.load(std::memory_order_relaxed) == channel);
  CallBack *savedCallback;
  void *savedArgument;
  recordNextUnlock = true;
  channel->GetCallBack(&savedCallback, &savedArgument);
  Check(recordedMutex && savedCallback == &callback);
  pthread_mutex_t *channelMutex = recordedMutex;
  enableUnlockStage.store(0, std::memory_order_release);

  std::thread enabler([&] {
    // Pause the first Include immediately after PollPoll drops chMutex.
    pauseAfterUnlock = channelMutex;
    Check(channel->Enable(Channel::writeEvents));
  });
  while (enableUnlockStage.load(std::memory_order_acquire) != 1)
    std::this_thread::yield();
  channel->Delete();
  Channel *replacement = new Channel(
    poller, replacementSocket.fd[0], &replacementCallback);
  Check(replacement->Enable(Channel::readEvents));
  enableUnlockStage.store(2, std::memory_order_release);
  enabler.join();

  // Fence the stale MiFD, then prove it did not alter the reused slot.
  Channel *fence = new Channel(poller, fenceSocket.fd[0], &fenceCallback);
  Check(fence->Enable(Channel::readEvents));
  replacementSocket.Write();
  replacementCallback.done.Wait();
  Check(replacementCallback.events.load() & CallBack::ReadyToRead);
  _exit(0);
}

void RunModifyBeforeFirstInclude()
{
  alarm(5);
  Poller *poller = MakePoller();
  SocketPair socket;
  RecordingCallback callback;
  Channel *channel = new Channel(poller, socket.fd[0], &callback);
  CallBack *savedCallback;
  void *savedArgument;
  recordNextUnlock = true;
  channel->GetCallBack(&savedCallback, &savedArgument);
  Check(recordedMutex && savedCallback == &callback);
  pthread_mutex_t *channelMutex = recordedMutex;
  enableUnlockStage.store(0, std::memory_order_release);

  std::thread firstEnable([&] {
    // Hold MiFD after Include drops chMutex but before it enters the pipe.
    pauseAfterUnlock = channelMutex;
    Check(channel->Enable(Channel::errorEvents));
  });
  while (enableUnlockStage.load(std::memory_order_acquire) != 1)
    std::this_thread::yield();
  Check(channel->Enable(Channel::readEvents));
  enableUnlockStage.store(2, std::memory_order_release);
  firstEnable.join();

  socket.Write();
  callback.done.Wait();
  Check(callback.events.load() & CallBack::ReadyToRead);
  _exit(0);
}

void RunReorderedModifyCommands()
{
  alarm(5);
  Poller *poller = MakePoller();
  SocketPair socket, fenceSocket;
  RecordingCallback callback, fenceCallback;
  Channel *channel = new Channel(poller, socket.fd[0], &callback);
  Check(channel->Enable(Channel::readEvents));
  CallBack *savedCallback;
  void *savedArgument;
  recordNextUnlock = true;
  channel->GetCallBack(&savedCallback, &savedArgument);
  Check(recordedMutex && savedCallback == &callback);
  pthread_mutex_t *channelMutex = recordedMutex;
  enableUnlockStage.store(0, std::memory_order_release);

  std::thread staleDisable([&] {
    pauseAfterUnlock = channelMutex;
    Check(channel->Disable(Channel::readEvents));
  });
  while (enableUnlockStage.load(std::memory_order_acquire) != 1)
    std::this_thread::yield();
  Check(channel->Enable(Channel::readEvents));
  enableUnlockStage.store(2, std::memory_order_release);
  staleDisable.join();

  Channel *fence = new Channel(poller, fenceSocket.fd[0], &fenceCallback);
  Check(fence->Enable(Channel::readEvents));
  socket.Write();
  callback.done.Wait();
  Check(callback.events.load() & CallBack::ReadyToRead);
  channel->Delete();
  fence->Delete();
  delete poller;
  _exit(0);
}

void RunStaleRemoveSlotReuse()
{
  alarm(5);
  Poller *poller = MakePoller();
  SocketPair oldSocket, barrierSocket, replacementSocket, fenceSocket;
  RecordingCallback oldCallback, barrierCallback, replacementCallback;
  RecordingCallback fenceCallback;
  Channel *oldChannel = new Channel(poller, oldSocket.fd[0], &oldCallback);
  Channel *barrier = new Channel(
    poller, barrierSocket.fd[0], &barrierCallback);
  Check(oldChannel->Enable(Channel::readEvents));
  Check(barrier->Enable(Channel::readEvents));
  pipeWriteStage.store(0, std::memory_order_release);

  std::thread deleter([&] {
    // Stop after Exclude validates the old slot but before RmFD enters the pipe.
    pauseBeforeNextWrite = true;
    oldChannel->Delete();
  });
  while (pipeWriteStage.load(std::memory_order_acquire) != 1)
    std::this_thread::yield();

  // The barrier's synchronous removal fences the hangup-driven FDRem.
  oldSocket.Hangup();
  barrier->Delete();
  Channel *replacement = new Channel(
    poller, replacementSocket.fd[0], &replacementCallback);
  Check(replacement->Enable(Channel::readEvents));
  pipeWriteStage.store(2, std::memory_order_release);
  deleter.join();

  Channel *fence = new Channel(poller, fenceSocket.fd[0], &fenceCallback);
  Check(fence->Enable(Channel::readEvents));
  replacementSocket.Write();
  replacementCallback.done.Wait();
  Check(replacementCallback.events.load() & CallBack::ReadyToRead);
  replacement->Delete();
  fence->Delete();
  delete poller;
  _exit(0);
}

void RunIncludeFloorReuseRace()
{
  alarm(5);
  recordNextUnlock = recordMutexLocks = recordedLockOverflow = false;
  recordedMutex = pauseAfterUnlock = pauseBeforeMutex = nullptr;
  recordedLockCount = 0;
  enableUnlockStage.store(0, std::memory_order_relaxed);
  mutexLockStage.store(0, std::memory_order_relaxed);
  Poller *poller = MakePoller();
  SocketPair probeSocket, delayedSocket, oldSocket, fenceSocket;
  RecordingCallback probeCallback, delayedCallback, oldCallback, fenceCallback;
  Channel *probe = new Channel(poller, probeSocket.fd[0], &probeCallback);

  // Include takes chMutex, pollMutex, and tagMutex; Modify takes only chMutex
  // and tagMutex. The unique lock remains pollMutex if tag allocation moves.
  recordMutexLocks = true;
  Check(probe->Enable(Channel::readEvents));
  recordMutexLocks = false;
  Check(!recordedLockOverflow);
  pthread_mutex_t *includeLocks[8];
  const int includeLockCount = recordedLockCount;
  for (int i = 0; i < includeLockCount; ++i)
    includeLocks[i] = recordedLocks[i];
  recordedLockCount = 0;
  recordedLockOverflow = false;
  recordMutexLocks = true;
  Check(probe->Disable(Channel::readEvents));
  recordMutexLocks = false;
  Check(!recordedLockOverflow);
  pthread_mutex_t *pollMutex = nullptr;
  int uniqueLocks = 0;
  for (int i = 0; i < includeLockCount; ++i)
  {
    int j = 0;
    while (j < recordedLockCount && includeLocks[i] != recordedLocks[j]) ++j;
    if (j == recordedLockCount) {pollMutex = includeLocks[i]; ++uniqueLocks;}
  }
  Check(uniqueLocks == 1);
  probe->Delete();

  Channel *delayed = new Channel(
    poller, delayedSocket.fd[0], &delayedCallback);

  std::thread delayedInclude([&] {
    pauseBeforeMutex = pollMutex;
    Check(delayed->Enable(Channel::readEvents));
  });
  while (mutexLockStage.load(std::memory_order_acquire) != 1)
    std::this_thread::yield();

  Channel *oldChannel = new Channel(poller, oldSocket.fd[0], &oldCallback);
  Check(oldChannel->Enable(Channel::readEvents));
  CallBack *savedCallback;
  void *savedArgument;
  recordNextUnlock = true;
  oldChannel->GetCallBack(&savedCallback, &savedArgument);
  Check(recordedMutex && savedCallback == &oldCallback);
  pthread_mutex_t *channelMutex = recordedMutex;
  enableUnlockStage.store(0, std::memory_order_release);

  std::thread staleModify([&] {
    pauseAfterUnlock = channelMutex;
    Check(oldChannel->Enable(Channel::writeEvents));
  });
  while (enableUnlockStage.load(std::memory_order_acquire) != 1)
    std::this_thread::yield();
  oldChannel->Delete();

  mutexLockStage.store(2, std::memory_order_release);
  delayedInclude.join();
  enableUnlockStage.store(2, std::memory_order_release);
  staleModify.join();

  // Fence the stale MdFD, then require the new slot's read-only mask.
  Channel *fence = new Channel(poller, fenceSocket.fd[0], &fenceCallback);
  Check(fence->Enable(Channel::readEvents));
  delayedSocket.Write();
  delayedCallback.done.Wait();
  Check(delayedCallback.events.load() == CallBack::ReadyToRead);
  delayed->Delete();
  fence->Delete();
  delete poller;
  _exit(0);
}
#endif

// A synchronous backend exposes the poll timeout computation without waiting
// weeks or measuring CPU load. Real Channel::Enable/Disable maintain the real
// timeout queue; only OS descriptor registration and the worker are omitted.
class TimeoutPoller : public Poller
{
public:
  TimeoutPoller() : Poller(-1, -1) { pollTid = XrdSysThread::ID(); }
  int NextWait() { return TmoGet(); }
  int PollEntry(Channel *channel) { return GetPollEnt(channel); }
protected:
  void Begin(XrdSysSemaphore *, int &, const char **) override {}
  void Exclude(Channel *, bool &, bool) override {}
  bool Include(Channel *, int &, const char **, bool &) override { return true; }
  bool Modify(Channel *, int &, const char **, bool &) override { return true; }
  void Shutdown() override {}
};

// Solaris event ports are one-shot, unlike the native Linux and macOS
// backends, so callback completion must re-associate a live channel. This
// synchronous backend exercises that contract on every test platform.
#ifdef XRD_SYS_IOEVENTS_FORCE_PORT_REARM
class EventPortPoller : public Poller
{
public:
  EventPortPoller() : Poller(-1, -1)
  {
    pollTid = XrdSysThread::ID();
    cmdFD = 0; // Make Detach exercise Exclude without using a command pipe.
  }

  void Dispatch(Channel *channel, int events, int error = 0)
  {
    pollTid = XrdSysThread::ID();
    const char *text = error ? "polling" : nullptr;
    bool locked = false;
    if (!CbkXeq(channel, events, error, text))
      Exclude(channel, locked, false);
  }

  void ResetCounts() { modifies = 0; excludes = 0; }

  std::atomic<int> modifies{0}, excludes{0};
  std::atomic<bool> blockDeleteExclude{false};
  XrdSysSemaphore deleteExcludeEntered{0}, deleteExcludeRelease{0};

protected:
  void Begin(XrdSysSemaphore *, int &, const char **) override {}
  void Exclude(Channel *, bool &, bool verify) override
  {
    ++excludes;
    if (verify && blockDeleteExclude)
    {
      deleteExcludeEntered.Post();
      deleteExcludeRelease.Wait();
    }
  }
  bool Include(Channel *, int &, const char **, bool &) override { return true; }
  bool Modify(Channel *channel, int &, const char **, bool &) override
  {
    (void)channel->GetFD();
    ++modifies;
    return true;
  }
  void Shutdown() override {}
};
#endif

#if defined(__linux__) && defined(XRD_SYS_IOEVENTS_FORCE_POLL)
int InspectPollEntry(Channel *channel)
{
  TimeoutPoller inspector;
  return inspector.PollEntry(channel);
}
#endif

void CheckTimeout(int seconds)
{
  TimeoutPoller poller;
  RecordingCallback callback;
  Channel *channel = new Channel(&poller, -1, &callback);
  const time_t before = time(nullptr);
  Check(channel->Enable(Channel::readEvents, seconds));
  const int actual = poller.NextWait();
  const time_t elapsed = time(nullptr) - before;
  const long long high = 1000LL * seconds;
  const long long low = 1000LL * (seconds - elapsed);
  Check(actual >= (low > INT_MAX ? INT_MAX : low));
  Check(actual <= (high > INT_MAX ? INT_MAX : high));
  Check(!callback.events && !callback.errors);
  channel->Delete();
}
}

#ifdef XRD_SYS_IOEVENTS_FORCE_PORT_REARM
// User: a plugin keeps a healthy channel enabled after handling ordinary input
// on Solaris. Valid-path control: event ports must re-associate that one-shot
// descriptor exactly once, and the review fix must preserve that behavior.
TEST(IOEventsCore, EventPortRearmsLiveCallbackExactlyOnce)
{
  struct KeepEnabled : CallBack
  {
    bool Event(Channel *, void *, int flags) override
    {
      Check(flags == CallBack::ReadyToRead);
      return true;
    }
  } callback;
  EventPortPoller poller;
  Channel *channel = new Channel(&poller, -1, &callback);
  ASSERT_TRUE(channel->Enable(Channel::readEvents));
  poller.ResetCounts();
  poller.Dispatch(channel, CallBack::ReadyToRead);
  EXPECT_EQ(poller.modifies.load(), 1);
  channel->Delete();
}

// User: a Solaris plugin asks for Fatal notification when its peer disconnects.
// Branch guard: PR 2957 before the review fix re-associated the failed descriptor
// after Fatal; the callback-completion contract must now leave it detached.
TEST(IOEventsCore, EventPortFatalCompletionDoesNotRearmFailedDescriptor)
{
  struct FatalOnly : CallBack
  {
    bool Event(Channel *, void *, int) override { Check(false); return false; }
    void Fatal(Channel *, void *, int error, const char *) override
    {
      Check(error == EPIPE);
    }
  } callback;
  EventPortPoller poller;
  Channel *channel = new Channel(&poller, -1, &callback);
  ASSERT_TRUE(channel->Enable(Channel::readEvents | Channel::errorEvents));
  poller.ResetCounts();
  poller.Dispatch(channel, 0, EPIPE);
  EXPECT_EQ(poller.modifies.load(), 0);
  EXPECT_EQ(poller.excludes.load(), 1);
  channel->Delete();
}

// User: a Solaris plugin releases a failed channel from inside Fatal.
// Branch guard: the pre-fix dispatch dereferenced the freed Channel in Modify;
// completion must return to the backend without trying to re-associate it.
TEST(IOEventsCore, EventPortFatalCallbackCanDeleteItsChannel)
{
  struct SelfDeleting : CallBack
  {
    bool Event(Channel *, void *, int) override { Check(false); return false; }
    void Fatal(Channel *channel, void *, int, const char *) override
    {
      channel->Delete();
    }
  } callback;
  EventPortPoller poller;
  Channel *channel = new Channel(&poller, -1, &callback);
  ASSERT_TRUE(channel->Enable(Channel::readEvents | Channel::errorEvents));
  poller.ResetCounts();
  poller.Dispatch(channel, 0, EPIPE);
  EXPECT_EQ(poller.modifies.load(), 0);
}

// User: one Solaris thread deletes a failed plugin channel while its Fatal
// callback is still running. Branch guard: PR 2957 before the review fix posted
// Delete's waiter and then called Modify through freed Channel storage.
TEST(IOEventsCore, EventPortConcurrentDeleteCannotRaceBackendRearm)
{
  struct BlockingFatal : CallBack
  {
    XrdSysSemaphore entered{0}, release{0};
    bool Event(Channel *, void *, int) override { Check(false); return false; }
    void Fatal(Channel *, void *, int, const char *) override
    {
      entered.Post();
      release.Wait();
    }
  } callback;
  EventPortPoller poller;
  Channel *channel = new Channel(&poller, -1, &callback);
  ASSERT_TRUE(channel->Enable(Channel::readEvents | Channel::errorEvents));
  poller.ResetCounts();
  poller.blockDeleteExclude = true;

  std::thread dispatcher([&] { poller.Dispatch(channel, 0, EPIPE); });
  callback.entered.Wait();
  std::atomic<bool> deleted{false};
  std::thread deleter([&] { channel->Delete(); deleted = true; });
  poller.deleteExcludeEntered.Wait();
  callback.release.Post();
  poller.deleteExcludeRelease.Post();
  dispatcher.join();
  deleter.join();

  EXPECT_TRUE(deleted);
  EXPECT_EQ(poller.modifies.load(), 0);
}
#endif

#if defined(XRD_SYS_IOEVENTS_FORCE_POLL)
// User: the fallback-poller evidence must execute PollPoll rather than silently
// retest Linux epoll. Branch guard: removing or ignoring the test-only backend
// selector makes backend-specific regressions appear to pass without coverage.
TEST(IOEventsCore, ForcedBuildUsesPollPollBackend)
{
  ASSERT_EXIT(([] {
    alarm(5);
    Poller *poller = MakePoller();
    TimeoutPoller inspector;
    SocketPair socket;
    RecordingCallback callback;
    Channel *channel = new Channel(poller, socket.fd[0], &callback);
    Check(channel->Enable(Channel::readEvents));
    Check(inspector.PollEntry(channel) != 0);
    channel->Delete();
    delete poller;
    _exit(0);
  }()), ::testing::ExitedWithCode(0), "");
}
#endif

#ifdef __linux__
// User: one thread enables a plugin channel while another releases its final
// reference. Delete may free the Channel as soon as Enable drops chMutex.
// Stock upstream 5b716c84a then reads chPollXQ from the freed object; the
// locked snapshot makes the remainder of Enable independent of that storage.
// Stock reproduced under AddressSanitizer; the barrier controls only the
// interleaving and adds no synchronization between the conflicting paths.
TEST(IOEventsCore, EnableDoesNotReadChannelAfterUnlock)
{
  ASSERT_EXIT(([] {
    alarm(5);
    TimeoutPoller poller;
    RecordingCallback callback;
    Channel *channel = new Channel(&poller, -1, &callback);
    CallBack *savedCallback;
    void *savedArgument;
    recordNextUnlock = true;
    channel->GetCallBack(&savedCallback, &savedArgument);
    Check(recordedMutex && savedCallback == &callback);
    pthread_mutex_t *channelMutex = recordedMutex;
    enableUnlockStage.store(0, std::memory_order_release);

    std::thread enabler([&] {
      // Pause when Enable drops chMutex, after which the wake decision must no
      // longer inspect the Channel. Targeting the mutex avoids other unlocks.
      pauseAfterUnlock = channelMutex;
      Check(channel->Enable(Channel::readEvents, 30));
    });
    while (enableUnlockStage.load(std::memory_order_acquire) != 1)
      std::this_thread::yield();

    channel->Delete();
    enableUnlockStage.store(2, std::memory_order_release);
    enabler.join();
    _exit(0);
  }()), ::testing::ExitedWithCode(0), "");
}
#endif

#if defined(__linux__) && defined(XRD_SYS_IOEVENTS_FORCE_POLL)
// User: a plugin enables another event while a management thread releases the
// same portable-poller channel; tracing must not inspect it after Modify drops
// the channel lock. Stock reproduced against upstream 5b716c84a and the
// minimized branch: TRACE_MOD reads chFD after Delete guards the allocation.
TEST(IOEventsCore, EnableTraceDoesNotReadChannelAfterPollPollModifyUnlock)
{
  ::testing::FLAGS_gtest_death_test_style = "threadsafe";
  setenv("XrdSysIOE_TRACE", "1", 1);
  EXPECT_EXIT(RunModifyDeleteRace(ModifyOperation::EnableWrite),
    ::testing::ExitedWithCode(0), ".*");
  unsetenv("XrdSysIOE_TRACE");
}

// User: a plugin disables one event while a management thread releases the
// same portable-poller channel; tracing must not inspect it after Modify drops
// the channel lock. Stock reproduced against upstream 5b716c84a and the
// minimized branch: TRACE_MOD reads chFD after Delete guards the allocation.
TEST(IOEventsCore, DisableTraceDoesNotReadChannelAfterPollPollModifyUnlock)
{
  ::testing::FLAGS_gtest_death_test_style = "threadsafe";
  setenv("XrdSysIOE_TRACE", "1", 1);
  EXPECT_EXIT(RunModifyDeleteRace(ModifyOperation::DisableRead),
    ::testing::ExitedWithCode(0), ".*");
  unsetenv("XrdSysIOE_TRACE");
}

// User: one thread disables a connection while another releases it and a new
// connection immediately reuses its fallback-poller slot.
// Stock reproduced against upstream 5b716c84a and the pre-fix minimized branch:
// delayed MdFD silently disables reads on the replacement connection.
TEST(IOEventsCore, StaleModifyDoesNotOverwriteReusedPollSlot)
{
  EXPECT_EXIT(RunStaleModifyReplacementRace(),
    ::testing::ExitedWithCode(0), "");
}

// User: a management thread releases a connection while its first Enable is
// registering with the fallback poller.
// Stock reproduced against upstream 5b716c84a and the pre-fix minimized branch:
// Init relocks the freed Channel.
TEST(IOEventsCore, FirstEnableDoesNotRelockFreedChannel)
{
  EXPECT_EXIT(RunFirstEnableDeleteRace(),
    ::testing::ExitedWithCode(0), "");
}

// User: a plugin adds readable interest while its first fallback-poller
// registration is waiting to enter the command pipe.
// Branch regression reproduced on the original PR 2958 head: delayed MiFD
// overwrites the newer MdFD mask, so the connection never becomes readable.
TEST(IOEventsCore, ModifyBeforeFirstIncludeKeepsNewestEventMask)
{
  EXPECT_EXIT(RunModifyBeforeFirstInclude(),
    ::testing::ExitedWithCode(0), "");
}

// User: two management threads disable and then re-enable the same plugin
// channel while the first command is delayed.
// Stock reproduced against upstream 5b716c84a and the pre-fix minimized branch:
// the old disable wins permanently.
TEST(IOEventsCore, NewerModifyWinsWhenPipeWritesArriveOutOfOrder)
{
  EXPECT_EXIT(RunReorderedModifyCommands(),
    ::testing::ExitedWithCode(0), "");
}

// User: a disconnect retires a slot after another thread validates its RmFD
// but before the write.
// Stock reproduced against upstream 5b716c84a and the pre-fix minimized branch:
// delayed removal deletes the replacement connection.
TEST(IOEventsCore, StaleRemoveCannotDeleteAReusedPollSlot)
{
  EXPECT_EXIT(RunStaleRemoveSlotReuse(),
    ::testing::ExitedWithCode(0), "");
}

// User: a new plugin registration can stall while an older connection uses,
// releases, and queues work for the same fallback-poller slot.
// Stock reproduced against upstream 5b716c84a: the stale command changes the
// new channel. Branch guard: assigning its tag before the table lock lowers the
// reused slot's floor and admits that same command.
TEST(IOEventsCore, IncludeCannotPublishAnOlderFloorAfterSlotReuse)
{
  ::testing::FLAGS_gtest_death_test_style = "threadsafe";
  EXPECT_EXIT(RunIncludeFloorReuseRace(),
    ::testing::ExitedWithCode(0), "");
}
#endif

// User: while one fallback-poller callback is slow, another connection's idle
// timeout becomes overdue and a third client registers immediately afterward.
// Stock reproduced under ThreadSanitizer against upstream 5b716c84a: Begin
// invokes the overdue callback from TmoGet after dropping pollMutex, so FDRem's
// descriptor-table write races the next Include. Branch guard: the callback
// ordering below selects that pre-poll path rather than the locked path used
// after poll itself times out.
TEST(IOEventsCore, PrePollTimeoutRemovalKeepsFallbackTableSerialized)
{
  ASSERT_EXIT(([] {
    alarm(10);
    struct BlockingRead : CallBack
    {
      XrdSysSemaphore entered{0}, release{0};
      bool Event(Channel *channel, void *, int flags) override
      {
        Check(flags & CallBack::ReadyToRead);
        char byte;
        Check(read(channel->GetFD(), &byte, 1) == 1);
        entered.Post();
        release.Wait();
        return true;
      }
      void Fatal(Channel *, void *, int, const char *) override { _exit(2); }
    } blocking;
    struct Expired : CallBack
    {
      XrdSysSemaphore entered{0}, release{0}, returned{0};
      bool Event(Channel *, void *, int flags) override
      {
        Check(flags & CallBack::ReadTimeOut);
        entered.Post();
        release.Wait();
        returned.Post();
        return false;
      }
      void Fatal(Channel *, void *, int, const char *) override { _exit(3); }
    } expired;

    Poller *poller = MakePoller();
    SocketPair busySocket, timeoutSocket, replacementSocket;
    RecordingCallback replacement;
    Channel *busy = new Channel(poller, busySocket.fd[0], &blocking);
    Channel *timeout = new Channel(poller, timeoutSocket.fd[0], &expired);
    Channel *replacementChannel = new Channel(
      poller, replacementSocket.fd[0], &replacement);
    Check(busy->Enable(Channel::readEvents));
    Check(timeout->Enable(Channel::readEvents));

    // Dispatch a real readable event first. While its callback holds the outer
    // PollPoll batch lock, add a deadline without modifying the poll set.
    busySocket.Write();
    blocking.entered.Wait();
    Check(timeout->Enable(Channel::readEvents, 1));
    const time_t armed = time(nullptr);
    while (time(nullptr) < armed + 2)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    blocking.release.Post();

    // The next loop evaluates TmoGet with pollMutex dropped. Registering the
    // replacement after the callback returns races stock's later unlocked
    // FDRem regardless of which access occurs first; the fixed lock orders it.
    expired.entered.Wait();
    expired.release.Post();
    expired.returned.Wait();
    Check(replacementChannel->Enable(Channel::readEvents));
    replacementSocket.Write();
    replacement.done.Wait();

    busy->Delete();
    timeout->Delete();
    replacementChannel->Delete();
    delete poller;
    _exit(0);
  }()), ::testing::ExitedWithCode(0), "");
}

// User: A plugin using IOEvents requests explicit fatal notifications and then
// releases a hung-up connection. Stock reproduced against upstream 5b716c84a:
// Fatal leaves callback mode set, so Delete waits for an already-finished call.
// Stock XrdCl does not request errorEvents; this is a public plugin API case.
TEST(IOEventsCore, FatalCompletionAllowsLaterDeletion)
{
  ASSERT_EXIT(([] {
    alarm(5);
    Poller *poller = MakePoller();
    SocketPair failed, healthy;
    RecordingCallback fatal, good;
    Channel *channel = new Channel(poller, failed.fd[0], &fatal);
    Channel *other = new Channel(poller, healthy.fd[0], &good);
    Check(channel->Enable(Channel::readEvents | Channel::errorEvents));
    Check(other->Enable(Channel::readEvents));
    failed.Hangup();
    fatal.done.Wait();
    Check(fatal.errors != 0 && fatal.events == 0);
    healthy.Write();
    good.done.Wait(); // Same event thread: Fatal has now returned completely.
    channel->Delete();
    other->Delete();
    delete poller;
    _exit(0);
  }()), ::testing::ExitedWithCode(0), "");
}

// User: after a plugin handles a failed connection, it installs a newly
// connected descriptor on the same Channel instead of allocating a new one.
// Valid-path control: upstream epoll/kqueue preserve the documented post-Fatal
// SetFD reuse. Branch guard: fatal cleanup must detach with keep=true; making it
// a permanent detach discards the callback and makes the later Enable fail.
TEST(IOEventsCore, FatalCompletionKeepsChannelReusable)
{
  ASSERT_EXIT(([] {
    alarm(5);
    Poller *poller = MakePoller();
    SocketPair failed, replacement, barrierSocket;
    RecordingCallback callback, barrier;
    Channel *channel = new Channel(poller, failed.fd[0], &callback);
    Channel *barrierChannel = new Channel(
      poller, barrierSocket.fd[0], &barrier);
    Check(channel->Enable(Channel::readEvents | Channel::errorEvents));
    Check(barrierChannel->Enable(Channel::readEvents));

    failed.Hangup();
    callback.done.Wait();
    Check(callback.errors != 0 && callback.events == 0);
    barrierSocket.Write();
    barrier.done.Wait(); // Fatal and its backend completion have both returned.

    channel->SetFD(replacement.fd[0]);
    Check(channel->Enable(Channel::readEvents));
    replacement.Write();
    callback.done.Wait();
    Check(callback.events & CallBack::ReadyToRead);

    channel->Delete();
    barrierChannel->Delete();
    delete poller;
    _exit(0);
  }()), ::testing::ExitedWithCode(0), "");
}

// User: A plugin closes its channel while its fatal-notification callback is
// still finishing. Stock reproduced against upstream 5b716c84a: the Fatal path
// omits the completion acknowledgment needed by a concurrent Delete.
TEST(IOEventsCore, FatalCompletionAcknowledgesConcurrentDeletion)
{
  ASSERT_EXIT(([] {
    alarm(5);
    struct BlockingFatal : CallBack
    {
      XrdSysSemaphore entered{0}, release{0};
      bool Event(Channel *, void *, int) override { _exit(2); }
      void Fatal(Channel *, void *, int, const char *) override
      {
        entered.Post();
        release.Wait();
      }
    } callback;
    Poller *poller = MakePoller();
    SocketPair failed;
    Channel *channel = new Channel(poller, failed.fd[0], &callback);
    Check(channel->Enable(Channel::readEvents | Channel::errorEvents));
    failed.Hangup();
    callback.entered.Wait();
    XrdSysSemaphore deleting(0);
    std::atomic<bool> deleted{false};
    std::thread deleter([&] {
      deleting.Post();
      channel->Delete();
      deleted = true;
    });
    deleting.Wait();
    callback.release.Post();
    deleter.join();
    Check(deleted);
    delete poller;
    _exit(0);
  }()), ::testing::ExitedWithCode(0), "");
}

// User: A plugin releases many failed connections as soon as each Fatal call
// reports the disconnect. Stock reproduced against upstream 5b716c84a: deletion
// hangs at the missing fatal-completion step. Branch guard: completion must also
// remove the descriptor under the channel lock before Delete can free backend
// storage; 128 repetitions stress that narrower removal overlap.
TEST(IOEventsCore, FatalReturnAndImmediateDeletionKeepBackendStorageAlive)
{
  ASSERT_EXIT(([] {
    alarm(10);
    Poller *poller = MakePoller();
    for (int attempt = 0; attempt < 128; ++attempt)
    {
      SocketPair failed;
      RecordingCallback callback;
      Channel *channel = new Channel(poller, failed.fd[0], &callback);
      Check(channel->Enable(Channel::readEvents | Channel::errorEvents));
      failed.Hangup();
      callback.done.Wait();
      Check(callback.errors != 0);
      // No second callback barrier: Delete races the end of Fatal and the
      // backend's removal of this channel from its descriptor set.
      channel->Delete();
    }
    delete poller;
    _exit(0);
  }()), ::testing::ExitedWithCode(0), "");
}

// User: A plugin frees the failed channel directly in its Fatal callback.
// Valid-path control: upstream 5b716c84a already permits self-deletion; sharing
// normal and fatal completion must not access the channel after callback deletion.
TEST(IOEventsCore, FatalCallbackCanDeleteItsOwnChannel)
{
  ASSERT_EXIT(([] {
    alarm(5);
    struct SelfDeleting : CallBack
    {
      XrdSysSemaphore done{0};
      bool Event(Channel *, void *, int) override { _exit(2); }
      void Fatal(Channel *channel, void *, int, const char *) override
      {
        channel->Delete();
        done.Post();
      }
    } callback;
    Poller *poller = MakePoller();
    SocketPair failed;
    Channel *channel = new Channel(poller, failed.fd[0], &callback);
    Check(channel->Enable(Channel::readEvents | Channel::errorEvents));
    failed.Hangup();
    callback.done.Wait();
    delete poller;
    _exit(0);
  }()), ::testing::ExitedWithCode(0), "");
}

// User: A plugin replaces a failed descriptor from inside Fatal and continues
// reading on the replacement connection. Stock reproduced against upstream
// 5b716c84a: fatal cleanup excludes the newly enabled descriptor even though
// SetFD during callbacks is explicitly supported by the public API.
TEST(IOEventsCore, FatalCallbackCanReplaceTheDescriptor)
{
  ASSERT_EXIT(([] {
    alarm(5);
    struct Replacing : RecordingCallback
    {
      int replacement = -1;
      void Fatal(Channel *channel, void *, int error, const char *) override
      {
        errors = error;
        channel->SetFD(replacement);
        Check(channel->Enable(Channel::readEvents));
        done.Post();
      }
    } callback;
    Poller *poller = MakePoller();
    SocketPair failed, replacement;
    callback.replacement = replacement.fd[0];
    Channel *channel = new Channel(poller, failed.fd[0], &callback);
    Check(channel->Enable(Channel::readEvents | Channel::errorEvents));
    failed.Hangup();
    callback.done.Wait();
    replacement.Write();
    callback.done.Wait();
    Check(callback.errors != 0 && (callback.events & CallBack::ReadyToRead));
    channel->Delete();
    delete poller;
    _exit(0);
  }()), ::testing::ExitedWithCode(0), "");
}

// User: A plugin moves an ordinary readable channel to a replacement socket
// from inside Event. Stock reproduced against upstream on forced PollPoll: its
// stale table entry makes the replacement Enable fail with EEXIST. Valid-path
// control: upstream epoll/kqueue already support this transition. Branch guard:
// common completion must keep event ports from altering the replacement.
TEST(IOEventsCore, EventCallbackCanReplaceTheDescriptor)
{
  ASSERT_EXIT(([] {
    alarm(5);
    struct Replacing : RecordingCallback
    {
      int replacement = -1;
      std::atomic<int> calls{0};
      bool Event(Channel *channel, void *, int flags) override
      {
        Check(flags & CallBack::ReadyToRead);
        char byte;
        Check(read(channel->GetFD(), &byte, 1) == 1);
        if (++calls == 1)
        {
          channel->SetFD(replacement);
          Check(channel->Enable(Channel::readEvents));
        }
        events = flags;
        done.Post();
        return true;
      }
      void Fatal(Channel *, void *, int, const char *) override { _exit(2); }
    } callback;
    Poller *poller = MakePoller();
    SocketPair first, replacement;
    callback.replacement = replacement.fd[0];
    Channel *channel = new Channel(poller, first.fd[0], &callback);
    Check(channel->Enable(Channel::readEvents));
    first.Write();
    callback.done.Wait();
    replacement.Write();
    callback.done.Wait();
    Check(callback.calls == 2 && (callback.events & CallBack::ReadyToRead));
    channel->Delete();
    delete poller;
    _exit(0);
  }()), ::testing::ExitedWithCode(0), "");
}

// User: an Event callback withdraws its descriptor with SetFD while another
// owner concurrently releases the Channel. Valid-path control: upstream keeps
// callback ownership until Event returns. Branch guard: the replacement state
// must remain nonzero so Delete waits rather than freeing callback storage;
// IOEvents tracing also names that state, guarding its status-table entry.
TEST(IOEventsCore, SetFDPreservesCallbackOwnershipUntilReturn)
{
  ASSERT_EXIT(([] {
    alarm(5);
    struct Withdrawing : CallBack
    {
      XrdSysSemaphore entered{0}, release{0};
      bool Event(Channel *channel, void *, int flags) override
      {
        Check(flags & CallBack::ReadyToRead);
        char byte;
        Check(read(channel->GetFD(), &byte, 1) == 1);
        channel->SetFD(-1);
        entered.Post();
        release.Wait();
        return true;
      }
      void Fatal(Channel *, void *, int, const char *) override { _exit(2); }
    } callback;

    Poller *poller = MakePoller();
    SocketPair socket;
    Channel *channel = new Channel(poller, socket.fd[0], &callback);
    Check(channel->Enable(Channel::readEvents));
    socket.Write();
    callback.entered.Wait();

    XrdSysSemaphore started(0);
    std::atomic<bool> deleted{false};
    std::thread deleter([&] {
      started.Post();
      channel->Delete();
      deleted = true;
    });
    started.Wait();
    for (int i = 0; i < 200 && !deleted; ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    Check(!deleted);
    callback.release.Post();
    deleter.join();
    Check(deleted);
    delete poller;
    _exit(0);
  }()), ::testing::ExitedWithCode(0), "");
}

// User: an Event callback withdraws a closed descriptor and its owner deletes
// the disabled Channel after that callback has finished. Valid-path control:
// upstream permits this lifecycle. Branch guard: callback completion must turn
// isChanged back into isClear or Delete waits for a callback already gone.
TEST(IOEventsCore, SetFDTransitionClearsAfterCallbackReturns)
{
  ASSERT_EXIT(([] {
    alarm(5);
    struct Withdrawing : CallBack
    {
      XrdSysSemaphore done{0};
      bool Event(Channel *channel, void *, int flags) override
      {
        Check(flags & CallBack::ReadyToRead);
        char byte;
        Check(read(channel->GetFD(), &byte, 1) == 1);
        channel->SetFD(-1);
        done.Post();
        return true;
      }
      void Fatal(Channel *, void *, int, const char *) override { _exit(2); }
    } callback;

    Poller *poller = MakePoller();
    SocketPair socket, barrierSocket;
    RecordingCallback barrier;
    Channel *channel = new Channel(poller, socket.fd[0], &callback);
    Channel *barrierChannel = new Channel(
      poller, barrierSocket.fd[0], &barrier);
    Check(channel->Enable(Channel::readEvents));
    Check(barrierChannel->Enable(Channel::readEvents));
    socket.Write();
    callback.done.Wait();
    barrierSocket.Write();
    barrier.done.Wait(); // The same event loop has completed the first callback.

    channel->Delete();
    barrierChannel->Delete();
    delete poller;
    _exit(0);
  }()), ::testing::ExitedWithCode(0), "");
}

// User: The ordinary client enables read events and sees a peer disconnect.
// Valid-path control: passes upstream 5b716c84a. Without errorEvents failure
// arrives as ReadyToRead, allowing the existing socket reader to observe EOF.
TEST(IOEventsCore, PeerDisconnectStillMapsToReadWithoutFatalEvents)
{
  ASSERT_EXIT(([] {
    alarm(5);
    Poller *poller = MakePoller();
    SocketPair failed;
    RecordingCallback callback;
    Channel *channel = new Channel(poller, failed.fd[0], &callback);
    Check(channel->Enable(Channel::readEvents));
    failed.Hangup();
    callback.done.Wait();
    Check(callback.errors == 0 && (callback.events & CallBack::ReadyToRead));
    channel->Delete();
    delete poller;
    _exit(0);
  }()), ::testing::ExitedWithCode(0), "");
}

// User: A plugin configures a timeout just beyond the signed-millisecond range
// (about 24.85 days). Stock reproduced against upstream 5b716c84a: TmoGet spins
// inside its timeout loop instead of returning a bounded positive sleep.
TEST(IOEventsCore, TimeoutJustBeyondMillisecondsRangeIsClamped)
{
  ASSERT_EXIT(([] {
    alarm(3);
    // Ten seconds beyond the boundary keeps upstream's overflowing busy loop
    // active past the three-second alarm regardless of wall-clock phase.
    CheckTimeout(INT_MAX / 1000 + 10);
    _exit(0);
  }()), ::testing::ExitedWithCode(0), "");
}

// User: A plugin uses INT_MAX seconds for a very long idle connection timeout.
// Stock reproduced against upstream 5b716c84a: the conversion never returns.
// The poller must cap each sleep while retaining the original distant deadline.
TEST(IOEventsCore, MaximumSecondsTimeoutIsClamped)
{
  ASSERT_EXIT(([] {
    alarm(3);
    if (sizeof(time_t) <= sizeof(int)) _exit(0);
    CheckTimeout(INT_MAX);
    _exit(0);
  }()), ::testing::ExitedWithCode(0), "");
}

// User: A long configured timeout wraps to a small *positive* millisecond value.
// Stock reproduced against upstream 5b716c84a: around 49.7 days the poller wakes
// unnecessarily rather than waiting for the maximum representable interval.
TEST(IOEventsCore, PositiveMillisecondsWrapIsAlsoClamped)
{
  ASSERT_EXIT(([] {
    alarm(3);
    CheckTimeout(static_cast<int>((1ULL << 32) / 1000 + 1));
    _exit(0);
  }()), ::testing::ExitedWithCode(0), "");
}

// User: A normal client uses short timeouts, or a plugin uses the largest whole
// seconds value that still fits poll's milliseconds. Valid-path control:
// upstream passes; clamping must preserve ordinary and boundary conversions.
TEST(IOEventsCore, RepresentableTimeoutsKeepTheirOriginalWait)
{
  ASSERT_EXIT(([] {
    alarm(3);
    for (int seconds : {1, 15, 60, INT_MAX / 1000 - 1, INT_MAX / 1000})
      CheckTimeout(seconds);
    _exit(0);
  }()), ::testing::ExitedWithCode(0), "");
}

// User: A plugin removes an idle connection or enables one with no timer.
// Valid-path control: an empty timer queue still requests an indefinite poll;
// upstream 5b716c84a passes; clamping must not invent periodic wakeups.
TEST(IOEventsCore, RemovedAndAbsentTimeoutsRemainIndefinite)
{
  ASSERT_EXIT(([] {
    alarm(3);
    TimeoutPoller poller;
    RecordingCallback callback;
    Check(poller.NextWait() == -1);
    Channel *channel = new Channel(&poller, -1, &callback);
    Check(channel->Enable(Channel::readEvents, 0));
    Check(poller.NextWait() == -1);
    Check(channel->Enable(Channel::readEvents, 60));
    Check(poller.NextWait() > 0);
    channel->Delete();
    Check(poller.NextWait() == -1);
    _exit(0);
  }()), ::testing::ExitedWithCode(0), "");
}

// User: A short query timeout shares a poller with a very long-lived transfer.
// Stock reproduced against upstream 5b716c84a plus ordering control: the short
// deadline wins initially; deleting it exposes the overflowing long timeout.
TEST(IOEventsCore, ShorterDeadlineWinsOverClampedDeadline)
{
  ASSERT_EXIT(([] {
    alarm(3);
    if (sizeof(time_t) <= sizeof(int)) _exit(0);
    TimeoutPoller poller;
    RecordingCallback callback;
    Channel *longLived = new Channel(&poller, -1, &callback);
    Channel *shortLived = new Channel(&poller, -1, &callback);
    Check(longLived->Enable(Channel::readEvents, INT_MAX));
    Check(shortLived->Enable(Channel::readEvents, 60));
    const int wait = poller.NextWait();
    Check(wait > 0 && wait <= 60000);
    shortLived->Delete();
    Check(poller.NextWait() == INT_MAX);
    longLived->Delete();
    _exit(0);
  }()), ::testing::ExitedWithCode(0), "");
}
