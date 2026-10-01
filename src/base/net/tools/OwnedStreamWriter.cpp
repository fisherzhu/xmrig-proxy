#include "base/net/tools/OwnedStreamWriter.h"

#include <algorithm>
#include <atomic>
#include <limits>
#include <list>
#include <string>
#include <utility>

namespace xmrig {

namespace {
std::atomic<size_t> g_ownedBytes{0};
}

struct OwnedStreamWriter::State {
    State(uv_stream_t *stream, Limits limits, FailureCallback callback, WriteFunction writer)
        : stream(stream), limits(limits), onFailure(std::move(callback)), writer(writer) {}

    uv_stream_t *stream;
    Limits limits;
    FailureCallback onFailure;
    WriteFunction writer;
    std::list<PendingWrite *> pending;
    size_t ownedBytes = 0;
    bool active = true;
};

struct OwnedStreamWriter::PendingWrite {
    uv_write_t request{};
    uv_buf_t buffer{};
    std::string bytes;
    std::shared_ptr<State> state;
    std::list<PendingWrite *>::iterator position;
    uint64_t createdAt;
};

void OwnedStreamWriter::release(PendingWrite *write)
{
    const auto state = write->state;
    state->pending.erase(write->position);
    state->ownedBytes -= write->bytes.size();
    g_ownedBytes.fetch_sub(write->bytes.size());
    delete write;
}

void OwnedStreamWriter::onWrite(uv_write_t *request, int status)
{
    auto *write = static_cast<PendingWrite *>(request->data);
    const auto state = write->state;
    release(write);

    if (status < 0 && state->active && state->onFailure) {
        state->onFailure(status);
    }
}

OwnedStreamWriter::OwnedStreamWriter(uv_stream_t *stream, Limits limits, FailureCallback onFailure, WriteFunction write)
    : m_state(std::make_shared<State>(stream, limits, std::move(onFailure), write))
{
}

OwnedStreamWriter::~OwnedStreamWriter()
{
    detach();
}

OwnedStreamWriter::Result OwnedStreamWriter::write(const char *data, size_t size, uint64_t now)
{
    const auto state = m_state;
    if (!state->active || !state->stream) {
        return Result::Closed;
    }

    if (!data || size == 0) {
        return Result::Error;
    }

    const size_t global = g_ownedBytes.load();
    if (size > std::numeric_limits<unsigned int>::max()
        || state->pending.size() >= state->limits.maxRequests
        || size > state->limits.maxBytes - std::min(state->ownedBytes, state->limits.maxBytes)
        || size > state->limits.maxGlobalBytes - std::min(global, state->limits.maxGlobalBytes)) {
        return Result::OverLimit;
    }

    auto *pending = new PendingWrite;
    pending->bytes.assign(data, size);
    pending->state = state;
    pending->createdAt = now;
    pending->buffer = uv_buf_init(&pending->bytes[0], static_cast<unsigned int>(size));
    pending->request.data = pending;
    state->pending.push_back(pending);
    pending->position = --state->pending.end();
    state->ownedBytes += size;
    g_ownedBytes.fetch_add(size);

    const int rc = state->writer(&pending->request, state->stream, &pending->buffer, 1, onWrite);
    if (rc < 0) {
        release(pending);
        return Result::Error;
    }

    return Result::Accepted;
}

bool OwnedStreamWriter::oldestExpired(uint64_t now) const
{
    const auto &state = m_state;
    if (state->pending.empty()) {
        return false;
    }

    const uint64_t oldest = state->pending.front()->createdAt;
    return now >= oldest && (now - oldest) >= state->limits.maxAgeMs;
}

void OwnedStreamWriter::detach()
{
    m_state->active = false;
    m_state->onFailure = nullptr;
}

size_t OwnedStreamWriter::outstandingBytes() const
{
    return m_state->ownedBytes;
}

size_t OwnedStreamWriter::outstandingRequests() const
{
    return m_state->pending.size();
}

size_t OwnedStreamWriter::globalOutstandingBytes()
{
    return g_ownedBytes.load();
}

} // namespace xmrig
