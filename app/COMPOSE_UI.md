# 여백 Compose UI 레이어

새 디자인 브리프("Deep Slate Teal, 여백의 미학")를 Jetpack Compose 로 구현한 레이어.
**기존 View/XML 화면(YeobaekHomeActivity 등)은 그대로 두고 공존**한다.

## 화면 (5)

| 화면 | 파일 | 바인딩 엔드포인트 |
|---|---|---|
| ① 메인 대시보드 (여백의 발견) | `compose/DashboardScreen.kt` | `/match` (twins·similarity·forecast_level·dist_km) |
| ② 스케줄 타임라인 (여백 스케줄러) | `compose/ScheduleScreen.kt` | `/schedule` (ordered·saved_congestion_pct·substituted_from) |
| ③ 혼잡 히트맵 (지도) | `compose/MapHeatmapScreen.kt` | `/places/heatmap` (level·quiet_score) |
| ④ 장소 상세 (오프피크·분산) | `compose/PlaceDetailScreen.kt` | `/places/offpeak`, `/places/disperse` |
| ⑤ 온보딩 | `compose/OnboardingScreen.kt` | — (클라이언트) |

- 진입점: `compose/YeobaekComposeActivity.kt` (하단탭 NavHost). 데모용 별도 런처 **"여백 ✦"** 로 노출.
- 모든 화면은 **상태를 인자로 받는 순수 Composable** — `SampleData.kt`(오프라인 시연) 또는
  ViewModel 이 `YeobaekClient.api.*()` 로 채운 실데이터, 둘 다 동일하게 렌더.

## 디자인 시스템

`compose/theme/` — Color / Type / Shape / Theme.
- **브랜드**: Deep Slate Teal `#17595E`. Material3 `ColorScheme` + 확장 토큰 `YeobaekTheme.colors`.
- **혼잡 밀도**(accent 와 분리된 시맨틱): 세이지/앰버/뮤트 크림슨, `forecast_level 1..4` 에 매핑.
  `YeobaekTheme.colors.density(level)` → (fg, bg, ink). 미매핑/범위밖은 중립 Lv.2 폴백.
- **모양**: card 18dp · hero/alt 24dp · pill 999. 그림자 대신 1dp 틸-바이어스 보더(`Modifier.hairline`).
- **다크 테마** 완비(`isSystemInDarkTheme`).

> ✅ **팔레트 통일(슬레이트 틸)**: 팀 결정에 따라 View/XML 화면도 틸로 통일했다.
> `res/values/yeobaek.xml`(브랜드·밀도 토큰)과 `Congestion.color()`(View 배지)를 Compose 밀도
> 토큰과 동일한 값으로 맞췄다 — 그린(#0FB86B) → 슬레이트 틸(#17595E), 밝은 혼잡색 → 채도 낮춘
> 세이지/앰버/뮤트 크림슨. 이제 View·Compose 두 레이어가 같은 브랜드로 렌더된다.

## 빌드 설정 (추가된 것)

- `gradle/libs.versions.toml`: Compose BOM `2024.09.00`, activity/navigation/lifecycle-compose, 그리고
  Kotlin 2.0 용 **Compose Compiler 플러그인**(`org.jetbrains.kotlin.plugin.compose`).
- `build.gradle.kts`(root): `compose.compiler` 플러그인 `apply false`.
- `app/build.gradle.kts`: 플러그인 적용 + `buildFeatures.compose = true` + Compose 의존성.

## ⚠️ 빌드 검증 상태

이 코드는 **Android SDK 가 없는 원격 환경에서 작성**되어 **Gradle 빌드로 컴파일 검증되지 않았다**.
Android Studio 에서 최초 sync·빌드가 필요하다. (기존 앱 자체도 아직 최초 빌드 전 — ROADMAP 참고.)
빌드 시 확인 순서:

1. Gradle sync — Compose 플러그인/BOM 해석 확인.
2. `./gradlew :app:compileDebugKotlin` — Compose 레이어 컴파일.
3. `@Preview` (각 `*Screen.kt` 하단) 로 화면별 렌더 확인 — 서버 없이 `SampleData` 로 미리보기.
4. 런처 "여백 ✦" 실행 → 하단탭 발견/스케줄/지도.

## 마이크로 인터랙션

- **혼잡→한적 스왑**(대시보드): 스왑 버튼 탭 시 히어로 혼잡%가 `animateIntAsState(tween 460)` 로 카운트다운.
- **밀도 미터**: `animateFloatAsState` 로 세그먼트 채움('꺼진 칸' = 여백).
- **온보딩 인디케이터**: 현재 페이지 도트가 `animateDpAsState` 로 늘어남.
- 실기기에선 스왑 성공에 햅틱 1회(`HapticFeedbackType.LongPress`) 추가 권장.

## 실서버 연결 (구현됨) — `compose/ui/`

각 화면은 얇은 ViewModel + Route(stateful) 로 실서버(YeobaekApi)에 연결된다.

| 파일 | 내용 |
|---|---|
| `ui/UiState.kt` | `UiState<T>`(Loading/Success/Error) + `StateHost`(공통 로딩·에러·재시도 UI) |
| `ui/ViewModels.kt` | Dashboard/Schedule/Map/PlaceDetail ViewModel — `viewModelScope` 로 API 호출 |
| `ui/StatefulScreens.kt` | `DashboardRoute`/`ScheduleRoute`/`MapRoute`/`PlaceDetailRoute` — VM 구독 → 순수 화면 |

- 흐름: `Route` 가 `LaunchedEffect(params)` 로 `vm.load(...)` → `StateFlow<UiState>` 구독 →
  Success 면 순수 화면 렌더, Error 면 재시도/**샘플로 보기**(오프라인 시연 안전) 노출.
- `YeobaekComposeActivity` 하단탭이 이제 Route(실데이터)를 사용. 데모 진입 기본값은 파일 상단 상수
  (`DEMO_CONTENT_ID` 등) — 실제 앱은 검색/지도 선택값을 네비 인자로 전달.
- ViewModel 은 no-arg 생성자(모든 파라미터 기본값) → `viewModel()` 기본 팩토리로 생성(DI 불필요).
- `SERVER_IP` 는 `local.properties` → `BuildConfig.SERVER_IP`(기존 앱과 공유), 포트 8000.

### 서버 미가동 시 시연
각 화면 에러 상태의 **"샘플로 보기"** → `SampleData` 로 완성 화면을 그린다(실측 서울 명소).
심사장에서 서버가 안 떠도 UI 전체를 시연할 수 있다.
