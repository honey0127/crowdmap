"""RAG 설득 카드 생성 (모듈3, 결정 G).

핵심 원칙: **수치는 코드가 계산하고, LLM 은 문장만 옮긴다.**
  - grounded_facts(혼잡도 차이 %, 공유 카테고리)는 여기서 예보값으로 계산한다.
  - LLM 없이도 동작하는 '템플릿 채우기'가 기본값(데모 안정성).
  - YEOBAEK_USE_LLM=1 이고 ANTHROPIC_API_KEY 가 있으면 LLM 이 '재서술'만 한다.
    context 밖 사실 추가를 금지하는 프롬프트를 쓰고, 어떤 실패든 템플릿으로 폴백.
"""
from __future__ import annotations
from typing import Optional

from ..config import settings

LEVEL_LABELS = {1: "여유", 2: "보통", 3: "약간 붐빔", 4: "붐빔"}

# TourAPI 서비스분류코드 → 사람이 읽는 라벨. 카드에 코드가 그대로 노출되지 않도록.
# 깊은 코드(cat3, 9자)부터 얕은 코드(cat1, 3자) 순으로 조회한다.
CAT_LABELS = {
    # 자연 (A01)
    "A01": "자연", "A0101": "자연 명소", "A0102": "관광자원",
    # 인문·문화·역사 (A02) — cat2 로 세분(공원=A0202 휴양)
    "A02": "역사·문화",
    "A0201": "역사 유적",
    "A02010100": "고궁", "A02010200": "성", "A02010400": "고택",
    "A02010600": "민속마을", "A02010700": "유적·사적지",
    "A0202": "공원·휴양",
    "A0203": "체험·명소",
    "A0205": "건축·조형물",
    "A0206": "문화시설(박물관·미술관)",
    "A0207": "축제·공연",
    # 레포츠 (A03) / 쇼핑 (A04) / 음식 (A05)
    "A03": "레포츠", "A0302": "레포츠",
    "A04": "쇼핑", "A0401": "쇼핑·시장",
    "A05": "맛집", "A0502": "맛집",
}


def cat_label(code: Optional[str]) -> Optional[str]:
    """분류코드를 라벨로. 정확 코드 → cat2(5자) → cat1(3자) 순 폴백."""
    if not code:
        return None
    for key in (code, code[:5], code[:3]):
        if key in CAT_LABELS:
            return CAT_LABELS[key]
    return None


def _congestion_diff_pct(source_level: int, alt_level: int) -> int:
    """원본 대비 대안지의 혼잡도 감소율(%). level 은 1~4.
    ratio = level/4 기준. 대안이 더 혼잡하면 음수가 될 수 있어 0 하한."""
    s = max(1, source_level) / 4.0
    a = max(0, alt_level) / 4.0
    if s <= 0:
        return 0
    return max(0, round((s - a) / s * 100))


def _shared_category(source_cat: Optional[str], alt_cat: Optional[str]) -> Optional[str]:
    """cat 문자열('a>b>c')에서 공유하는 최하위 공통 카테고리."""
    if not source_cat or not alt_cat:
        return None
    s = [c.strip() for c in source_cat.split(">") if c.strip()]
    a = [c.strip() for c in alt_cat.split(">") if c.strip()]
    shared = None
    for x, y in zip(s, a):
        if x == y:
            shared = x
        else:
            break
    return shared


def compute_grounded_facts(source: dict, alt: dict,
                           source_level: int, alt_level: int) -> dict:
    """카드에 넣을 '검증된 사실'을 코드가 계산."""
    code = _shared_category(source.get("cat"), alt.get("cat"))
    return {
        "congestion_diff_pct": _congestion_diff_pct(source_level, alt_level),
        "shared_category": cat_label(code) or code,   # 라벨 우선(부록 B), 없으면 코드
        "source_level_label": LEVEL_LABELS.get(source_level, "보통"),
        "alt_level_label": LEVEL_LABELS.get(alt_level, "보통"),
    }


def _template_body(source: dict, alt: dict, facts: dict) -> str:
    diff = facts["congestion_diff_pct"]
    cat = facts["shared_category"]   # 이미 라벨화됨
    cat_clause = f" 같은 ‘{cat}’ 감성을 유지하면서" if cat else ""
    if diff > 0:
        return (
            f"{source['title']}은(는) 지금 도착 시점 예보가 '{facts['source_level_label']}'입니다. "
            f"대신 {alt['title']}은(는) '{facts['alt_level_label']}'로,{cat_clause} "
            f"예상 혼잡도가 약 {diff}% 낮습니다. 같은 감성, 다른 시간·장소로 여백을 즐겨보세요."
        )
    return (
        f"{alt['title']}은(는){cat_clause} {source['title']}과(와) 감성적으로 유사한 대안지입니다. "
        f"도착 시점 예보 기준 혼잡도는 비슷하니, 동선·취향에 맞춰 선택하세요."
    )


def _headline(source: dict, alt: dict) -> str:
    return f"{source['title']} 대신 {alt['title']}"


def build_card(source: dict, alt: dict,
               source_level: int, alt_level: int) -> dict:
    """설득 카드 생성. 부록 B 계약 형식."""
    facts = compute_grounded_facts(source, alt, source_level, alt_level)
    headline = _headline(source, alt)
    template_body = _template_body(source, alt, facts)

    generated_by = "template"
    body = template_body
    if settings.USE_LLM and settings.ANTHROPIC_API_KEY:
        llm_body = _llm_rewrite(source, alt, facts)
        if llm_body:
            body = llm_body
            generated_by = "llm"

    # 응답에 노출할 grounded_facts (내부 라벨 제외)
    public_facts = {
        "congestion_diff_pct": facts["congestion_diff_pct"],
        "shared_category": facts["shared_category"],
    }
    return {
        "headline": headline,
        "body": body,
        "grounded_facts": public_facts,
        "generated_by": generated_by,
    }


def _llm_rewrite(source: dict, alt: dict, facts: dict) -> Optional[str]:
    """LLM 재서술(옵션). context 밖 사실 추가 금지. 실패 시 None → 템플릿 폴백."""
    try:
        import anthropic
    except Exception:
        return None
    try:
        client = anthropic.Anthropic(api_key=settings.ANTHROPIC_API_KEY)
        system = (
            "너는 여행 코스 추천 카피라이터다. 아래 <context> 안의 사실만 사용해 "
            "2~3문장의 설득 문구를 한국어로 쓴다. context 에 없는 수치·사실·장소를 "
            "절대 추가하지 마라. 혼잡도 수치는 주어진 값을 그대로 쓴다. 과장 금지."
        )
        src_ov = (source.get("overview") or "")[:600]
        alt_ov = (alt.get("overview") or "")[:600]
        cat = facts["shared_category"] or "유사 감성"
        context = (
            f"<context>\n"
            f"원본 장소: {source['title']}\n소개: {src_ov}\n"
            f"대안 장소: {alt['title']}\n소개: {alt_ov}\n"
            f"계산된 혼잡도 비교: 대안지가 원본 대비 약 "
            f"{facts['congestion_diff_pct']}% 덜 혼잡(도착 시점 예보 기준). "
            f"원본 예보={facts['source_level_label']}, 대안 예보={facts['alt_level_label']}.\n"
            f"공유 카테고리: {cat}\n</context>"
        )
        # 결정 G: 짧고 제약된 재서술 작업 — 스트리밍 불필요, 낮은 max_tokens.
        resp = client.messages.create(
            model=settings.LLM_MODEL,
            max_tokens=512,
            system=system,
            messages=[{"role": "user", "content": context + "\n\n설득 문구:"}],
        )
        parts = [b.text for b in resp.content if getattr(b, "type", None) == "text"]
        text = "".join(parts).strip()
        return text or None
    except Exception:
        # 네트워크/인증/기타 어떤 실패든 템플릿으로 안전하게 폴백
        return None
