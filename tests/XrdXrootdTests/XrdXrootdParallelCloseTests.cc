#include "XrdSfs/XrdSfsInterface.hh"
#include "XrdSys/XrdSysPthread.hh"

#define private public
#define protected public
#include "Xrd/XrdLinkXeq.hh"
#include "XrdXrootd/XrdXrootdFile.hh"
#include "XrdXrootd/XrdXrootdProtocol.hh"
#undef protected
#undef private

#include <gtest/gtest.h>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>

namespace XrdXrootd { extern XrdSysError eLog; }

namespace {

void Check(bool ok, const char *expression, int line)
{
  if (ok) return;
  dprintf(STDERR_FILENO, "XrdXrootdParallelCloseTests.cc:%d: %s\n",
          line, expression);
  _exit(1);
}

#define REQUIRE(expression) Check((expression), #expression, __LINE__)
#define FINISH_WITHIN(seconds, ...) \
  ASSERT_EXIT(([] { alarm(seconds); __VA_ARGS__ _exit(0); }()), \
              ::testing::ExitedWithCode(0), "")

class TestSfsFile : public XrdSfsFile
{
public:
  TestSfsFile() : XrdSfsFile("test", 0) {}

  int open(const char *, XrdSfsFileOpenMode, mode_t,
           const XrdSecEntity * = 0, const char * = 0) override
    {return SFS_OK;}
  int close() override {return SFS_OK;}
  int fctl(const int, const char *, XrdOucErrInfo &) override
    {return SFS_ERROR;}
  const char *FName() override {return "offload-test";}
  int getMmap(void **address, off_t &size) override
    {*address = nullptr; size = 0; return SFS_ERROR;}
  XrdSfsXferSize read(XrdSfsFileOffset, XrdSfsXferSize) override {return 0;}
  XrdSfsXferSize read(XrdSfsFileOffset, char *, XrdSfsXferSize) override
    {return 0;}
  int read(XrdSfsAio *) override {return SFS_ERROR;}
  XrdSfsXferSize write(XrdSfsFileOffset, const char *,
                       XrdSfsXferSize size) override {return size;}
  int write(XrdSfsAio *) override {return SFS_ERROR;}
  int stat(struct stat *info) override
    {std::memset(info, 0, sizeof(*info)); return SFS_OK;}
  int sync() override {return SFS_OK;}
  int sync(XrdSfsAio *) override {return SFS_OK;}
  int truncate(XrdSfsFileOffset) override {return SFS_OK;}
  int getCXinfo(char type[4], int &size) override
    {std::memset(type, 0, 4); size = 0; return SFS_OK;}
};

class TestLink : public XrdLinkXeq
{
public:
  void SetUse(int count) {LinkInfo.InUse = count;}
  int Use() const {return LinkInfo.InUse;}
};

}

// User: a client loses a bound parallel data connection after its offload was
// queued but before a server worker starts it. Closing that connection must
// cancel the offload, release the control-link and file references, and wake
// the request thread. Branch regression: stock runs the queued link job, while
// the first #2965 revision rejects it and waits forever in Recycle; this test
// bootstraps the close-fence-compatible cleanup.
TEST(XrdXrootdParallelClose, QueuedOffloadIsCancelledDuringBoundRecycle)
{
  FINISH_WITHIN(5,
    auto *parent = new XrdXrootdProtocol;
    auto *bound = new XrdXrootdProtocol;
    auto *parentLink = new TestLink;
    auto *boundLink = new TestLink;
    auto *file = new XrdXrootdFile("test", "/offload-test",
                                  new TestSfsFile);
    auto *recycled = new XrdSysSemaphore(0);
    XrdSysSemaphore retry(0);

    parentLink->SetUse(2);
    parent->Link = parentLink;
    bound->Link = boundLink;
    bound->Status = XRD_BOUNDPATH;
    bound->Stream[0] = parent;
    bound->boundRecycle = recycled;
    bound->IO.File = file;
    bound->isActive = true;
    bound->isLinkWT = false;
    bound->newPio = true;
    bound->reTry = &retry;
    file->Ref(1);

    XrdXrootd::eLog.setMsgMask(0);
    bound->Recycle(boundLink, 0, "test close");

    REQUIRE(!bound->isActive);
    REQUIRE(retry.CondWait());
    REQUIRE(recycled->CondWait());
    REQUIRE(parentLink->Use() == 1);
    file->Serialize();
  );
}
