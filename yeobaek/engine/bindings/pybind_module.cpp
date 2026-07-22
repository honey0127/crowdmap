// ─────────────────────────────────────────────────────────────────────────────
// [신규] yeobaek_engine — Python 바인딩 (절차서 Phase 2-4)
//
// FastAPI(server/) 가 오케스트레이션에서 호출하는 C++ 표면:
//   SpatialIndex.query_radius(lat, lng, km)            -> list[int]   (모듈1)
//   ForecastProvider.get(area, arrival_unix)           -> ForecastResult (모듈2)
//   Scheduler.optimize(stops, start_unix, w, sub)      -> Plan        (모듈2)
//   SeoulCityDataForecastClient.resolve_now(lat, lng)  -> (level,ratio,valid) (모듈4)
//
// Thread-safety: 네트워크/계산 구간에서 GIL 을 해제(gil_scoped_release)해
//                동시 요청 처리량을 확보한다. C++ 코드는 Python 을 호출하지 않는다.
// ─────────────────────────────────────────────────────────────────────────────
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <memory>

#include "../external/SeoulCityDataForecastClient.h"
#include "../congestion/ForecastProvider.h"
#include "../spatial/SpatialIndex.h"
#include "../scheduler/Scheduler.h"

namespace py = pybind11;

PYBIND11_MODULE(yeobaek_engine, m) {
    m.doc() = "여백(Yeobaek) C++ 엔진: 반경검색 · 도착시점 예보 · 시간의존 스케줄러";

    // ── ForecastResult ──
    py::class_<ForecastResult>(m, "ForecastResult")
        .def_readonly("level", &ForecastResult::level)
        .def_readonly("fcst_unix", &ForecastResult::fcstUnixTime)
        .def_readonly("ppltn_min", &ForecastResult::ppltnMin)
        .def_readonly("ppltn_max", &ForecastResult::ppltnMax)
        .def_readonly("valid", &ForecastResult::valid)
        .def_property_readonly("ratio", [](const ForecastResult& r){ return r.level / 4.0; })
        .def("__repr__", [](const ForecastResult& r){
            return "<ForecastResult level=" + std::to_string(r.level)
                 + " valid=" + (r.valid ? "True" : "False") + ">";
        });

    // ── SpatialIndex (모듈1 반경 후보) ──
    py::class_<SpatialIndex>(m, "SpatialIndex")
        .def(py::init<>())
        .def("add", &SpatialIndex::add, py::arg("content_id"), py::arg("lat"), py::arg("lng"))
        .def("add_many", [](SpatialIndex& self,
                            const std::vector<std::tuple<int64_t,double,double>>& pts){
                py::gil_scoped_release rel;
                for (auto& [id, lat, lng] : pts) self.add(id, lat, lng);
            }, py::arg("points"), "일괄 적재: [(content_id, lat, lng), ...]")
        .def("query_radius", &SpatialIndex::queryRadius,
             py::arg("lat"), py::arg("lng"), py::arg("radius_km"),
             py::call_guard<py::gil_scoped_release>())
        .def("clear", &SpatialIndex::clear)
        .def("size", &SpatialIndex::size);

    // ── SeoulCityDataForecastClient (예보 원천 + 모듈4 실시간) ──
    py::class_<SeoulCityDataForecastClient, std::shared_ptr<SeoulCityDataForecastClient>>(
            m, "SeoulCityDataForecastClient")
        .def(py::init<std::string>(), py::arg("api_key"))
        .def("get_forecast_by_area", &SeoulCityDataForecastClient::getForecastByArea,
             py::arg("area_name"), py::arg("arrival_unix"),
             py::call_guard<py::gil_scoped_release>())
        .def("nearest_area", [](SeoulCityDataForecastClient& c, double lat, double lng){
                auto na = c.nearestArea(lat, lng);
                return py::make_tuple(na.name, na.distKm, na.found);
            }, py::arg("lat"), py::arg("lng"),
            "가장 가까운 서울 121 예보지점 -> (name, dist_km, found)  [build_area_map 용]")
        // 모듈4: router.resolve_now(lat,lng) -> (level, ratio, valid)
        .def("resolve_now", [](SeoulCityDataForecastClient& c, double lat, double lng){
                auto r = c.getCongestion(lat, lng);
                return py::make_tuple(r.level, r.level / 4.0, r.valid);
            }, py::arg("lat"), py::arg("lng"),
            py::call_guard<py::gil_scoped_release>());

    // ── ForecastProvider (모듈2 예보 캐시) ──
    py::class_<ForecastProvider>(m, "ForecastProvider")
        .def(py::init<std::shared_ptr<SeoulCityDataForecastClient>, int, int, int, int>(),
             py::arg("client"), py::arg("bucket_seconds") = 3600,
             py::arg("ok_ttl_sec") = 600, py::arg("fallback_ttl_sec") = 60,
             py::arg("max_size") = 4096)
        .def("get", &ForecastProvider::get, py::arg("area_name"), py::arg("arrival_unix"),
             py::call_guard<py::gil_scoped_release>());

    // ── Scheduler 입출력 타입 ──
    py::class_<TwinCandidate>(m, "TwinCandidate")
        .def(py::init<>())
        .def_readwrite("content_id", &TwinCandidate::contentId)
        .def_readwrite("lat", &TwinCandidate::lat)
        .def_readwrite("lng", &TwinCandidate::lng)
        .def_readwrite("area_name", &TwinCandidate::areaName)
        .def_readwrite("similarity", &TwinCandidate::similarity);

    py::class_<SchedStop>(m, "SchedStop")
        .def(py::init<>())
        .def_readwrite("content_id", &SchedStop::contentId)
        .def_readwrite("lat", &SchedStop::lat)
        .def_readwrite("lng", &SchedStop::lng)
        .def_readwrite("area_name", &SchedStop::areaName)
        .def_readwrite("dwell_sec", &SchedStop::dwellSec)
        .def_readwrite("twins", &SchedStop::twins);

    py::class_<SchedWeights>(m, "SchedWeights")
        .def(py::init<>())
        .def(py::init([](double c, double d, double s){ return SchedWeights{c, d, s}; }),
             py::arg("congestion"), py::arg("distance"), py::arg("similarity"))
        .def_readwrite("congestion", &SchedWeights::congestion)
        .def_readwrite("distance", &SchedWeights::distance)
        .def_readwrite("similarity", &SchedWeights::similarity);

    py::class_<PlanStop>(m, "PlanStop")
        .def_readonly("content_id", &PlanStop::contentId)
        .def_readonly("substituted_from", &PlanStop::substitutedFrom)
        .def_readonly("area_name", &PlanStop::areaName)
        .def_readonly("arrival_unix", &PlanStop::arrivalUnix)
        .def_readonly("forecast_level", &PlanStop::forecastLevel)
        .def_readonly("lat", &PlanStop::lat)
        .def_readonly("lng", &PlanStop::lng)
        .def_readonly("similarity", &PlanStop::similarity);

    py::class_<Plan>(m, "Plan")
        .def_readonly("ordered", &Plan::ordered)
        .def_readonly("total_cost", &Plan::totalCost)
        .def_readonly("saved_congestion_pct", &Plan::savedCongestionPct)
        .def_readonly("valid", &Plan::valid);

    // ── Scheduler (모듈2) ──
    py::class_<Scheduler>(m, "Scheduler")
        .def(py::init<ForecastProvider*, double, double, int>(),
             py::arg("provider"), py::arg("avg_speed_kmh") = 25.0, py::arg("d_max_km") = 50.0,
             py::arg("high_congestion_level") = 3,
             py::keep_alive<1, 2>())   // scheduler 가 provider 를 살려둔다
        .def("optimize", &Scheduler::optimize,
             py::arg("stops"), py::arg("start_unix"), py::arg("weights"),
             py::arg("allow_substitution") = true,
             py::call_guard<py::gil_scoped_release>());
}
