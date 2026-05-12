#include "UserCountManager.h"

void UserCountManager::updateUserLocation(int userId, int newZoneId) {
    std::unique_lock<std::mutex> lock(mtx);
    
    // 사용자가 이전에 다른 zone에 있었다면 -1 처리
    if (userZones.find(userId) != userZones.end()) {
        int oldZoneId = userZones[userId];
        if (zoneCounts.find(oldZoneId) != zoneCounts.end()) {
            zoneCounts[oldZoneId]--;
            if (zoneCounts[oldZoneId] < 0) zoneCounts[oldZoneId] = 0;
        }
    }
    
    // 새로운 zone에 사용자 추가
    zoneCounts[newZoneId]++;
    userZones[userId] = newZoneId;
}

int UserCountManager::getZoneCount(int zoneId) {
    std::unique_lock<std::mutex> lock(mtx);
    if (zoneCounts.find(zoneId) != zoneCounts.end()) {
        return zoneCounts[zoneId];
    }
    return 0;
}

std::unordered_map<int, int> UserCountManager::getAllZoneCounts() {
    std::unique_lock<std::mutex> lock(mtx);
    return zoneCounts;
}

void UserCountManager::removeUser(int userId) {
    std::unique_lock<std::mutex> lock(mtx);
    if (userZones.count(userId)) {
        int oldZone = userZones[userId];
        if (zoneCounts.count(oldZone) && zoneCounts[oldZone] > 0)
            zoneCounts[oldZone]--;
        userZones.erase(userId);
    }
}