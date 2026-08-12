"""여백 서버 설정 — 환경변수 기반. (12-factor)

민감정보(API 키)는 코드가 아니라 환경변수/.env 로만 주입한다.
"""
from __future__ import annotations
import os
from pathlib import Path

# 레포 루트 기준 경로 (yeobaek/)
ROOT = Path(__file__).resolve().parent.parent
DATA_DIR = ROOT / "data"


def _get(name: str, default: str = "") -> str:
    return os.environ.get(name, default)


class Settings:
    # ── 외부 API 키 ──
    SEOUL_API_KEY: str = _get("SEOUL_API_KEY")           # 서울 실시간 도시데이터(서울 혼잡)
    TOURAPI_KEY: str = _get("TOURAPI_KEY")               # 한국관광공사 TourAPI (필수 활용)
    # 전국 관광지 집중률 예측(한국관광공사 TatsCnctrRateService) — 전국 혼잡 소스
    TATS_API_KEY: str = _get("TATS_API_KEY")
    TATS_ENDPOINT: str = _get(
        "TATS_ENDPOINT",
        "https://apis.data.go.kr/B551011/TatsCnctrRateService/tatsCnctrRatedList")

    # 관광지 별 연관 관광지 정보(관광 빅데이터) — 감성 쌍둥이의 공식 근거.
    # 키는 공공데이터포털 서비스키 하나로 공용이지만, 서비스별 '활용신청'이 따로 필요하다.
    # 미설정이면 연관 데이터 없이 임베딩 유사도만으로 동작한다(폴백).
    RLTE_API_KEY: str = _get("RLTE_API_KEY") or _get("TATS_API_KEY") or _get("TOURAPI_KEY")
    # 오퍼레이션(searchKeyword/areaBasedList) 앞까지의 베이스 URL.
    # 기관이 버전을 올리면(…Service1 → …Service2) 이 값만 env 로 바꾸면 된다.
    RLTE_ENDPOINT: str = _get(
        "RLTE_ENDPOINT",
        "https://apis.data.go.kr/B551011/TarRlteTarService1")

    # ── 데이터 경로 ──
    DB_PATH: str = _get("YEOBAEK_DB", str(DATA_DIR / "yeobaek.db"))
    EMB_VECTORS: str = _get("YEOBAEK_EMB", str(DATA_DIR / "embeddings.npy"))
    EMB_IDS: str = _get("YEOBAEK_EMB_IDS", str(DATA_DIR / "embeddings_ids.npy"))

    # ── 스케줄러 기본 가중치 (부록 C 튜닝 시작값) ──
    W_CONGESTION: float = float(_get("YEOBAEK_W_CONGESTION", "1.0"))
    W_DISTANCE: float = float(_get("YEOBAEK_W_DISTANCE", "0.3"))
    W_SIMILARITY: float = float(_get("YEOBAEK_W_SIMILARITY", "0.5"))

    # 카테고리별 기본 체류시간(초) — travel/dwell 근사(결정 D)
    DEFAULT_DWELL_SEC: int = int(_get("YEOBAEK_DWELL", "3600"))

    # 고혼잡 임계(레벨) — 이 값 이상이면 대안 치환 후보를 붙인다
    HIGH_CONGESTION_LEVEL: int = int(_get("YEOBAEK_HIGH_LVL", "3"))

    # 대안 탐색 기본값
    DEFAULT_RADIUS_KM: float = float(_get("YEOBAEK_RADIUS_KM", "5.0"))
    DEFAULT_TOP_K: int = int(_get("YEOBAEK_TOP_K", "3"))

    # ── RAG (결정 G) ──
    USE_LLM: bool = _get("YEOBAEK_USE_LLM", "0") not in ("", "0", "false", "False")
    ANTHROPIC_API_KEY: str = _get("ANTHROPIC_API_KEY")
    # 기본 모델은 최신 Opus. 비용 최적화가 필요하면 env 로 haiku 등으로 교체.
    LLM_MODEL: str = _get("YEOBAEK_LLM_MODEL", "claude-opus-4-8")


settings = Settings()
