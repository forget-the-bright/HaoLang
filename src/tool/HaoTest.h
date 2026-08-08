// ============================================================
//  hao test —— Go 式单元测试（*_test.hao + TestXxx）
// ============================================================
#pragma once

#include <string>
#include <vector>

namespace hao {

struct TestOptions {
    bool verbose = false;       // -v：打印 --- PASS 与 t.log
    std::string runPattern;     // -run：用例名正则（ECMAScript search）；空=全部
};

// paths 为空时：有 haoproject.json 或当前目录含 *_test.hao → `.`；否则报错。
// 不回退跑业务 main / test/suite。
// 产物在 target/test/hao-test/，跑完删除。
// 返回 0=全过；非 0=有失败。
int runTests(const std::vector<std::string>& paths, const TestOptions& opts);

} // namespace hao
