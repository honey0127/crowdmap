#pragma once
// CPU 배치: 리액터 전용 코어 1개 + 나머지 물리 코어를 워커 코어로 쓴다.
//
// 워커 수의 상한은 스레드 수가 아니라 물리 코어 수다. 같은 물리 코어의
// 하이퍼스레드는 처리량을 0.2~0.3배밖에 늘리지 못하므로 c_max 는 물리 코어로 센다.
#include <sched.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace cpu {

// "1-3,5" → {1,2,3,5}
inline std::vector<int> parseList(const std::string& s) {
    std::vector<int> out;
    std::stringstream ss(s);
    std::string part;
    while (std::getline(ss, part, ',')) {
        if (part.empty()) continue;
        const auto dash = part.find('-');
        try {
            if (dash == std::string::npos) {
                out.push_back(std::stoi(part));
            } else {
                const int a = std::stoi(part.substr(0, dash));
                const int b = std::stoi(part.substr(dash + 1));
                for (int i = a; i <= b; ++i) out.push_back(i);
            }
        } catch (...) {
            // 잘못된 항목은 무시
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

inline std::string formatList(const std::vector<int>& v) {
    std::string s;
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) s += ",";
        s += std::to_string(v[i]);
    }
    return s.empty() ? "-" : s;
}

inline int readIntFile(const std::string& path, int def) {
    std::ifstream f(path);
    int v = def;
    if (f) f >> v;
    return v;
}

// 이 프로세스가 쓸 수 있는 논리 CPU 를 물리 코어별로 묶는다 (첫 논리 CPU 번호 순).
inline std::vector<std::vector<int>> physicalCores() {
    cpu_set_t mask;
    CPU_ZERO(&mask);
    if (sched_getaffinity(0, sizeof(mask), &mask) != 0) return {};

    std::map<std::pair<int, int>, std::vector<int>> byCore;   // (package, core_id) → cpus
    for (int c = 0; c < CPU_SETSIZE; ++c) {
        if (!CPU_ISSET(c, &mask)) continue;
        const std::string base = "/sys/devices/system/cpu/cpu" + std::to_string(c) + "/topology/";
        const int pkg  = readIntFile(base + "physical_package_id", 0);
        const int core = readIntFile(base + "core_id", c);   // 정보 없으면 논리 CPU = 물리 코어
        byCore[{pkg, core}].push_back(c);
    }
    std::vector<std::vector<int>> cores;
    for (auto& [key, cpus] : byCore) cores.push_back(cpus);
    std::sort(cores.begin(), cores.end(),
              [](const auto& a, const auto& b) { return a.front() < b.front(); });
    return cores;
}

struct Plan {
    bool             pin         = false;
    int              reactorCpu  = -1;
    std::vector<int> reactorCpus;      // 리액터·배경 스레드(I/O, 컨트롤러, 청소)
    std::vector<int> workerCpus;       // 워커 스레드
    int              workerCores = 1;  // 워커가 쓸 수 있는 물리 코어 수 → c_max 기본값
    int              physCores   = 1;

    std::string describe() const {
        return "pin=" + std::string(pin ? "on" : "off")
               + " phys_cores=" + std::to_string(physCores)
               + " reactor_cpu=" + formatList(reactorCpus)
               + " worker_cpus=" + formatList(workerCpus)
               + " worker_cores=" + std::to_string(workerCores);
    }
};

// pinEnv: "auto"(기본) | "0"/"off"
// reactorEnv / workerEnv: 비우면 자동(첫 물리 코어 = 리액터, 나머지 = 워커)
inline Plan plan(const std::string& pinEnv, const std::string& reactorEnv,
                 const std::string& workerEnv) {
    Plan p;
    const auto cores = physicalCores();
    p.physCores = std::max<int>(1, static_cast<int>(cores.size()));

    // 논리 CPU → 물리 코어 번호
    std::map<int, int> coreOf;
    for (size_t i = 0; i < cores.size(); ++i)
        for (int c : cores[i]) coreOf[c] = static_cast<int>(i);

    const bool pinOff = (pinEnv == "0" || pinEnv == "off" || pinEnv == "false");

    if (!reactorEnv.empty()) {
        p.reactorCpus = parseList(reactorEnv);
    } else if (!cores.empty()) {
        p.reactorCpus = {cores.front().front()};
    }
    p.reactorCpu = p.reactorCpus.empty() ? -1 : p.reactorCpus.front();

    if (!workerEnv.empty()) {
        p.workerCpus = parseList(workerEnv);
    } else {
        // 리액터가 쓰는 물리 코어(하이퍼스레드 형제 포함)를 뺀 나머지
        std::set<int> reactorCores;
        for (int c : p.reactorCpus)
            if (coreOf.count(c)) reactorCores.insert(coreOf[c]);
        for (size_t i = 0; i < cores.size(); ++i) {
            if (reactorCores.count(static_cast<int>(i))) continue;
            for (int c : cores[i]) p.workerCpus.push_back(c);
        }
    }

    std::set<int> workerCoreSet;
    for (int c : p.workerCpus)
        workerCoreSet.insert(coreOf.count(c) ? coreOf[c] : 1000 + c);
    p.workerCores = std::max<int>(1, static_cast<int>(workerCoreSet.size()));

    // 물리 코어가 1개뿐이면 나눌 코어가 없으므로 고정하지 않는다
    p.pin = !pinOff && !p.reactorCpus.empty() && !p.workerCpus.empty();
    if (!p.pin) {
        p.workerCores = std::max(1, p.physCores - (p.physCores > 1 ? 1 : 0));
    }
    return p;
}

inline bool pinThisThread(const std::vector<int>& cpus) {
    if (cpus.empty()) return false;
    cpu_set_t mask;
    CPU_ZERO(&mask);
    for (int c : cpus)
        if (c >= 0 && c < CPU_SETSIZE) CPU_SET(c, &mask);
    return sched_setaffinity(0, sizeof(mask), &mask) == 0;   // 0 = 호출 스레드
}

// 호출 스레드의 nice 값 설정 (Linux 에서 setpriority 는 스레드 단위로 적용된다).
// nice 를 올리는(우선순위를 낮추는) 것은 권한 없이 가능하다.
inline bool setThreadNice(int nice) {
    const id_t tid = static_cast<id_t>(::syscall(SYS_gettid));
    return ::setpriority(PRIO_PROCESS, tid, nice) == 0;
}

}  // namespace cpu
