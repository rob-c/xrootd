#include "XrdPfc/XrdPfcFile.hh"
#include "XrdPfc/XrdPfcPathParseTools.hh"

#include <gtest/gtest.h>

#include <cerrno>

class PathParseToolTest : public ::testing::Test {
protected:
    std::vector<std::string> dirs { "vultures", "nest", "quite", "high", "in", "a",
                                    "directory", "tree" };
    std::string file { "an_egg.gzip" };
    std::string path { "" };

    const int n_dirs = dirs.size();

    void clear_path() { path = ""; }
    std::string get_lfn() { return path + "/" + file; }
};

using namespace XrdPfc;

// End users reach the same result handling through ordinary reads, vector
// reads, page reads, and block-file reads.  Stock XRootD fails the completed
// EAGAIN case because its pending marker has the same value; the other rows
// document when these callers must wait and when they must return immediately.
TEST(ReadPendingSentinelTest, CompletionStateAndResultAreUnambiguous)
{
    struct ReadState
    {
        const char *description;
        long long bytes_read;
        int error;
        int pending_chunks;
        bool synchronous_work_done;
        bool direct_read_done;
        bool complete;
        int expected_result;
    };

    const ReadState states[] = {
        {"completed retry error",       0, -EAGAIN,    0, true,  true,  true,  -EAGAIN},
        {"completed I/O error",       512, -EIO,       0, true,  true,  true,  -EIO},
        {"completed timeout",           0, -ETIMEDOUT, 0, true,  true,  true,  -ETIMEDOUT},
        {"completed empty read",        0, 0,          0, true,  true,  true,  0},
        {"completed successful read", 4096, 0,         0, true,  true,  true,  4096},
        {"synchronous work remains",    0, 0,          0, false, true,  false, kReadPending},
        {"cache chunk remains",         0, 0,          1, true,  true,  false, kReadPending},
        {"direct read remains",         0, 0,          0, true,  false, false, kReadPending},
        {"all work remains",            0, 0,          1, false, false, false, kReadPending},
    };

    for (const ReadState &state : states)
    {
        SCOPED_TRACE(state.description);

        ReadRequest request(nullptr, nullptr);
        request.m_bytes_read = state.bytes_read;
        if (state.error)
            request.update_error_cond(state.error);
        request.m_n_chunk_reqs = state.pending_chunks;
        request.m_sync_done = state.synchronous_work_done;
        request.m_direct_done = state.direct_read_done;

        ASSERT_EQ(state.complete, request.is_complete());
        const int result = request.is_complete() ? request.return_value() : kReadPending;
        EXPECT_EQ(state.expected_result, result);
        if (state.complete)
            EXPECT_NE(kReadPending, result);
        else
            EXPECT_EQ(kReadPending, result);
    }
}

TEST_F(PathParseToolTest, SplitParser)
{
    for (int i = 0; i < n_dirs; ++i)
    {
        path += "/" + dirs[i];
        SplitParser sp(get_lfn(), "/");
        int n_tokens = i + 2;
        EXPECT_EQ(sp.pre_count_n_tokens(), n_tokens);
        for (int it = 0; it < i + 1; ++it)
        {
            EXPECT_EQ(sp.get_token_as_string(), dirs[it]);
        }
        EXPECT_EQ(std::string(sp.get_reminder()), file);
    }
}

TEST_F(PathParseToolTest, PathTokenizer)
{
    int  max_depth;
    bool parse_as_lfn;

    parse_as_lfn = true;
    {
        // Separate test for files in root directory.
        max_depth = 1024;
        PathTokenizer pt(get_lfn(), max_depth, parse_as_lfn);
        ASSERT_EQ(pt.m_n_dirs, 0);
        ASSERT_EQ(std::string(pt.m_reminder), file);
    }
    for (int i = 0; i < n_dirs; ++i)
    {
        path += "/" + dirs[i];
        {
            max_depth = 1024;
            PathTokenizer pt(get_lfn(), max_depth, parse_as_lfn);
            ASSERT_EQ(pt.m_n_dirs, i + 1);
            ASSERT_EQ(std::string(pt.m_reminder), file);
        }
        {
            max_depth = 0;
            PathTokenizer pt(get_lfn(), max_depth, parse_as_lfn);
            ASSERT_EQ(pt.m_n_dirs, 0);
            ASSERT_EQ(std::string(pt.m_reminder), get_lfn());
        }
    }
    clear_path();

    parse_as_lfn = false;
    for (int i = 0; i < n_dirs; ++i)
    {
        path += "/" + dirs[i];
        {
            max_depth = 1024;
            PathTokenizer pt(get_lfn(), max_depth, parse_as_lfn);
            ASSERT_EQ(pt.m_n_dirs, i + 2);
            ASSERT_EQ(std::string(pt.m_reminder), std::string(""));
        }
        {
            max_depth = 0;
            PathTokenizer pt(get_lfn(), max_depth, parse_as_lfn);
            ASSERT_EQ(pt.m_n_dirs, 0);
            ASSERT_EQ(std::string(pt.m_reminder), get_lfn());
        }
    }
    clear_path();
}
