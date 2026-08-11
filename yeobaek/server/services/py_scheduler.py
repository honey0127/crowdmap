"""엔진(.so) 없을 때의 순수 파이썬 스케줄러 폴백 (모듈2 대체 경로).

engine/scheduler/Scheduler.h 의 optimize()/evaluate() 를 그대로 이식한다 —
비용식·치환 규칙·저장 혼잡도 계산까지 동일 로직. 배포 환경에 C++ 엔진이 없어도
/schedule 이 503 대신 (조금 느리더라도) 실제 결과를 반환하게 하기 위함.

/schedule 의 ScheduleRequest.stops 는 max_length=8 로 고정돼 있어 완전탐색(<=8!)
경로만 실제로 쓰인다 — 2-opt 는 방어적으로 구현만 해둔다(대량 stop 허용 시를 대비).
"""
from __future__ import annotations
import itertools
from dataclasses import dataclass, field

from ..util import haversine_km
from . import py_forecast


@dataclass
class PyPlanStop:
    content_id: int
    substituted_from: int = -1
    area_name: str | None = None
    arrival_unix: int = 0
    forecast_level: int = 1
    lat: float = 0.0
    lng: float = 0.0
    similarity: float = 1.0


@dataclass
class PyPlan:
    ordered: list[PyPlanStop] = field(default_factory=list)
    total_cost: float = 0.0
    saved_congestion_pct: int = 0
    valid: bool = False


_SPEED_KMH = 25.0
_D_MAX_KM = 50.0
_HIGH_LEVEL = 3


def _evaluate(stops: list[dict], order: list[int], start_unix: int,
              w_congestion: float, w_distance: float, w_similarity: float,
              allow_substitution: bool) -> PyPlan:
    plan = PyPlan()
    sum_cong = 0.0
    total_dist_km = 0.0
    sim_penalty = 0.0
    clock = start_unix
    prev_lat = prev_lng = 0.0
    has_prev = False
    used_ids: set[int] = set()

    for idx in order:
        s = stops[idx]
        opts = [{
            "id": s["content_id"], "from": -1, "lat": s["lat"], "lng": s["lng"],
            "area": s.get("area_name"), "sim": 1.0,
        }]

        if allow_substitution and s.get("twins"):
            edge0 = haversine_km(prev_lat, prev_lng, s["lat"], s["lng"]) if has_prev else 0.0
            arr0 = (clock + round(edge0 / max(1.0, _SPEED_KMH) * 3600.0)) if has_prev else start_unix
            if py_forecast.forecast_level(s.get("area_name"), arr0) >= _HIGH_LEVEL:
                for t in s["twins"]:
                    if t["content_id"] in used_ids or t["content_id"] == s["content_id"]:
                        continue
                    opts.append({
                        "id": t["content_id"], "from": s["content_id"],
                        "lat": t["lat"], "lng": t["lng"], "area": t.get("area_name"),
                        "sim": t.get("similarity", 0.0),
                    })

        best_local = None
        best_opt = opts[0]
        best_arrival = clock
        best_level = 1
        best_edge = 0.0

        for o in opts:
            edge_km = haversine_km(prev_lat, prev_lng, o["lat"], o["lng"]) if has_prev else 0.0
            arrival = (clock + round(edge_km / max(1.0, _SPEED_KMH) * 3600.0)) if has_prev else start_unix
            level = py_forecast.forecast_level(o["area"], arrival)
            c = level / 4.0
            substituted = o["from"] != -1
            local = (w_congestion * c
                     + w_distance * (edge_km / _D_MAX_KM)
                     + (w_similarity * (1.0 - o["sim"]) if substituted else 0.0))
            if best_local is None or local < best_local:
                best_local, best_opt, best_arrival = local, o, arrival
                best_level, best_edge = level, edge_km

        ps = PyPlanStop(
            content_id=best_opt["id"], substituted_from=best_opt["from"],
            area_name=best_opt["area"], arrival_unix=best_arrival,
            forecast_level=best_level, lat=best_opt["lat"], lng=best_opt["lng"],
            similarity=best_opt["sim"],
        )
        plan.ordered.append(ps)

        sum_cong += best_level / 4.0
        total_dist_km += best_edge
        if best_opt["from"] != -1:
            sim_penalty += (1.0 - best_opt["sim"])

        used_ids.add(best_opt["id"])
        clock = best_arrival + s.get("dwell_sec", 3600)
        prev_lat, prev_lng, has_prev = best_opt["lat"], best_opt["lng"], True

    plan.total_cost = w_congestion * sum_cong + w_distance * (total_dist_km / _D_MAX_KM) + w_similarity * sim_penalty
    return plan


def optimize(stops: list[dict], start_unix: int, weights: dict,
             allow_substitution: bool, keep_order: bool = False) -> PyPlan:
    n = len(stops)
    if n == 0:
        return PyPlan()

    w_c = weights.get("congestion", 1.0)
    w_d = weights.get("distance", 0.3)
    w_s = weights.get("similarity", 0.5)
    order = list(range(n))

    if keep_order:
        best_plan = _evaluate(stops, order, start_unix, w_c, w_d, w_s, allow_substitution)
    elif n <= 8:
        best_plan, best_cost = None, float("inf")
        for perm in itertools.permutations(order):
            p = _evaluate(stops, list(perm), start_unix, w_c, w_d, w_s, allow_substitution)
            if p.total_cost < best_cost:
                best_cost, best_plan = p.total_cost, p
    else:
        cur = order[:]
        best_plan = _evaluate(stops, cur, start_unix, w_c, w_d, w_s, allow_substitution)
        best_cost = best_plan.total_cost
        improved, guard = True, 0
        while improved and guard < 50:
            guard += 1
            improved = False
            for i in range(n - 1):
                if improved:
                    break
                for k in range(i + 1, n):
                    cand = cur[:i] + list(reversed(cur[i:k + 1])) + cur[k + 1:]
                    p = _evaluate(stops, cand, start_unix, w_c, w_d, w_s, allow_substitution)
                    if p.total_cost + 1e-9 < best_cost:
                        best_cost, best_plan, cur = p.total_cost, p, cand
                        improved = True
                        break

    baseline = _evaluate(stops, order, start_unix, w_c, w_d, w_s, False)
    base_sum = sum(s.forecast_level for s in baseline.ordered)
    opt_sum = sum(s.forecast_level for s in best_plan.ordered)
    saved = round((base_sum - opt_sum) / base_sum * 100.0) if base_sum > 0 else 0
    best_plan.saved_congestion_pct = max(0, saved)
    best_plan.valid = True
    return best_plan
