# 여백 서버 배포 (Fly.io)

이 디렉터리(`yeobaek/`)의 `Dockerfile`은 C++ 엔진(pybind11)까지 컨테이너 안에서 직접 빌드해
FastAPI 서버 하나로 묶는다. `data/yeobaek.db` + `embeddings.npy`도 이미지에 함께 들어가므로,
배포하면 바로 실제 데이터(7,361개 장소)로 동작한다.

이 저장소를 열람 중인 에이전트는 이 환경에 Docker/flyctl이 없어 로컬에서 직접 `docker build`/
`flyctl deploy`를 실행해 검증하지 못했다. Dockerfile은 `engine/CMakeLists.txt`·
`server/requirements.txt` 실제 내용을 그대로 반영해 작성했지만, **최초 배포 시 빌드 로그를
꼭 확인할 것.**

## 0) 준비물

- Fly.io 계정 (무료 가입, 카드 등록 필요할 수 있음 — 무료 티어 한도 내에서는 과금 안 됨)
- `flyctl` CLI 설치: https://fly.io/docs/hands-on/install-flyctl/
  (Windows PowerShell: `pwsh -c "iwr https://fly.io/install.ps1 -useb | iex"`)

## 1) 로그인 (대화형 — 직접 실행)

```
flyctl auth login
```

## 2) 최초 배포

```
cd yeobaek
flyctl launch --no-deploy   # fly.toml 이 이미 있으므로 기존 설정 사용할지 물으면 Yes
flyctl deploy
```

`yeobaek-api`라는 앱 이름이 이미 다른 사람이 쓰고 있으면 `flyctl launch`가 실패한다 —
그러면 `fly.toml`의 `app = "yeobaek-api"`를 다른 이름(예: `yeobaek-api-honey0127`)으로
바꾸고 다시 실행. 배포된 주소는 `https://<app 이름>.fly.dev`.

## 3) API 키 주입 (이미지에는 안 들어있음 — 반드시 여기서)

```
flyctl secrets set SEOUL_API_KEY=... TOURAPI_KEY=... TATS_API_KEY=...
```

키를 안 넣어도 서버는 뜨지만(중립값 폴백), 실제 혼잡 예보를 보여주려면 최소
`SEOUL_API_KEY`는 필요하다.

## 4) 배포 확인

```
curl https://<app 이름>.fly.dev/health
```

`{"status":"ok","engine_available":true,"places_loaded":7361,...}` 가 나오면 성공.

## 5) 앱의 PROD_BASE_URL 갱신

프로젝트 루트 `local.properties`(git 추적 안 됨)에서:

```properties
PROD_BASE_URL=https://<app 이름>.fly.dev/
```

이 값이 릴리스 빌드의 `BuildConfig.BASE_URL`로 들어간다(`app/build.gradle.kts` release
buildType 참고). 값을 바꾼 뒤에는 `./gradlew bundleRelease`로 다시 빌드해야 반영된다.

## 무료 티어가 자동으로 꺼지는 것에 대해

`fly.toml`의 `min_machines_running = 0` 설정 때문에 트래픽이 없으면 머신이 완전히
꺼진다(과금 절약). 심사위원이 접속하는 순간 자동으로 다시 켜지지만 **첫 요청은 몇 초
지연**될 수 있다. 시연 직전에는 `curl .../health`로 한 번 깨워두는 걸 권장.
