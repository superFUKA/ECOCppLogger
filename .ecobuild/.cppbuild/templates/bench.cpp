// Benchmark: put the code to measure in measure() and run it in Release
// (ecobuild run --project <this Project> --configuration Release).
#include <chrono>
#include <cstdio>

#include "{{header}}"

namespace {

template <class Function>
double measure(const char* name, int repeat, Function function) {
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < repeat; ++i) {
        function();
    }
    const std::chrono::duration<double, std::micro> elapsed = std::chrono::steady_clock::now() - start;
    const double average = elapsed.count() / repeat;
    std::printf("%s: %.3f us/iteration (%d iterations)\n", name, average, repeat);
    return average;
}

}  // namespace

int main() {
    measure("example", 1000, [] {});
    return 0;
}
