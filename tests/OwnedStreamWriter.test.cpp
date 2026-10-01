#include "base/net/tools/OwnedStreamWriter.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using xmrig::OwnedStreamWriter;

namespace {

void check(bool ok, const char *expression, int line)
{
    if (!ok) {
        std::fprintf(stderr, "line %d: %s failed\n", line, expression);
        std::abort();
    }
}

#define CHECK(expression) check((expression), #expression, __LINE__)

struct Call {
    uv_write_t *request;
    uv_write_cb callback;
    const char *data;
    size_t size;
};

std::vector<Call> calls;
int nextError = 0;

int fakeWrite(uv_write_t *request, uv_stream_t *, const uv_buf_t buffers[], unsigned int count, uv_write_cb callback)
{
    CHECK(count == 1);
    if (nextError) {
        const int error = nextError;
        nextError = 0;
        return error;
    }

    calls.push_back({request, callback, buffers[0].base, buffers[0].len});
    return 0;
}

void complete(int status)
{
    CHECK(!calls.empty());
    const Call call = calls.front();
    calls.erase(calls.begin());
    call.callback(call.request, status);
}

void testCopyAndLimits()
{
    uv_tcp_t stream{};
    const OwnedStreamWriter::Limits limits{4, 1, 5, 30};
    OwnedStreamWriter writer(reinterpret_cast<uv_stream_t *>(&stream), limits, nullptr, fakeWrite);

    char frame[] = "abc";
    CHECK(writer.write(frame, 3, 100) == OwnedStreamWriter::Result::Accepted);
    frame[0] = 'x';
    CHECK(std::string(calls.front().data, calls.front().size) == "abc");
    CHECK(writer.outstandingBytes() == 3);
    CHECK(writer.outstandingRequests() == 1);
    CHECK(OwnedStreamWriter::globalOutstandingBytes() == 3);
    CHECK(writer.write("de", 2, 101) == OwnedStreamWriter::Result::OverLimit);
    CHECK(!writer.oldestExpired(129));
    CHECK(writer.oldestExpired(130));

    complete(0);
    CHECK(writer.outstandingBytes() == 0);
    CHECK(OwnedStreamWriter::globalOutstandingBytes() == 0);
}

void testGlobalLimitAndImmediateError()
{
    uv_tcp_t stream{};
    const OwnedStreamWriter::Limits limits{8, 4, 5, 30};
    OwnedStreamWriter first(reinterpret_cast<uv_stream_t *>(&stream), limits, nullptr, fakeWrite);
    OwnedStreamWriter second(reinterpret_cast<uv_stream_t *>(&stream), limits, nullptr, fakeWrite);
    CHECK(first.write("abcd", 4, 1) == OwnedStreamWriter::Result::Accepted);
    CHECK(second.write("ef", 2, 1) == OwnedStreamWriter::Result::OverLimit);
    complete(0);

    nextError = UV_EPIPE;
    CHECK(second.write("ef", 2, 2) == OwnedStreamWriter::Result::Error);
    CHECK(second.lastError() == UV_EPIPE);
    CHECK(second.outstandingBytes() == 0);
    CHECK(OwnedStreamWriter::globalOutstandingBytes() == 0);
    CHECK(calls.empty());
}

void testFailureAndDetachedLifetime()
{
    uv_tcp_t stream{};
    const OwnedStreamWriter::Limits limits{8, 4, 8, 30};
    int failures = 0;
    {
        OwnedStreamWriter writer(reinterpret_cast<uv_stream_t *>(&stream), limits,
            [&](int status) { CHECK(status == UV_ECANCELED); ++failures; }, fakeWrite);
        CHECK(writer.write("a", 1, 1) == OwnedStreamWriter::Result::Accepted);
        complete(UV_ECANCELED);
        CHECK(failures == 1);
        CHECK(writer.write("b", 1, 2) == OwnedStreamWriter::Result::Accepted);
    }

    complete(UV_ECANCELED);
    CHECK(failures == 1);
    CHECK(OwnedStreamWriter::globalOutstandingBytes() == 0);
}

} // namespace

int main()
{
    testCopyAndLimits();
    testGlobalLimitAndImmediateError();
    testFailureAndDetachedLifetime();
    CHECK(calls.empty());
}
