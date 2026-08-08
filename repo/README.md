# HaoLang 包仓测试布局

| 目录 | 角色 |
|------|------|
| **`RegisterRepo/`** | **远程源数据**——只给 `script/haoreg_server.py --root` 用（与语言无关）。 |
| **`LocalRepo/`** | **测试用本地仓**——`HAO_REPO=repo/LocalRepo`；由 tidy 从 HTTP 源写入，用例前可清空。 |

语言默认本地仓仍是用户目录 `~/.hao/repo`；测试规范：

```powershell
python script/haoreg_server.py --root repo/RegisterRepo --port 8765
# 浏览器 http://127.0.0.1:8765 默认只看第一层，点目录下钻
$env:HAO_REGISTRY = "http://127.0.0.1:8765"
$env:HAO_REPO = "repo/LocalRepo"
hao mod tidy test/modsmoke/depsapp
# 测完务必停止 python 私服
```
