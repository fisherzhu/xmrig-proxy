#ifndef XMRIG_OWNEDSTREAMWRITER_H
#define XMRIG_OWNEDSTREAMWRITER_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <uv.h>

namespace xmrig {

class OwnedStreamWriter
{
public:
    enum class Result { Accepted, Closed, OverLimit, Error };

    struct Limits {
        size_t maxBytes;
        size_t maxRequests;
        size_t maxGlobalBytes;
        uint64_t maxAgeMs;
    };

    using WriteFunction = int (*)(uv_write_t *, uv_stream_t *, const uv_buf_t[], unsigned int, uv_write_cb);
    using FailureCallback = std::function<void(int)>;

    OwnedStreamWriter(uv_stream_t *stream, Limits limits, FailureCallback onFailure, WriteFunction write = uv_write);
    ~OwnedStreamWriter();

    OwnedStreamWriter(const OwnedStreamWriter &) = delete;
    OwnedStreamWriter &operator=(const OwnedStreamWriter &) = delete;

    Result write(const char *data, size_t size, uint64_t now);
    bool oldestExpired(uint64_t now) const;
    void detach();

    size_t outstandingBytes() const;
    size_t outstandingRequests() const;
    int lastError() const;
    static const char *resultName(Result result);
    static size_t globalOutstandingBytes();

private:
    struct State;
    struct PendingWrite;
    static void onWrite(uv_write_t *request, int status);
    static void release(PendingWrite *write);
    std::shared_ptr<State> m_state;
};

} // namespace xmrig

#endif // XMRIG_OWNEDSTREAMWRITER_H
