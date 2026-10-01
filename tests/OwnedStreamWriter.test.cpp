#include "base/net/tools/OwnedStreamWriter.h"

#include <cassert>
#include <string>
#include <vector>

using xmrig::OwnedStreamWriter;

namespace {

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
    assert(count == 1);
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
    assert(!calls.empty());
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
    assert(writer.write(frame, 3, 100) == OwnedStreamWriter::Result::Accepted);
    frame[0] = 'x';
    assert(std::string(calls.front().data, calls.front().size) == "abc");
    assert(writer.outstandingBytes() == 3);
    assert(writer.outstandingRequests() == 1);
    assert(OwnedStreamWriter::globalOutstandingBytes() == 3);
    assert(writer.write("de", 2, 101) == OwnedStreamWriter::Result::OverLimit);
    assert(!writer.oldestExpired(129));
    assert(writer.oldestExpired(130));

    complete(0);
    assert(writer.outstandingBytes() == 0);
    assert(OwnedStreamWriter::globalOutstandingBytes() == 0);
}

void testGlobalLimitAndImmediateError()
{
    uv_tcp_t stream{};
    const OwnedStreamWriter::Limits limits{8, 4, 5, 30};
    OwnedStreamWriter first(reinterpret_cast<uv_stream_t *>(&stream), limits, nullptr, fakeWrite);
    OwnedStreamWriter second(reinterpret_cast<uv_stream_t *>(&stream), limits, nullptr, fakeWrite);
    assert(first.write("abcd", 4, 1) == OwnedStreamWriter::Result::Accepted);
    assert(second.write("ef", 2, 1) == OwnedStreamWriter::Result::OverLimit);
    complete(0);

    nextError = UV_EPIPE;
    assert(second.write("ef", 2, 2) == OwnedStreamWriter::Result::Error);
    assert(second.outstandingBytes() == 0);
    assert(OwnedStreamWriter::globalOutstandingBytes() == 0);
    assert(calls.empty());
}

void testFailureAndDetachedLifetime()
{
    uv_tcp_t stream{};
    const OwnedStreamWriter::Limits limits{8, 4, 8, 30};
    int failures = 0;
    {
        OwnedStreamWriter writer(reinterpret_cast<uv_stream_t *>(&stream), limits,
            [&](int status) { assert(status == UV_ECANCELED); ++failures; }, fakeWrite);
        assert(writer.write("a", 1, 1) == OwnedStreamWriter::Result::Accepted);
        complete(UV_ECANCELED);
        assert(failures == 1);
        assert(writer.write("b", 1, 2) == OwnedStreamWriter::Result::Accepted);
    }

    complete(UV_ECANCELED);
    assert(failures == 1);
    assert(OwnedStreamWriter::globalOutstandingBytes() == 0);
}

} // namespace

int main()
{
    testCopyAndLimits();
    testGlobalLimitAndImmediateError();
    testFailureAndDetachedLifetime();
    assert(calls.empty());
}
