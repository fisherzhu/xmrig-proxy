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
std::atomic<size_t> g_peakOwnedBytes{0};
std::atomic<uint64_t> g_acceptedWrites{0};
std::atomic<uint64_t> g_completedWrites{0};
std::atomic<uint64_t> g_overLimitWrites{0};
std::atomic<uint64_t> g_closedWrites{0};
std::atomic<uint64_t> g_immediateErrors{0};
std::atomic<uint64_t> g_callbackErrors{0};
std::atomic<uint64_t> g_callbackCancelled{0};
std::atomic<uint64_t> g_ageCloses{0};
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
    int lastError = 0;
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

    if (status == UV_ECANCELED) {
        g_callbackCancelled.fetch_add(1);
    }
    else if (status < 0) {
        g_callbackErrors.fetch_add(1);
    }
    else {
        g_completedWrites.fetch_add(1);
    }

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
    state->lastError = 0;
    if (!state->active || !state->stream) {
        g_closedWrites.fetch_add(1);
        return Result::Closed;
    }

    if (!data || size == 0) {
        g_immediateErrors.fetch_add(1);
        return Result::Error;
    }

    const size_t global = g_ownedBytes.load();
    if (size > std::numeric_limits<unsigned int>::max()
        || state->pending.size() >= state->limits.maxRequests
        || size > state->limits.maxBytes - std::min(state->ownedBytes, state->limits.maxBytes)
        || size > state->limits.maxGlobalBytes - std::min(global, state->limits.maxGlobalBytes)) {
        g_overLimitWrites.fetch_add(1);
        return Result::OverLimit;
    }

    std::unique_ptr<PendingWrite> pendingOwner(new PendingWrite);
    auto *pending = pendingOwner.get();
    pending->bytes.assign(data, size);
    pending->state = state;
    pending->createdAt = now;
    pending->buffer = uv_buf_init(&pending->bytes[0], static_cast<unsigned int>(size));
    pending->request.data = pending;
    state->pending.push_back(pending);
    pending->position = --state->pending.end();
    state->ownedBytes += size;
    const size_t total = g_ownedBytes.fetch_add(size) + size;
    size_t peak = g_peakOwnedBytes.load();
    while (total > peak && !g_peakOwnedBytes.compare_exchange_weak(peak, total)) {}
    pendingOwner.release();

    const int rc = state->writer(&pending->request, state->stream, &pending->buffer, 1, onWrite);
    if (rc < 0) {
        state->lastError = rc;
        g_immediateErrors.fetch_add(1);
        release(pending);
        return Result::Error;
    }

    g_acceptedWrites.fetch_add(1);
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

int OwnedStreamWriter::lastError() const
{
    return m_state->lastError;
}

const char *OwnedStreamWriter::resultName(Result result)
{
    switch (result) {
    case Result::Accepted: return "accepted";
    case Result::Closed: return "closed";
    case Result::OverLimit: return "over_limit";
    case Result::Error: return "write_error";
    }

    return "unknown";
}

size_t OwnedStreamWriter::globalOutstandingBytes()
{
    return g_ownedBytes.load();
}

size_t OwnedStreamWriter::peakOutstandingBytes() { return g_peakOwnedBytes.load(); }
uint64_t OwnedStreamWriter::acceptedWrites() { return g_acceptedWrites.load(); }
uint64_t OwnedStreamWriter::completedWrites() { return g_completedWrites.load(); }
uint64_t OwnedStreamWriter::overLimitWrites() { return g_overLimitWrites.load(); }
uint64_t OwnedStreamWriter::closedWrites() { return g_closedWrites.load(); }
uint64_t OwnedStreamWriter::immediateErrors() { return g_immediateErrors.load(); }
uint64_t OwnedStreamWriter::callbackErrors() { return g_callbackErrors.load(); }
uint64_t OwnedStreamWriter::callbackCancelled() { return g_callbackCancelled.load(); }
uint64_t OwnedStreamWriter::ageCloses() { return g_ageCloses.load(); }
void OwnedStreamWriter::noteAgeClose() { g_ageCloses.fetch_add(1); }

} // namespace xmrig
