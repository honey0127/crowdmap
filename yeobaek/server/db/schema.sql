-- 여백(Yeobaek) SQLite 스키마 (절차서 부록 A)
-- 적용:  sqlite3 data/yeobaek.db < server/db/schema.sql

CREATE TABLE IF NOT EXISTS places (
  content_id    INTEGER PRIMARY KEY,
  title         TEXT NOT NULL,
  addr          TEXT,
  mapx          REAL,   -- 경도(lng)
  mapy          REAL,   -- 위도(lat)
  area_code     INTEGER,
  sigungu_code  INTEGER,
  cat           TEXT,   -- "cat1>cat2>cat3" 또는 대표 카테고리명
  overview      TEXT,
  updated_at    TEXT
);

-- 장소 → 가장 가까운 서울 예보지점 (결정 C, 사전계산)
CREATE TABLE IF NOT EXISTS place_area_map (
  content_id       INTEGER PRIMARY KEY REFERENCES places(content_id),
  seoul_area_name  TEXT NOT NULL,
  dist_km          REAL
);

-- 임베딩 영속화(선택) — 기본은 numpy 사이드카(embeddings.npy)를 사용한다.
CREATE TABLE IF NOT EXISTS embeddings (
  content_id  INTEGER PRIMARY KEY REFERENCES places(content_id),
  dim         INTEGER,
  vector      BLOB
);

-- 예보 영속화(선택) — 런타임 캐시는 C++ ForecastProvider 가 담당.
CREATE TABLE IF NOT EXISTS forecast_cache (
  seoul_area_name TEXT,
  fcst_time       TEXT,
  congest_lvl     INTEGER,
  ppltn_min       INTEGER,
  ppltn_max       INTEGER,
  fetched_at      TEXT,
  PRIMARY KEY (seoul_area_name, fcst_time)
);

CREATE INDEX IF NOT EXISTS idx_places_area ON places(area_code);

-- 여행자 실시간 제보(크라우드소싱, 모듈5) — "지금 붐벼요/한적해요/팁"
CREATE TABLE IF NOT EXISTS user_reports (
  id          INTEGER PRIMARY KEY AUTOINCREMENT,
  content_id  INTEGER,           -- 연관 명소(선택)
  lat         REAL NOT NULL,
  lng         REAL NOT NULL,
  kind        TEXT NOT NULL,     -- 'busy' | 'quiet' | 'tip'
  text        TEXT,
  created_at  TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_reports_created ON user_reports(created_at);
