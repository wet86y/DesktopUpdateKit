# 共享下载架构

`DesktopUpdateKit` 是所有 Windows 单 EXE 项目唯一的更新下载实现位置。项目自身只能提供更新页面、按钮、进度文本和本项目的 `release.config.json`，不得复制或自行演进下载协议、分块策略或校验逻辑。

## 当前能力

- 更新清单仍从配置的 GitHub Release 读取；官方原始资产地址由配置的仓库、Tag 和资产名构造，不使用远端清单的 `downloadUrl` 字段。远程节点只能通过受校验的 `{url}` 模板包装该官方地址。
- 完整更新 EXE 在大于 16 MiB 时，先以 HTTP `Range: bytes=0-0` 探测服务端支持。
- 探测成功后，按均匀字节区间使用最多 4 个 HTTPS 连接并行下载；连接数、阈值和缓冲区大小通过 `UpdateDownloadOptions` 复用配置。
- 任意分块未返回正确的 `206 Partial Content` 或 `Content-Range` 时，自动删除不完整文件并退回单连接下载。
- 所有下载最终仍校验完整 EXE 的 SHA-256；分块完成不代表更新可信。
- `UpdateDownloadControl` 提供暂停/继续控制；取消由调用方的 `CancellationToken` 终止，并清理本次临时目录。

当前版本不会在应用退出或取消后保留 `.part` 文件，因此尚不提供跨进程断点续传。若以后加入持久断点、镜像择优、增量补丁或测速回退，均必须在本目录内实现，并保持完整 EXE + SHA-256 回退路径。

## GitHub 下载节点

完整 EXE 可以通过 `update.json` 的 `downloadNodes` 数组提供下载节点。每个节点均为统一结构：

```json
{
  "id": "gh-proxy",
  "template": "https://gh-proxy.com/{url}",
  "priority": 10,
  "enabled": true
}
```

`{url}` 只会替换为当前清单对应的、带固定 Tag 的官方 GitHub Release EXE 地址；不会使用 `latest/download`，从而避免下载期间发布新版本导致资产与 SHA 不一致。内置节点作为最终回退包含 `gh-proxy`、`gh-llkk`、`ghproxy-net` 和 `github-direct`。官方节点始终强制启用且模板固定为 `{url}`。

节点选择顺序是：上次通过完整大小与 SHA-256 校验的节点、当前远程节点优先级、GitHub 官方。远程节点缺失或无效时使用 `%LocalAppData%\<application-id>\update-node-cache.json` 中最长 7 天的节点目录；缓存也不可用时使用内置节点。缓存仅保存节点配置、最后成功节点和延迟，不保存代理账号、令牌或下载内容。

每个节点按顺序使用 `GET` 与 `Range: bytes=0-65535` 探测，不使用 HEAD，不并发测速。探测要求 5 秒内收到非 HTML 的 EXE 响应；`206` 且 `Content-Range` 正确时可使用分块下载，`200` 时只允许单连接下载。节点实际下载最多重试一次；连接阶段 5 秒超时、任何一次读取连续 20 秒无数据即切换下一个节点。所有节点最终都必须通过清单大小、发布 SHA 文件和 EXE SHA-256 三重校验。

四路 Range 下载中任一路出现超时、连接中断、错误响应或不完整区间时，不会立刻切换节点：先删除分块临时文件，并在同一节点回退为单连接下载；该单连接最多重试一次后才会尝试下一个节点。进度会明确显示“4 路失败，已回退单路”。

下载速率按最近两秒的滑动窗口计算，而非从下载开始至今的累计平均值。进度同时带有节点 ID 和实际连接数，便于区分节点限速、单连接回退与四路 Range 下载。

## 进程级下载会话

`UpdateDownloadSession` 保存下载任务、暂停控制、进度、错误和已校验的临时 EXE 路径。窗口关闭不再销毁会话：默认调用 `PauseWhenUiCloses` 暂停下载；用户显式调用 `ContinueInBackground` 后，窗口可关闭而下载继续。后台下载完成后，调用方必须要求用户明确触发安装，不应在用户无感知时退出主程序。

## 项目接入边界

项目通过链接编译 `src\DesktopUpdateKit\UpdateClient.cs`、`UpdateModels.cs` 和 `UpdateLauncher.cs` 接入：

- 项目 UI 可以传入进度回调、暂停控制和取消令牌。
- 项目 UI 不得创建 HTTP Range 请求、管理分段文件、合并字节块或自行计算更新包完整性。
- 项目 UI 只能显示共享组件报告的节点 ID、进度与失败状态；不得为任何节点写专用下载逻辑。
- 项目 UI 可在关闭时调用共享会话的暂停或后台继续操作，但不得自行持有下载线程、取消令牌或临时 EXE 路径。
- 系统 DNS、`hosts`、代理和网络设置由用户及操作系统管理；共享组件不得改写它们，也不得硬编码 GitHub/CDN IP。

## 验收要求

每次修改下载逻辑，至少验证：支持 Range 的资产并行完成、拒绝 Range 的资产自动回退、暂停/继续、取消清理、SHA 不匹配清理，以及中文和带空格安装路径的完整更新流程。
