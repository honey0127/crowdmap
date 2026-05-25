#ifndef EPOLL_SERVER_H
#define EPOLL_SERVER_H

#include <sys/epoll.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>
#include <unordered_map>
#include <string_view>
#include <vector>
#include <string>
#include "MemoryPool.h"
#include "SpatialDensityEngine.h"
#include "ZoneMapper.h"
#include "CongestionRouter.h"
#include "CacheManager.h"

// ... (ClientContext unchanged) ...

class EpollServer {
public:
    EpollServer(uint16_t port, 
                SpatialDensityEngine& engine,
                ZoneMapper& zm,
                CongestionRouter& cr,
                CacheManager& cm);
// ...
private:
// ...
    // 비즈니스 로직 컴포넌트 참조
    SpatialDensityEngine& densityEngine_;
    ZoneMapper& zoneMapper_;
    CongestionRouter& congestionRouter_;
    CacheManager& cacheManager_;
// ...


#endif // EPOLL_SERVER_H

