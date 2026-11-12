"""Exercise Showcase protection inside the production evolution/transfer queue."""
import test_showcase_tag_runtime_native as guards


class ShowcaseEvolutionTransferTests(guards.ShowcaseTagRuntimeTests):
    SOURCE = 'evolve_transfer_runtime_test.cpp'
    MARKER = 'PASS evolve/transfer'
    HEADERS = {
        'android/log.h': '#pragma once\n#define ANDROID_LOG_INFO 4\n'
                         '#define ANDROID_LOG_ERROR 6\n'
                         'inline int __android_log_print(int,const char*,const char*,...){return 0;}\n',
        'unistd.h': '#pragma once\n#include <time.h>\n'
                    '#ifndef CLOCK_BOOTTIME\n#define CLOCK_BOOTTIME 7\n#endif\n'
                    'inline int fixture_tid(){return 71;}\n'
                    'inline int fixture_clock(int,timespec* t){t->tv_sec=100;t->tv_nsec=0;return 0;}\n'
                    '#define gettid fixture_tid\n#define clock_gettime fixture_clock\n',
    }


if __name__ == '__main__':
    import unittest
    unittest.main()
