#include <cassert>
#include <cstdio>
#include "../src/llamaste/hwdetect.h"

int main() {
    HardwareInfo hw = detect_hardware();

    assert(hw.cpu_cores >= 1);
    printf("PASS: cpu_cores=%d\n", hw.cpu_cores);

    assert(hw.ram_total_mb > 0);
    printf("PASS: ram_total_mb=%d\n", hw.ram_total_mb);

    assert(!hw.cpu_model.empty());
    printf("PASS: cpu_model=%s\n", hw.cpu_model.c_str());

    printf("\nAll hardware detection tests passed.\n");
    return 0;
}
