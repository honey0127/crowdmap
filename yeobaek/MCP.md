# 서울 실시간 도시데이터 MCP — 여백에서의 위치

## MCP가 하는 일 (한 줄)

**AI(Claude·ChatGPT 같은 LLM 클라이언트)가 외부 데이터를 직접 조회할 수 있게 해주는 표준 규격**이다.
사람이 API 문서를 읽고 코드를 짜는 대신, AI가 "지금 서울에서 제일 붐비는 곳 알려줘"라고
물으면 알아서 해당 API를 호출하고 결과를 해석한다.

```
[사람] "지금 서울에서 제일 붐비는 곳?"
   ↓
[AI 클라이언트]  ← MCP 규격 →  [MCP 서버]  ← REST →  [서울시 실시간 도시데이터 API]
   ↓
[사람] "강남역이 '붐빔', 홍대입구역이 '붐빔' 입니다"
```

제공 도구(문서 기준): `list_pois`, `get_population`, `get_population_prediction`,
`get_consumption`, `get_weather`, `get_air_quality`, 버스·지하철 승하차, 주차장 잔여,
따릉이 잔여, `list_events`(문화행사) 등.

## ⚠️ 여백 앱/서버에는 MCP를 넣지 않는다 — 이유

MCP는 **AI 클라이언트용 프로토콜**이다. 안드로이드 앱이나 FastAPI 서버가 쓰는
라이브러리가 아니다. 그리고 여백은 **이미 같은 데이터를 직접 호출**하고 있다:

| | 여백 런타임(현재) | MCP 경유(가정) |
| --- | --- | --- |
| 경로 | 여백 서버 → 서울 API | 여백 서버 → MCP 서버 → 서울 API |
| 구현 | `SeoulCityDataForecastClient`(C++) / `py_forecast.py` | 미들웨어 1단 추가 |
| 지연 | 1홉 | 2홉 |
| 장애 지점 | 서울 API | 서울 API + MCP 서버 |

즉 런타임에 MCP를 끼우면 **느려지고 장애 지점만 늘어난다.** 그래서 여백 서비스 코드는
지금 구조(직접 호출)를 유지한다.

## 그럼 어디에 쓰는가 — 개발·기획 도구로

`.mcp.json`(레포 루트)에 등록해 두면, **개발 중 대화로** 서울 데이터를 즉시 확인할 수 있다:

- "지금 북촌 인구 혼잡도와 예측 보여줘" → 우리 서버 응답이 맞는지 **교차 검증**
- "서울숲 근처 문화행사 목록" → 코스 추천에 붙일 만한 소재 탐색
- "강남역 따릉이·주차장 잔여" → 다음 기능 아이디어 검토

이건 코드 품질·데이터 검증에 쓰는 것이지, 제출물의 실행 경로가 아니다.

## 연결 방법

### 방법 1 — PlayMCP 커넥터 (가장 간단, 계정 단위)

1. https://mcp.whitellm.ai 접속 → 카카오 로그인 → **[키 발급하기]**
2. https://playmcp.kakao.com 접속 → **"서울 실시간 도시데이터 MCP"** 검색
3. **[+도구함에 추가]** → 키 입력
4. Claude **설정 > 커넥터 > 커넥터 둘러보기 > PlayMCP > [연결]** → 카카오 OAuth 동의

> Claude Team/Enterprise는 Owner가 **[팀에 추가]** 후 각자 [연결].
> ChatGPT는 설정 > 앱 > 개발자 모드 > [앱 만들기]에 도구함 URL 등록(Authentication=OAuth).

### 방법 2 — 이 레포의 `.mcp.json` (프로젝트 단위)

키를 발급받은 뒤 환경변수만 설정하면 된다(키는 파일에 쓰지 않는다):

```bash
export SEOUL_MCP_URL="https://<발급페이지에 안내된 주소>/mcp"
export SEOUL_MCP_KEY="<발급받은 키>"
```

Claude Code를 재시작하고 `/mcp`로 연결 상태를 확인한다.
HTTP 연결이 안 되는 구형 클라이언트라면 문서의 `mcp-remote` 브리지 방식을 쓴다:

```bash
npx -y mcp-remote "$SEOUL_MCP_URL" --header "Authorization: Bearer $SEOUL_MCP_KEY"
```

## 공모전 관점 — 주의

- **심사 요건은 한국관광공사 OpenAPI 활용**이다. 서울 실시간 도시데이터(서울시)와
  이 MCP는 **보조 소스**이지 필수 요건을 대체하지 않는다.
- MCP를 "썼다"고 쓰려면, **실행 경로에 실제로 들어가야** 한다. 개발 중 조회용으로만
  썼다면 기능설명서에 활용 API로 적지 말 것(확인 과정에서 문제가 된다).
- 발전성(20점) 소재로 쓰고 싶다면 → 아래 "확장 아이디어" 참고.

---

# 여백 MCP 서버 (구현 완료)

방향을 뒤집어, **여백 자체가 AI 에이전트의 도구**가 된다.
Claude/ChatGPT 에서 이렇게 물으면 여백 엔진이 답한다:

> "경복궁 지금 붐비는데 비슷한 분위기의 한적한 곳 알려줘"
> "내일 오전 10시에 경복궁·국립중앙박물관·명동 도는 코스 짜줘"

→ 여백은 앱일 뿐 아니라 **다른 AI 서비스가 호출할 수 있는 관광 혼잡 회피 엔진**이 된다.

## 제공 도구

| 도구 | 하는 일 | 입력 |
| --- | --- | --- |
| `search_places` | 여백 보유 관광지 이름 검색(다른 도구에 넘길 이름 확인) | `query` |
| `find_quiet_alternative` | 붐비는 곳 → 비슷한 분위기의 한적한 대안 | `place_name`, `top_k?`, `radius_km?`, `arrival_time?` |
| `get_offpeak_hours` | 그 장소가 덜 붐비는 시간대(예측 인구 근거 포함) | `place_name` |
| `plan_quiet_course` | 장소 목록 → 혼잡 회피 하루 코스(순서 재배치·대체) | `place_names[]`, `start_time?`, … |
| `get_congestion` | 현재 혼잡도(1 여유 ~ 4 붐빔) | `place_name` |

AI 는 `content_id` 를 모르므로 **모든 도구가 장소 이름을 받고** 내부에서 검색해 푼다.

## 엔드포인트

앱 API 와 **같은 서버**에 붙어 있다(별도 배포 불필요).

```
POST https://<서버주소>/mcp     # JSON-RPC 2.0 (MCP Streamable HTTP)
GET  https://<서버주소>/mcp     # 도구 목록 등 안내(브라우저 확인용)
```

## AI 클라이언트에 연결하기

Claude Desktop / Claude Code 의 커넥터 설정에 HTTP MCP 서버로 등록한다:

```json
{
  "mcpServers": {
    "yeobaek": { "type": "http", "url": "https://<서버주소>/mcp" }
  }
}
```

동작 확인(서버만 떠 있으면 됨):

```bash
curl -s -X POST http://localhost:8000/mcp \
  -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":1,"method":"tools/list"}' | head -c 400
```

## 구현 노트

- `server/mcp_server.py` — 외부 SDK 없이 JSON-RPC(`initialize`/`tools/list`/`tools/call`)만
  직접 처리한다. 배포 이미지에 의존성을 늘리지 않기 위해서다.
- 도구 구현은 **기존 FastAPI 라우트 핸들러를 그대로 호출**한다(전부 동기 함수).
  로직을 복제하지 않으므로 앱과 MCP 의 답이 어긋날 수 없다.
- 인증은 없다 — 공개 조회 API 만 노출하며 쓰기 동작(제보 등)은 도구로 열지 않았다.

## 심사에서의 의미 (발전성 20점)

- "앱 하나"가 아니라 **재사용 가능한 엔진**임을 실물로 보여준다.
- 심사위원이 Claude 에 연결해 직접 질문하는 시연이 가능하다.
- 관광 데이터 → 혼잡 회피 판단을 **다른 AI 서비스가 가져다 쓰는** 확장 경로가 열린다.
