# 여백(Yeobaek) API 서버 — FastAPI + C++ 엔진(pybind11) 컨테이너.
# 빌드 컨텍스트는 이 파일이 있는 yeobaek/ 디렉터리다:
#   cd yeobaek && docker build -t yeobaek-api .
#   docker run --rm -p 8080:8080 yeobaek-api
#
# 실서비스 키(SEOUL_API_KEY/TOURAPI_KEY/TATS_API_KEY)는 이미지에 넣지 않는다.
# 배포 플랫폼의 환경변수/시크릿으로만 주입할 것(-e 또는 fly secrets set 등).

# ── 1단계: C++ 엔진(yeobaek_engine.so) 빌드 ──
FROM python:3.12-slim AS engine-build

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake git libcurl4-openssl-dev \
    && rm -rf /var/lib/apt/lists/*

RUN pip install --no-cache-dir pybind11

WORKDIR /src
COPY engine/ engine/
COPY server/ server/

RUN cmake -S engine -B engine/build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build engine/build --target yeobaek_engine -j "$(nproc)"

# ── 2단계: 런타임 ──
FROM python:3.12-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
        libcurl4 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY server/requirements.txt server/requirements.txt
RUN pip install --no-cache-dir -r server/requirements.txt

COPY server/ server/
COPY data/ data/
# CMakeLists.txt 의 POST_BUILD 단계가 이미 engine-build 의 server/ 옆에 .so 를 복사해둔다.
COPY --from=engine-build /src/server/yeobaek_engine*.so server/

ENV YEOBAEK_DB=/app/data/yeobaek.db \
    YEOBAEK_EMB=/app/data/embeddings.npy \
    YEOBAEK_EMB_IDS=/app/data/embeddings_ids.npy \
    PORT=8080

EXPOSE 8080
# python -m uvicorn(바로 uvicorn 아님) — 그래야 /app(cwd)이 sys.path 에 들어가 `server` 패키지를 찾는다.
CMD ["sh", "-c", "python -m uvicorn server.main:app --host 0.0.0.0 --port ${PORT}"]
