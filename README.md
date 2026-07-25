# Anti-Fraud Engine

A small, self-contained **C++17/20** engine for real-time transaction fraud
scoring, exposed to **Python** through [pybind11](https://github.com/pybind/pybind11).

Built as a portfolio project to demonstrate: modern C++ design (RAII, smart
pointers, templates), a hand-rolled concurrent data structure (striped
locking), a pluggable rule-engine architecture, and clean Python
interoperability — with correctness backed by ThreadSanitizer/ASan/UBSan runs
rather than just "it compiles."

```
$ python3 scripts/demo_benchmark.py 200000
...
[PASS] High amount              expected=HIGH_AMOUNT          got=HIGH_AMOUNT
[PASS] Velocity spam            expected=VELOCITY_EXCEEDED    got=VELOCITY_EXCEEDED
[PASS] Volume breach            expected=VOLUME_EXCEEDED      got=VOLUME_EXCEEDED
[PASS] Impossible geo speed     expected=IMPOSSIBLE_GEO_SPEED got=IMPOSSIBLE_GEO_SPEED

Effective throughput:                         ~700,000 tx/s
Cards currently tracked in cache:             5,000
```

## Table of contents

- [Architecture](#architecture)
- [Repository layout](#repository-layout)
- [Building](#building)
- [Python usage](#python-usage)
- [Rules](#rules)
- [Design notes](#design-notes)
- [Testing](#testing)
- [Roadmap / non-goals](#roadmap--non-goals)

## Architecture

```
                      ┌─────────────────────────┐
Python ── pybind11 ──▶│      AntiFraudEngine     │
                      │  (rule pipeline runner)  │
                      └───────────┬──────────────┘
                                  │ evaluate() for each rule, in order
                    ┌─────────────┼──────────────┬───────────────┐
                    ▼             ▼              ▼               ▼
            HighAmountRule  VelocityRule   VolumeRule      GeoSpeedRule
                    │             │              │               │
                    └─────────────┴──────┬───────┴───────────────┘
                                          ▼
                              ConcurrentMap<card_id, CardState>
                              (striped std::shared_mutex locking)
                                          │
                                          ▼
                          CardState { SlidingWindow<double> × 2,
                                       last_country, last_timestamp }
```

- **`AntiFraudEngine`** owns an ordered list of `IFraudRule`s and a single
  `CardCache`. `check()` runs each rule in registration order and
  short-circuits on the first rule that flags the transaction.
- **Rules** are stateless configuration objects (thresholds only); all
  *mutable* per-card state lives in the shared `CardCache`, so a single rule
  instance can be safely invoked concurrently for different cards.
- **`ConcurrentMap`** partitions the key space into `N` shards, each guarded
  by its own `std::shared_mutex` (striped locking), so unrelated cards never
  contend on the same lock.
- **`SlidingWindow<T>`** is a `std::deque`-backed time window with amortized
  O(1) eviction of stale entries per push.

## Repository layout

```
.
├── CMakeLists.txt              # pybind11_add_module + static core library
├── include/
│   ├── AntiFraudEngine.h       # Engine facade: rule pipeline + cache
│   ├── SlidingWindow.h         # Template: time-based sliding window
│   ├── ConcurrentMap.h         # Template: striped-lock thread-safe map
│   └── Rules.h                 # IFraudRule + 4 concrete rules
├── src/
│   ├── AntiFraudEngine.cpp
│   ├── Rules.cpp
│   └── bindings.cpp            # pybind11 module definition
├── scripts/
│   └── demo_benchmark.py       # Throughput benchmark + rule scenarios
├── tests/
│   └── tsan_stress.cpp         # Pure-C++ concurrency stress test
└── README.md
```

## Building

### Requirements

- A C++20 compiler (GCC ≥ 10, Clang ≥ 12, MSVC ≥ 19.29)
- CMake ≥ 3.15
- Python ≥ 3.8 with development headers
- [pybind11](https://pypi.org/project/pybind11/) (`pip install pybind11`)

### Build steps

```bash
git clone https://github.com/<you>/antifraud-engine.git
cd antifraud-engine

pip install pybind11

mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release \
      -Dpybind11_DIR="$(python3 -m pybind11 --cmakedir)" \
      ..
cmake --build . -j
```

This produces `antifraud_core*.so` (Linux/macOS) or `antifraud_core*.pyd`
(Windows) inside `build/`.

### Running the demo

```bash
# from build/, so Python can find the freshly built extension
PYTHONPATH=. python3 ../scripts/demo_benchmark.py 200000
```

The argument (default `200000`) controls how many synthetic transactions the
throughput benchmark generates.

Alternatively, install the module onto your `PYTHONPATH` (e.g. copy the
`.so`/`.pyd` file into your project, or `cmake --install .` into a directory
already on `PYTHONPATH`).

## Python usage

```python
import antifraud_core as af

engine = af.AntiFraudEngine()  # default thresholds, see below

tx = af.Transaction(
    id=1,
    card_id="4111-XXXX-XXXX-1111",
    amount=15000.0,
    timestamp=1_753_400_000,   # unix seconds
    country="DE",
)

result = engine.check(tx)
print(result.is_fraud, result.reason_code, result.processing_time_ns)
# True 1 187   -> ReasonCode.HIGH_AMOUNT
```

### Custom thresholds

```python
engine = af.AntiFraudEngine(
    high_amount_threshold=5000.0,
    velocity_max_count=3,
    velocity_window_seconds=30,
    volume_max_amount=10000.0,
    volume_window_seconds=1800,
    geo_max_speed_kmh=1000.0,
)
```

### Reason codes

```python
af.ReasonCode.OK                     # 0
af.ReasonCode.HIGH_AMOUNT            # 1
af.ReasonCode.VELOCITY_EXCEEDED      # 2
af.ReasonCode.VOLUME_EXCEEDED        # 3
af.ReasonCode.IMPOSSIBLE_GEO_SPEED   # 4
```

### Concurrency from Python

`AntiFraudEngine.check()` releases the GIL for the duration of the C++ call
(`py::call_guard<py::gil_scoped_release>()`), so it's safe — and useful — to
call it from multiple Python threads (e.g. via `concurrent.futures.ThreadPoolExecutor`)
to score independent cards in parallel.

## Rules

| Rule              | Triggers when...                                                                 | Reason code            |
|-------------------|-----------------------------------------------------------------------------------|-------------------------|
| `HighAmountRule`   | a single transaction's amount exceeds a fixed threshold                          | `HIGH_AMOUNT`           |
| `VelocityRule`     | a card makes more than `max_count` transactions within a rolling window          | `VELOCITY_EXCEEDED`     |
| `VolumeRule`       | a card's cumulative spend within a rolling window exceeds a limit                | `VOLUME_EXCEEDED`       |
| `GeoSpeedRule`     | the implied travel speed between two consecutive countries exceeds a threshold   | `IMPOSSIBLE_GEO_SPEED`  |

Rules run in registration order and the engine short-circuits on the first
match, so `reason_code` always reflects the *first* rule that fired (default
order: HighAmount → Velocity → Volume → GeoSpeed). `GeoSpeedRule` uses a
small built-in table of approximate country centroids and a haversine
distance calculation; unknown country codes are never flagged.

Custom rules can be added from C++ by subclassing `IFraudRule` and calling
`engine.addRule(std::make_unique<MyRule>(...))` — this hook is not currently
exposed to Python, since a Python-implemented rule would have to reacquire
the GIL on every call, defeating the purpose of releasing it in `check()`.

## Design notes

- **Why striped locking instead of one global mutex or one mutex per card?**
  A single global mutex would serialize all traffic regardless of which
  cards are involved. A mutex per card would avoid contention but adds
  unbounded lock allocation and doesn't amortize well for a cache with a
  long tail of rarely-seen cards. Striped locking (a fixed array of `N`
  shard mutexes, shard chosen by `hash(card_id) % N`) is the standard
  middle ground: O(1) memory overhead, and cards hash-distribute across
  shards so unrelated traffic parallelizes well in practice.
- **Why separate sliding windows for `VelocityRule` and `VolumeRule`?**
  Both track "amount over time" but are normally configured with different
  window durations (e.g. 60s vs. 1h). Keeping their windows independent
  means either rule can be added, removed, or reconfigured without
  silently changing the other's behavior.
- **Why does `check()` short-circuit?** Once a transaction is flagged, later
  rules' side effects on `CardState` don't run for that transaction. This
  is a deliberate simplicity/performance trade-off for a portfolio engine;
  a production system might instead run every rule for observability and
  only use the first/highest-severity match for the final decision.
- **No networking, no external services.** Everything here is in-process:
  no sockets, no ports, no database. State lives entirely in memory for the
  lifetime of the `AntiFraudEngine` object; use `reset_state()` to clear it.

## Testing

- `scripts/demo_benchmark.py` doubles as a lightweight correctness check: it
  asserts each of the four rules fires with its expected `reason_code` on a
  crafted scenario, and exits with a non-zero status if any scenario fails.
- `tests/tsan_stress.cpp` is a pure-C++ (no Python) stress test that hammers
  `AntiFraudEngine::check()` from 16 threads against a small, deliberately
  contended pool of `card_id`s. It's meant to be built with sanitizers to
  validate the concurrency design directly, independent of the Python
  binding layer:

  ```bash
  g++ -std=c++20 -fsanitize=thread -g -O1 -Iinclude \
      src/AntiFraudEngine.cpp src/Rules.cpp tests/tsan_stress.cpp \
      -o tsan_stress -lpthread
  ./tsan_stress

  g++ -std=c++20 -fsanitize=address,undefined -g -O1 -Iinclude \
      src/AntiFraudEngine.cpp src/Rules.cpp tests/tsan_stress.cpp \
      -o asan_stress -lpthread
  ./asan_stress
  ```

  Both configurations run clean (no reported races, no memory/UB errors)
  against 320,000 concurrent `check()` calls across 64 shared cards.

## Roadmap / non-goals

Deliberately out of scope for this project, to keep it a focused, readable
portfolio piece rather than a production fraud platform:

- Persistence / durability of card state across process restarts
- Distributed deployment, sharding across machines, or a network API
- Machine-learning-based scoring (this is a rules engine by design)
- A Python-side rule-authoring API

## License

MIT — see [LICENSE](LICENSE).
