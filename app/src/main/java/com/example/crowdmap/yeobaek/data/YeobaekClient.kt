package com.example.crowdmap.yeobaek.data

import com.example.crowdmap.BuildConfig
import com.google.gson.Gson
import okhttp3.OkHttpClient
import retrofit2.Retrofit
import retrofit2.converter.gson.GsonConverterFactory
import java.util.concurrent.TimeUnit

// 여백 서버(FastAPI/uvicorn) Retrofit 싱글턴.
// 서버 IP 는 local.properties → BuildConfig.SERVER_IP (기존 앱과 공유), 포트는 uvicorn 기본 8000.
object YeobaekClient {

    const val PORT = 8000

    val gson: Gson = Gson()

    val api: YeobaekApi by lazy {
        val ok = OkHttpClient.Builder()
            .connectTimeout(10, TimeUnit.SECONDS)
            .readTimeout(30, TimeUnit.SECONDS)   // 스케줄러/예보는 다소 오래 걸릴 수 있음
            .build()

        Retrofit.Builder()
            .baseUrl("http://${BuildConfig.SERVER_IP}:$PORT/")
            .client(ok)
            .addConverterFactory(GsonConverterFactory.create(gson))
            .build()
            .create(YeobaekApi::class.java)
    }
}
