#pragma once
// Pure revision rule for the core bridge worker: no WinRT, no OS headers, so the
// tests can build it without the core DLL. Not thread-safe; the worker owns it.
#include <cstdint>

namespace telaviva {

// Decides whether a snapshot revision still needs to be fetched or published. The first revision always passes.
class RevisionGate {
public:
    // True when a wake that carries `woken` may bring something new.
    bool shouldFetch(std::uint64_t woken) const
    {
        return !published_ || woken > last_;
    }

    // True when a fetched snapshot with `fetched` has not been published yet.
    bool shouldPublish(std::uint64_t fetched) const
    {
        return !published_ || fetched > last_;
    }

    // Records the revision that was just published.
    void markPublished(std::uint64_t revision)
    {
        published_ = true;
        last_ = revision;
    }

private:
    bool published_ = false;
    std::uint64_t last_ = 0;
};

}  // namespace telaviva
