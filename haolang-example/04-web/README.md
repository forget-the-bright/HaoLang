# 04 · Web / MVC

用注解控制器 + `HttpApp.scan("demo/web")` 注册嵌套包路由（含 `demo/web/api`）。

v0.49 起还演示：

- 控制器直接返回 Bean → JSON、返回 `String` → text
- `Html.render` 模板页
- `app.staticFiles("/static", dir)` 静态文件

## 目录

```text
04-web/
  main.hao
  static/                 # staticFiles 挂载目录
  demo/web/Home.hao
  demo/web/api/Api.hao
```

## 运行

```powershell
hao run haolang-example\04-web\main.hao
```

默认在 `18080` 上自测后退出。预期含：

```text
web-home
api-v1
api:zz
"name":"Ada"
<h1>HaoLang</h1>
static-hello
web demo done
```

## 常驻服务

把 `main.hao` 里自测段换成：

```hao
app.serve();   // Ctrl+C 结束
```

然后访问例如：

- `http://127.0.0.1:18080/web/home`
- `http://127.0.0.1:18080/web/api/user`
- `http://127.0.0.1:18080/web/hi`
- `http://127.0.0.1:18080/static/hello.txt`
