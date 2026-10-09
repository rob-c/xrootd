#include "XrdOuc/XrdOucCache.hh"
#include "XrdOuc/XrdOucEnv.hh"
#include "XrdSys/XrdSysLogger.hh"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <fstream>
#include <string>
#include <utility>
#include <sys/stat.h>
#include <unistd.h>

namespace
{

class EagainOrigin final : public XrdOucCacheIO
{
public:
    explicit EagainOrigin(std::string path) : m_path(std::move(path)) {}

    bool Detach(XrdOucCacheIOCD &) override { return true; }
    long long FSize() override { return 4096; }

    int Fstat(struct stat &st) override
    {
        std::memset(&st, 0, sizeof(st));
        st.st_mode = S_IFREG | 0644;
        st.st_size = 4096;
        st.st_blocks = 8;
        return 0;
    }

    const char *Path() override { return m_path.c_str(); }
    int Read(char *, long long, int) override { return -EAGAIN; }

    void Read(XrdOucCacheIOCB &callback, char *, long long, int) override
    {
        callback.Done(-EAGAIN);
    }

    int Sync() override { return 0; }
    int Trunc(long long) override { return -ENOTSUP; }
    int Write(char *, long long, int) override { return -ENOTSUP; }

private:
    std::string m_path;
};

void Cleanup(const std::string &cache_path, const std::string &config_dir)
{
    unlink(cache_path.c_str());
    unlink((cache_path + ".cinfo").c_str());
    unlink((config_dir + "/pfc.cfg").c_str());
    rmdir((config_dir + "/tmp").c_str());
    rmdir(config_dir.c_str());
}

} // namespace

// An end user encounters this when an origin read immediately completes with
// EAGAIN.  Stock XRootD mistakes that completed error for its pending marker
// and waits forever.  This test loads the real PFC plugin and calls its actual
// synchronous reader; CTest's five-second limit makes the stock lockup visible.
int main(int argc, char **argv)
{
    if (argc != 2)
    {
        std::fprintf(stderr, "usage: %s /path/to/libXrdPfc.so\n", argv[0]);
        return 2;
    }

    char config_template[] = "/tmp/xrdpfc-read-pending-XXXXXX";
    const char *config_dir_ptr = mkdtemp(config_template);
    if (!config_dir_ptr)
    {
        std::perror("mkdtemp");
        return 3;
    }

    const std::string config_dir(config_dir_ptr);
    const std::string config_path = config_dir + "/pfc.cfg";
    const std::string cache_path = config_dir + "/tmp/data";
    const std::string origin_url = "root://invalid.example//tmp/data";
    if (mkdir((config_dir + "/tmp").c_str(), 0700) != 0)
    {
        Cleanup(cache_path, config_dir);
        return 4;
    }

    std::ofstream config(config_path);
    config << "all.export /tmp\n"
           << "oss.localroot " << config_dir << "\n"
           << "pfc.diskusage 0.80 0.90\n";
    config.close();

    void *plugin = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!plugin)
    {
        std::fprintf(stderr, "dlopen: %s\n", dlerror());
        Cleanup(cache_path, config_dir);
        return 5;
    }

    auto get_cache = reinterpret_cast<XrdOucCache_t>(
        dlsym(plugin, "XrdOucGetCache"));
    if (!get_cache)
    {
        std::fprintf(stderr, "dlsym: %s\n", dlerror());
        Cleanup(cache_path, config_dir);
        return 6;
    }

    const int log_fd = open("/dev/null", O_WRONLY);
    if (log_fd < 0)
    {
        Cleanup(cache_path, config_dir);
        return 7;
    }

    XrdSysLogger logger(log_fd, 0);
    XrdOucEnv environment;
    XrdOucCache *cache = get_cache(&logger, config_path.c_str(), nullptr,
                                   &environment);
    if (!cache)
    {
        Cleanup(cache_path, config_dir);
        return 8;
    }

    EagainOrigin origin(origin_url);
    XrdOucCacheIO *reader = cache->Attach(&origin);
    if (reader == &origin)
    {
        Cleanup(cache_path, config_dir);
        return 9;
    }

    char buffer[4096];
    const int result = reader->Read(buffer, 0, sizeof(buffer));
    Cleanup(cache_path, config_dir);

    if (result != -EAGAIN)
    {
        std::fprintf(stderr, "expected -EAGAIN, got %d\n", result);
        return 10;
    }
    return 0;
}
