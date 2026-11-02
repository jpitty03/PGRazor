#include "../test/egg_hatch_policy_test.cpp"
#include <cstdio>
int main(){int line=run_egg_tests();if(line)std::fprintf(stderr,"Egg policy failed at line %d\n",line);else std::puts("PASS native hatch policy");return line?1:0;}
