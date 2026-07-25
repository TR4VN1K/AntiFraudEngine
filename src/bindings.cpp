#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "AntiFraudEngine.h"
#include "Rules.h"

namespace py = pybind11;
using namespace antifraud;

PYBIND11_MODULE(antifraud_core, m) {
    m.doc() =
        "Anti-Fraud Engine: a C++17/20 core for real-time transaction "
        "fraud scoring, exposed to Python via pybind11.";

    py::enum_<ReasonCode>(m, "ReasonCode")
        .value("OK", ReasonCode::kOk)
        .value("HIGH_AMOUNT", ReasonCode::kHighAmount)
        .value("VELOCITY_EXCEEDED", ReasonCode::kVelocityExceeded)
        .value("VOLUME_EXCEEDED", ReasonCode::kVolumeExceeded)
        .value("IMPOSSIBLE_GEO_SPEED", ReasonCode::kImpossibleGeoSpeed)
        .export_values();

    py::class_<Transaction>(m, "Transaction",
        "A single card transaction to be scored.")
        .def(py::init<>())
        .def(py::init([](uint64_t id, std::string card_id, double amount,
                          uint64_t timestamp, std::string country) {
                 Transaction tx;
                 tx.id = id;
                 tx.card_id = std::move(card_id);
                 tx.amount = amount;
                 tx.timestamp = timestamp;
                 tx.country = std::move(country);
                 return tx;
             }),
             py::arg("id"), py::arg("card_id"), py::arg("amount"),
             py::arg("timestamp"), py::arg("country"))
        .def_readwrite("id", &Transaction::id)
        .def_readwrite("card_id", &Transaction::card_id)
        .def_readwrite("amount", &Transaction::amount)
        .def_readwrite("timestamp", &Transaction::timestamp)
        .def_readwrite("country", &Transaction::country)
        .def("__repr__", [](const Transaction& tx) {
            return "<Transaction id=" + std::to_string(tx.id) +
                   " card_id=" + tx.card_id +
                   " amount=" + std::to_string(tx.amount) +
                   " country=" + tx.country + ">";
        });

    py::class_<CheckResult>(m, "CheckResult",
        "Outcome of AntiFraudEngine.check() for one transaction.")
        .def(py::init<>())
        .def_readwrite("is_fraud", &CheckResult::is_fraud)
        .def_readwrite("reason_code", &CheckResult::reason_code)
        .def_readwrite("processing_time_ns", &CheckResult::processing_time_ns)
        .def("__repr__", [](const CheckResult& r) {
            return "<CheckResult is_fraud=" + std::string(r.is_fraud ? "True" : "False") +
                   " reason_code=" + std::to_string(r.reason_code) +
                   " processing_time_ns=" + std::to_string(r.processing_time_ns) + ">";
        });

    py::class_<AntiFraudEngine>(m, "AntiFraudEngine",
        "Thread-safe fraud-scoring engine combining a pipeline of rules "
        "with a per-card sliding-window state cache.")
        .def(py::init<>(), "Construct with sensible default rule thresholds.")
        .def(py::init<double, std::size_t, uint64_t, double, uint64_t, double>(),
             py::arg("high_amount_threshold"),
             py::arg("velocity_max_count"),
             py::arg("velocity_window_seconds"),
             py::arg("volume_max_amount"),
             py::arg("volume_window_seconds"),
             py::arg("geo_max_speed_kmh"),
             "Construct with explicit rule thresholds.")
        .def("check", &AntiFraudEngine::check, py::arg("transaction"),
             py::call_guard<py::gil_scoped_release>(),
             "Run the rule pipeline against a transaction and return a CheckResult. "
             "Releases the GIL while running, so it is safe to call from multiple "
             "Python threads concurrently for higher throughput.")
        .def("reset_state", &AntiFraudEngine::resetState,
             "Clear all cached per-card state (sliding windows, geo history).")
        .def("cached_card_count", &AntiFraudEngine::cachedCardCount,
             "Approximate number of distinct cards currently tracked in the cache.")
        .def("rule_count", &AntiFraudEngine::ruleCount,
             "Number of rules currently registered in the pipeline.");
}
