"""임베딩 유사도 서비스 — 감성 쌍둥이 매칭 (모듈1, 결정 E).

역할 분리:
  - 반경 후보 추림 = C++ SpatialIndex (지오)
  - 코사인 유사도 top-K = 여기 numpy (e5 벡터)

벡터는 오프라인(build_embeddings.py, e5-small)에서 L2 정규화되어 저장되므로
코사인 유사도 = 내적으로 단순화된다. 벡터가 없으면 유사도 계산은 비활성(빈 결과).
"""
from __future__ import annotations
from pathlib import Path
from typing import Iterable, Optional

import numpy as np

from ..config import settings


class EmbeddingStore:
    def __init__(self, vectors_path: str, ids_path: str):
        self.ok = False
        self.dim = 0
        self._vectors: Optional[np.ndarray] = None
        self._id_to_row: dict[int, int] = {}
        self._load(vectors_path, ids_path)

    def _load(self, vectors_path: str, ids_path: str) -> None:
        vp, ip = Path(vectors_path), Path(ids_path)
        if not vp.exists() or not ip.exists():
            return
        vecs = np.load(vp).astype(np.float32)
        ids = np.load(ip).astype(np.int64)
        if vecs.ndim != 2 or vecs.shape[0] != ids.shape[0]:
            return
        # 안전을 위해 재정규화(build 단계에서 이미 정규화됐다면 no-op에 가깝다)
        norms = np.linalg.norm(vecs, axis=1, keepdims=True)
        norms[norms == 0] = 1.0
        self._vectors = vecs / norms
        self._id_to_row = {int(cid): i for i, cid in enumerate(ids.tolist())}
        self.dim = int(vecs.shape[1])
        self.ok = True

    def has(self, content_id: int) -> bool:
        return content_id in self._id_to_row

    def similar(
        self,
        source_id: int,
        candidate_ids: Iterable[int],
        top_k: int,
    ) -> list[tuple[int, float]]:
        """source_id 와 후보들 간 코사인 유사도 상위 top_k → [(content_id, similarity)].

        벡터가 없거나 source 미보유 시 빈 리스트(호출측이 graceful 처리)."""
        if not self.ok or self._vectors is None or source_id not in self._id_to_row:
            return []
        cands = [c for c in candidate_ids if c != source_id and c in self._id_to_row]
        if not cands:
            return []
        src = self._vectors[self._id_to_row[source_id]]
        rows = np.fromiter((self._id_to_row[c] for c in cands), dtype=np.int64, count=len(cands))
        mat = self._vectors[rows]                 # (M, D)
        sims = mat @ src                          # 정규화됨 → 코사인 = 내적
        order = np.argsort(-sims)[:top_k]
        return [(int(cands[i]), float(sims[i])) for i in order]


_store: Optional[EmbeddingStore] = None


def get_store() -> EmbeddingStore:
    global _store
    if _store is None:
        _store = EmbeddingStore(settings.EMB_VECTORS, settings.EMB_IDS)
    return _store
