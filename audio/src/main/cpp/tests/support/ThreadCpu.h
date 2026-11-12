#pragma once

#include <algorithm>
#include <cstdint>
#include <ctime>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace wma_test {

#if defined(_WIN32)
// En Windows `CLOCK_THREAD_CPUTIME_ID` sale de GetThreadTimes, que avanza de a un tick del scheduler
// (15,625 ms): un callback de 0,5 ms mide 0 o 15,6 ms y el limite de "un periodo" (5,3 ms) salta al azar.
// QueryThreadCycleTime cuenta ciclos del hilo (sin el tiempo en que lo desalojan); se pasa a ns con una
// razon ciclos/ns calibrada contra el reloj de pared, tomando la MAYOR de varias rondas (una ronda
// desalojada da una razon menor, o sea ns de mas: nunca de menos).
inline int64_t threadCpuNs() {
    static const double cyclesPerNs = [] {
        LARGE_INTEGER freq;
        QueryPerformanceFrequency(&freq);
        double best = 0.0;
        for (int round = 0; round < 5; ++round) {
            LARGE_INTEGER w0, w1;
            ULONG64 c0 = 0, c1 = 0;
            QueryPerformanceCounter(&w0);
            QueryThreadCycleTime(GetCurrentThread(), &c0);
            do { QueryPerformanceCounter(&w1); } while ((w1.QuadPart - w0.QuadPart) * 1000 < freq.QuadPart * 5);
            QueryThreadCycleTime(GetCurrentThread(), &c1);
            const double wallNs = static_cast<double>(w1.QuadPart - w0.QuadPart) * 1e9 / static_cast<double>(freq.QuadPart);
            best = std::max(best, static_cast<double>(c1 - c0) / wallNs);
        }
        return best;
    }();
    ULONG64 cycles = 0;
    QueryThreadCycleTime(GetCurrentThread(), &cycles);
    return static_cast<int64_t>(static_cast<double>(cycles) / cyclesPerNs);
}
#else
inline int64_t threadCpuNs() {
    timespec ts{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}
#endif

}  // namespace wma_test
