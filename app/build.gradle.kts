import java.util.Properties

plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
    alias(libs.plugins.compose.compiler)
}

val localProperties = Properties().apply {
    val f = rootProject.file("local.properties")
    if (f.exists()) load(f.inputStream())
}

// 릴리스 서명 정보. keystore.properties 는 반드시 .gitignore 에 넣을 것.
val keystoreProperties = Properties().apply {
    val f = rootProject.file("keystore.properties")
    if (f.exists()) load(f.inputStream())
}
val hasReleaseKeystore = keystoreProperties.getProperty("storeFile") != null

android {
    // namespace = R 클래스/소스 package 선언 기준. 지금 바꾸면 전 소스의 package 선언을
    // 다 고쳐야 하므로 그대로 둔다. 스토어에 노출되는 건 namespace 가 아니라 applicationId.
    namespace = "com.example.crowdmap"
    compileSdk = 36

    defaultConfig {
        // 한 번 업로드하면 영원히 못 바꾼다. com.example.* 은 Google Play 가 업로드 자체를 거부한다.
        applicationId = "io.github.honey0127.yeobaek"
        minSdk = 26
        targetSdk = 36
        versionCode = 1
        versionName = "0.9.0"

        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"

        // 지도 키: local.properties 의 MAPS_API_KEY 사용.
        // 기존 하드코딩 폴백 키는 공개 저장소에 노출되었으므로 제거했다. (재발급 필요)
        manifestPlaceholders["MAPS_API_KEY"] =
            localProperties.getProperty("MAPS_API_KEY", "")
    }

    signingConfigs {
        if (hasReleaseKeystore) {
            create("release") {
                storeFile = rootProject.file(keystoreProperties.getProperty("storeFile"))
                storePassword = keystoreProperties.getProperty("storePassword")
                keyAlias = keystoreProperties.getProperty("keyAlias")
                keyPassword = keystoreProperties.getProperty("keyPassword")
            }
        }
    }

    buildFeatures {
        buildConfig = true
        compose = true
    }

    buildTypes {
        debug {
            // 에뮬레이터에서 호스트 PC 의 FastAPI 는 10.0.2.2 로 잡힌다.
            // 실기기 테스트는 local.properties 에 DEV_BASE_URL=http://192.168.x.x:8000/ 지정.
            buildConfigField(
                "String", "BASE_URL",
                "\"${localProperties.getProperty("DEV_BASE_URL", "http://10.0.2.2:8000/")}\""
            )
        }
        release {
            // 릴리스는 반드시 https. network_security_config 가 http 를 차단한다.
            // 도메인 미보유 → Fly.io 기본 서브도메인 사용(yeobaek/DEPLOY.md 참고).
            // 배포 후 local.properties 의 PROD_BASE_URL 을 실제 https://<앱이름>.fly.dev/ 로 갱신할 것.
            buildConfigField(
                "String", "BASE_URL",
                "\"${localProperties.getProperty("PROD_BASE_URL", "https://yeobaek-api.fly.dev/")}\""
            )

            // R8 는 Retrofit/Gson 리플렉션과 충돌 여지가 있어 첫 출시에서는 끈다.
            // 출시 후 여유가 생기면 켜고 proguard-rules.pro 를 채울 것.
            isMinifyEnabled = false
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro"
            )
            if (hasReleaseKeystore) {
                signingConfig = signingConfigs.getByName("release")
            }
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_11
        targetCompatibility = JavaVersion.VERSION_11
    }

    kotlinOptions {
        jvmTarget = "11"
    }
}

dependencies {
    implementation(libs.androidx.core.ktx)
    implementation(libs.androidx.appcompat)
    implementation(libs.material)
    implementation(libs.androidx.activity)
    implementation(libs.androidx.constraintlayout)
    implementation(libs.play.services.maps)
    testImplementation(libs.junit)
    androidTestImplementation(libs.androidx.junit)
    androidTestImplementation(libs.androidx.espresso.core)
// 코루틴 (비동기 처리)
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.7.3")
// ViewModel
    implementation("androidx.lifecycle:lifecycle-viewmodel-ktx:2.6.2")
// 여백: lifecycleScope (코루틴 UI 연동)
    implementation("androidx.lifecycle:lifecycle-runtime-ktx:2.6.2")
// 여백: RecyclerView (플래너 타임라인 / 대안 목록)
    implementation("androidx.recyclerview:recyclerview:1.3.2")
// 여백: Retrofit2 + Gson (FastAPI /schedule·/match·/card)
    implementation("com.squareup.retrofit2:retrofit:2.11.0")
    implementation("com.squareup.retrofit2:converter-gson:2.11.0")

    // 여백 Compose 레이어 (기존 View/XML 화면과 공존, 슬레이트 틸 디자인 시스템)
    val composeBom = platform(libs.androidx.compose.bom)
    implementation(composeBom)
    androidTestImplementation(composeBom)
    implementation(libs.androidx.ui)
    implementation(libs.androidx.ui.graphics)
    implementation(libs.androidx.ui.tooling.preview)
    implementation(libs.androidx.material3)
    implementation(libs.androidx.activity.compose)
    implementation(libs.androidx.navigation.compose)
    implementation(libs.androidx.lifecycle.viewmodel.compose)
    debugImplementation(libs.androidx.ui.tooling)
}
