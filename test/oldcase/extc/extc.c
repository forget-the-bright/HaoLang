/* 手写 C 库，供 test/extc/extc.hao 通过 --link 直接链接验证 */
int hao_addext(int a, int b) { return a + b + 100; }
double hao_sq(double x) { return x * x; }