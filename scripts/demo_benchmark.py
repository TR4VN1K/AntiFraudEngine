#!/usr/bin/env python3
"""
demo_benchmark.py
==================

Demonstration & micro-benchmark script for the Anti-Fraud Engine
(``antifraud_core``, a pybind11 extension module built from the C++
core in this repository).

Run after building the extension (see README.md):

    python3 scripts/demo_benchmark.py

The script does three things:

1. Throughput benchmark -- generates 100,000+ synthetic "normal"
   transactions in Python and measures how fast the C++ engine can
   score them via ``engine.check()``.
2. Rule-triggering scenarios -- crafts small, deliberate anomalies
   (a high-amount purchase, a velocity spam burst, a volume breach,
   and an impossible-geo-speed pair of transactions) and verifies the
   engine flags each one with the expected reason code.
3. A short summary report printed to stdout.
"""

from __future__ import annotations

import random
import statistics
import sys
import time
from dataclasses import dataclass

try:
    import antifraud_core as af
except ImportError as exc:  # pragma: no cover
    sys.stderr.write(
        "Could not import antifraud_core. Build the extension first, e.g.:\n\n"
        "    mkdir build && cd build\n"
        "    cmake -Dpybind11_DIR=$(python3 -m pybind11 --cmakedir) ..\n"
        "    make -j\n\n"
        "and run this script from the build directory, or add it to PYTHONPATH.\n"
    )
    raise


COUNTRIES = ["DE", "FR", "GB", "US", "IT", "ES", "NL", "PL", "CA", "SG"]
REASON_NAMES = {
    af.ReasonCode.OK: "OK",
    af.ReasonCode.HIGH_AMOUNT: "HIGH_AMOUNT",
    af.ReasonCode.VELOCITY_EXCEEDED: "VELOCITY_EXCEEDED",
    af.ReasonCode.VOLUME_EXCEEDED: "VOLUME_EXCEEDED",
    af.ReasonCode.IMPOSSIBLE_GEO_SPEED: "IMPOSSIBLE_GEO_SPEED",
}


def reason_name(code: int) -> str:
    return REASON_NAMES.get(code, f"UNKNOWN({code})")



# 1. Throughput benchmark


def generate_normal_transactions(n: int, base_timestamp: int, num_cards: int = 5000):
    """Generates n synthetic, mostly-benign transactions spread across
    num_cards distinct cards. Amounts and timing are randomized but kept
    within ranges that should NOT trip the default rule thresholds, so
    the benchmark measures steady-state throughput rather than the
    (slightly) different cost of a flagged path.

    Two things matter for keeping this "benign" against the default
    rules:
      - each card has a fixed "home" country (occasional travel is
        allowed, but with realistic multi-hour gaps), so GeoSpeedRule
        doesn't spuriously trigger on random country jitter;
      - the global transaction stream advances one second per
        transaction, so with num_cards >> 1 the average gap between two
        transactions on the *same* card comfortably exceeds the default
        velocity (60s) and volume (3600s) windows.
    """
    card_home_country = {
        i: random.choice(COUNTRIES) for i in range(num_cards)
    }

    transactions = []
    for i in range(n):
        card_idx = random.randint(0, num_cards - 1)
        card_id = f"card_{card_idx}"
        amount = round(random.uniform(1.0, 500.0), 2)
        timestamp = base_timestamp + i
        # ~0.1% of transactions simulate genuine travel: a different
        # country than the card's home, still a plausible trip since the
        # gap to this card's previous transaction is on the order of
        # num_cards seconds (hours), not minutes.
        country = (
            random.choice(COUNTRIES)
            if random.random() < 0.001
            else card_home_country[card_idx]
        )
        transactions.append(
            af.Transaction(
                id=i,
                card_id=card_id,
                amount=amount,
                timestamp=timestamp,
                country=country,
            )
        )
    return transactions


def run_throughput_benchmark(engine: "af.AntiFraudEngine", n: int = 200_000) -> None:
    print("=" * 72)
    print(f"THROUGHPUT BENCHMARK -- {n:,} transactions")
    print("=" * 72)

    base_ts = int(time.time())
    print("Generating synthetic transactions in Python...")
    gen_start = time.perf_counter()
    transactions = generate_normal_transactions(n, base_ts)
    gen_elapsed = time.perf_counter() - gen_start
    print(f"  Generated {n:,} transactions in {gen_elapsed:.3f}s "
          f"({n / gen_elapsed:,.0f} tx/s generation rate)\n")

    print("Scoring transactions through the C++ engine (engine.check)...")
    per_call_ns = []
    start = time.perf_counter()
    flagged = 0
    for tx in transactions:
        result = engine.check(tx)
        if result.is_fraud:
            flagged += 1
        per_call_ns.append(result.processing_time_ns)
    elapsed = time.perf_counter() - start

    throughput = n / elapsed
    mean_ns = statistics.mean(per_call_ns)
    p50_ns = statistics.median(per_call_ns)
    p99_ns = sorted(per_call_ns)[int(0.99 * len(per_call_ns)) - 1]

    print(f"\n  Wall-clock time (incl. Python call overhead): {elapsed:.3f}s")
    print(f"  Effective throughput:                         {throughput:,.0f} tx/s")
    print(f"  Flagged as fraud:                             {flagged:,} "
          f"({100.0 * flagged / n:.2f}%)")
    print("\n  Per-call processing_time_ns reported by the C++ engine itself")
    print("  (isolates engine.check() cost from Python loop/call overhead):")
    print(f"    mean: {mean_ns:,.0f} ns   median: {p50_ns:,.0f} ns   p99: {p99_ns:,.0f} ns")
    print(f"  Cards currently tracked in cache: {engine.cached_card_count():,}")
    print()



# 2. Rule-triggering scenarios


@dataclass
class Scenario:
    name: str
    expected_reason: int
    build: "callable"


def scenario_high_amount(engine: "af.AntiFraudEngine"):
    tx = af.Transaction(id=1, card_id="scn_high_amount", amount=25_000.0,
                         timestamp=int(time.time()), country="DE")
    return [engine.check(tx)]


def scenario_velocity(engine: "af.AntiFraudEngine"):
    """Fires many small transactions on the same card within a few
    seconds -- a classic card-testing / velocity-spam pattern."""
    now = int(time.time())
    results = []
    for i in range(8):
        tx = af.Transaction(id=100 + i, card_id="scn_velocity", amount=5.0,
                             timestamp=now + i, country="DE")
        results.append(engine.check(tx))
    return results


def scenario_volume(engine: "af.AntiFraudEngine"):
    """Several transactions individually below the high-amount threshold
    and spaced far enough apart (70s) to stay under VelocityRule's 60s
    window, but whose sum within VolumeRule's 1h window exceeds the
    volume limit."""
    now = int(time.time())
    results = []
    for i in range(6):
        tx = af.Transaction(id=200 + i, card_id="scn_volume", amount=4_000.0,
                             timestamp=now + i * 70, country="DE")
        results.append(engine.check(tx))
    return results


def scenario_geo_speed(engine: "af.AntiFraudEngine"):
    """Two purchases a few minutes apart on opposite sides of the globe --
    physically implausible travel speed between them."""
    now = int(time.time())
    tx1 = af.Transaction(id=300, card_id="scn_geo", amount=50.0,
                          timestamp=now, country="DE")
    tx2 = af.Transaction(id=301, card_id="scn_geo", amount=50.0,
                          timestamp=now + 300, country="AU")  # 5 minutes later
    return [engine.check(tx1), engine.check(tx2)]


SCENARIOS = [
    Scenario("High amount", af.ReasonCode.HIGH_AMOUNT, scenario_high_amount),
    Scenario("Velocity spam", af.ReasonCode.VELOCITY_EXCEEDED, scenario_velocity),
    Scenario("Volume breach", af.ReasonCode.VOLUME_EXCEEDED, scenario_volume),
    Scenario("Impossible geo speed", af.ReasonCode.IMPOSSIBLE_GEO_SPEED, scenario_geo_speed),
]


def run_scenarios() -> bool:
    print("=" * 72)
    print("RULE-TRIGGERING SCENARIOS")
    print("=" * 72)

    all_ok = True
    for scenario in SCENARIOS:
        # Fresh engine per scenario so scenarios don't interfere with
        # each other's per-card state.
        engine = af.AntiFraudEngine()
        results = scenario.build(engine)
        flagged_results = [r for r in results if r.is_fraud]

        triggered = bool(flagged_results)
        matched_reason = (
            triggered and flagged_results[-1].reason_code == scenario.expected_reason
        )
        status = "PASS" if (triggered and matched_reason) else "FAIL"
        if status == "FAIL":
            all_ok = False

        last = results[-1]
        print(f"[{status}] {scenario.name:<24s} "
              f"expected={reason_name(scenario.expected_reason):<20s} "
              f"got={reason_name(last.reason_code):<20s} "
              f"is_fraud={last.is_fraud}")

    print()
    return all_ok



# Entry point


def main() -> int:
    random.seed(42)

    print(f"antifraud_core module loaded from: {af.__file__}\n")

    scenarios_ok = run_scenarios()

    bench_engine = af.AntiFraudEngine()
    n = 200_000
    if len(sys.argv) > 1:
        n = int(sys.argv[1])
    run_throughput_benchmark(bench_engine, n=n)

    print("=" * 72)
    print("SUMMARY:", "all scenarios passed" if scenarios_ok else "SOME SCENARIOS FAILED")
    print("=" * 72)

    return 0 if scenarios_ok else 1


if __name__ == "__main__":
    sys.exit(main())
