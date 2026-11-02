#include "../test/manual_transfer_policy_test.cpp"
#include <cstdio>
int main(){int line=run_manual_tests();if(line)std::fprintf(stderr,"Manual policy failed at line %d\n",line);else std::puts("PASS native manual transfer policy");return line?1:0;}
