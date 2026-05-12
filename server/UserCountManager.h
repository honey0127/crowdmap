#ifndef USERCOUNTMANAGER_H
#define USERCOUNTMANAGER_H

#include <unordered_map>
#include <mutex>

class UserCountManager {
private:
    // zoneID -> 사용자 수
    std::unordered_map<int, int> zoneCounts;
    // userID -> 현재 zoneID (사용자 이동 시 이전 zone -1, 새 zone +1 처리용)
    std::unordered_map<int, int> userZones;
    std::mutex mtx;

public:
    // 사용자 위치 업데이트
    void updateUserLocation(int userId, int newZoneId);
    
    // 특정 zone의 사용자 수 조회
    int getZoneCount(int zoneId);

    // 모든 zone의 사용자 수 조회
    std::unordered_map<int, int> getAllZoneCounts();

    // 사용자 완전 제거 (DeadSessionSweeper 호출용)
    void removeUser(int userId);
};

#endif // USERCOUNTMANAGER_H