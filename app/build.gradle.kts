    import java.util.Properties

plugins {
    id("com.android.application") // 버전 정보를 빼고 ID만 적습니다.
    id("org.jetbrains.kotlin.android")
}

val localProperties = Properties().apply {
    val f = rootProject.file("local.properties")
    if (f.exists()) load(f.inputStream())
}

android {
    namespace = "com.example.crowdmap"
    compileSdk = 34

    defaultConfig {
        applicationId = "com.example.crowdmap"
        minSdk = 26
        targetSdk = 34
        versionCode = 1
        versionName = "1.0"

        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"
        // 지도 키: local.properties 의 MAPS_API_KEY 우선, 없으면 기존 기본 키로 폴백(회귀 방지).
        manifestPlaceholders["MAPS_API_KEY"] =
            localProperties.getProperty("MAPS_API_KEY", "AIzaSyCzG9WISxXy7e8EZ-YCf5XtVbpSfX4V9SA")
        buildConfigField("String", "SERVER_IP",
            "\"${localProperties.getProperty("SERVER_IP", "127.0.0.1")}\"")
    }

    buildFeatures {
        buildConfig = true
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro"
            )
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
    // 위치 서비스 (Fused Location Provider)
    implementation("com.google.android.gms:play-services-location:21.0.1")
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
}