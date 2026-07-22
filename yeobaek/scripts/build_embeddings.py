"""1-3. e5-small 임베딩 생성 → embeddings.npy(+ ids) (결정 E).

intfloat/multilingual-e5-small 규칙:
  - 문서(passage) 는 "passage: {overview}" 프리픽스
  - 쿼리는 "query: {text}" 프리픽스 (런타임 매칭은 문서-문서 코사인이므로 여기선 passage)
L2 정규화하여 코사인=내적으로 단순화(embedding.py 가 그대로 사용).

사용:
    pip install -r scripts/requirements.txt
    cd yeobaek && python scripts/build_embeddings.py
"""
from __future__ import annotations
import sqlite3
import sys
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

from server.config import settings   # noqa: E402

MODEL_NAME = "intfloat/multilingual-e5-small"
BATCH = 64


def main() -> int:
    try:
        from sentence_transformers import SentenceTransformer
    except Exception:
        print("ERROR: sentence-transformers 가 필요합니다. "
              "pip install -r scripts/requirements.txt", file=sys.stderr)
        return 2

    con = sqlite3.connect(settings.DB_PATH)
    con.row_factory = sqlite3.Row
    rows = con.execute(
        "SELECT content_id, overview FROM places "
        "WHERE overview IS NOT NULL AND length(trim(overview))>0 ORDER BY content_id"
    ).fetchall()
    if not rows:
        print("임베딩할 overview 가 없습니다. collect_tourapi.py 를 먼저 실행하세요.", file=sys.stderr)
        return 1

    ids = np.array([r["content_id"] for r in rows], dtype=np.int64)
    texts = [f"passage: {r['overview']}" for r in rows]

    print(f"모델 로드: {MODEL_NAME} (최초 실행 시 다운로드)")
    model = SentenceTransformer(MODEL_NAME)
    vecs = model.encode(
        texts, batch_size=BATCH, convert_to_numpy=True,
        normalize_embeddings=True,   # L2 정규화 → 코사인=내적
        show_progress_bar=True,
    ).astype(np.float32)

    Path(settings.EMB_VECTORS).parent.mkdir(parents=True, exist_ok=True)
    np.save(settings.EMB_VECTORS, vecs)
    np.save(settings.EMB_IDS, ids)

    # 선택: DB BLOB 에도 저장(부록 A embeddings 테이블)
    dim = int(vecs.shape[1])
    con.executemany(
        "INSERT INTO embeddings(content_id,dim,vector) VALUES (?,?,?)"
        " ON CONFLICT(content_id) DO UPDATE SET dim=excluded.dim, vector=excluded.vector",
        [(int(cid), dim, vecs[i].tobytes()) for i, cid in enumerate(ids.tolist())],
    )
    con.commit()
    con.close()

    print(f"완료: {vecs.shape[0]}개 벡터(dim={dim}) → {settings.EMB_VECTORS} / {settings.EMB_IDS}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
