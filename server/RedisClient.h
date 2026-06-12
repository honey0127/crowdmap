#pragma once
#include <hiredis/hiredis.h>
#include <chrono>
#include <cstdio>
#include <cinttypes>
#include <mutex>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

/**
 * Redis 연결 래퍼.
 * 저장 키 형식: cm:e:{spatialKey}:{userId}  →  unix 타임스탬프(초)
 * TTL: 300초 (GridZone 슬라이딩 윈도우와 동일)
 */
class RedisClient {
public:
    explicit RedisClient(const std::string& host = "127.0.0.1", int port = 6379) {
        ctx_ = redisConnect(host.c_str(), port);
        if (!ctx_ || ctx_->err) {
            std::string err = ctx_ ? ctx_->errstr : "null context";
            if (ctx_) redisFree(ctx_);
            ctx_ = nullptr;
            throw std::runtime_error("Redis 연결 실패: " + err);
        }
    }

    ~RedisClient() {
        if (ctx_) redisFree(ctx_);
    }

    RedisClient(const RedisClient&) = delete;
    RedisClient& operator=(const RedisClient&) = delete;

    // recordLocation 호출 시마다 Redis에 이벤트 저장 (TTL 300초)
    void saveEvent(int64_t spatialKey, int32_t userId, long long unixTs) {
        std::lock_guard<std::mutex> lock(mutex_);
        // [FIX] int64_t 포맷 PRId64 사용 (플랫폼 독립적)
        auto* r = static_cast<redisReply*>(
            redisCommand(ctx_, "SET cm:e:%" PRId64 ":%d %lld EX 300",
                         spatialKey, userId, unixTs));
        if (r) freeReplyObject(r);
    }

    // 서버 시작 시 Redis에 남은 모든 이벤트 읽기
    // 반환: (spatialKey, userId, unixTs) 튜플 목록
    std::vector<std::tuple<int64_t, int32_t, long long>> loadAllEvents() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::tuple<int64_t, int32_t, long long>> result;

        long long cursor = 0;
        do {
            auto* reply = static_cast<redisReply*>(
                redisCommand(ctx_, "SCAN %lld MATCH cm:e:*:* COUNT 200", cursor));
            if (!reply || reply->type != REDIS_REPLY_ARRAY) break;

            cursor = std::stoll(reply->element[0]->str);
            auto* keys = reply->element[1];

            for (size_t i = 0; i < keys->elements; ++i) {
                const std::string key = keys->element[i]->str;

                int64_t sk = 0;
                int32_t uid = 0;
                // [FIX] int64_t 파싱 포맷 SCNd64 사용
                if (std::sscanf(key.c_str(),
                                "cm:e:%" SCNd64 ":%d", &sk, &uid) != 2) continue;

                auto* val = static_cast<redisReply*>(
                    redisCommand(ctx_, "GET %s", key.c_str()));
                if (val && val->type == REDIS_REPLY_STRING) {
                    result.emplace_back(sk, uid, std::stoll(val->str));
                }
                if (val) freeReplyObject(val);
            }
            freeReplyObject(reply);
        } while (cursor != 0);

        return result;
    }

    // Redis가 살아있는지 확인
    bool ping() {
        std::lock_guard<std::mutex> lock(mutex_);
        auto* r = static_cast<redisReply*>(redisCommand(ctx_, "PING"));
        bool ok = r && r->type == REDIS_REPLY_STATUS &&
                  std::string(r->str) == "PONG";
        if (r) freeReplyObject(r);
        return ok;
    }

private:
    redisContext* ctx_ = nullptr;
    std::mutex mutex_;
};