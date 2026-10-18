#include "TestHarness.h"

#include "../Platform/RevisionGate.h"

#include <cstdint>

using telaviva::RevisionGate;

TEST(revision_gate_fresh_gate_passes_any_revision)
{
    RevisionGate gate;
    CHECK(gate.shouldFetch(0));
    CHECK(gate.shouldPublish(0));
    CHECK(gate.shouldFetch(5));
    CHECK(gate.shouldPublish(5));
}

TEST(revision_gate_rejects_published_and_older_revisions)
{
    RevisionGate gate;
    gate.markPublished(5);
    CHECK(!gate.shouldFetch(5));
    CHECK(!gate.shouldPublish(5));
    CHECK(!gate.shouldFetch(4));
    CHECK(!gate.shouldPublish(4));
    CHECK(gate.shouldFetch(6));
    CHECK(gate.shouldPublish(6));
}

TEST(revision_gate_zero_is_a_real_revision)
{
    RevisionGate gate;
    gate.markPublished(0);
    CHECK(!gate.shouldFetch(0));
    CHECK(!gate.shouldPublish(0));
    CHECK(gate.shouldFetch(1));
    CHECK(gate.shouldPublish(1));
}

TEST(revision_gate_repeated_wake_after_publish_is_rejected)
{
    // Wake 3, fetch, publish 3; a second wake for 3 must not fetch or publish again.
    RevisionGate gate;
    CHECK(gate.shouldFetch(3));
    CHECK(gate.shouldPublish(3));
    gate.markPublished(3);
    CHECK(!gate.shouldFetch(3));
    CHECK(!gate.shouldPublish(3));
}

TEST(revision_gate_fetch_newer_than_wake_publishes_then_rejects_it)
{
    // Wake 3, but the fetch returned 4: 4 is published, and later wakes for 4 or 3 are rejected.
    RevisionGate gate;
    CHECK(gate.shouldFetch(3));
    CHECK(gate.shouldPublish(4));
    gate.markPublished(4);
    CHECK(!gate.shouldFetch(4));
    CHECK(!gate.shouldPublish(4));
    CHECK(!gate.shouldFetch(3));
}
