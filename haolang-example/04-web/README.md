# 04 · Web / MVC

用注解控制器 + `HttpApp.scan("demo/web")` 注册嵌套包路由（含 `demo/web/api`）。

## 目录

```text
04-web/
  main.hao
  demo/web/Home.hao
  demo/web/api/Api.hao
```

## 运行

```powershell
hao run haolang-example\04-web\main.hao
```

默认在 `18080` 上自测三条请求后退出。预期含：

```text
web-home
api-v1
api:zz
web demo done
```

## 常驻服务

把 `main.hao` 里自测段换成：

```hao
app.serve();   // Ctrl+C 结束
```

然后浏览器或 curl 访问 `http://127.0.0.1:18080/web/home`。
