# 共享下载架构

`DesktopUpdateKit` 是所有 Windows 单 EXE 项目唯一的更新下载实现位置。项目自身只能提供更新页面、按钮、进度文本和本项目的 `release.config.json`，不得复制或自行演进下载协议、分块策略或校验逻辑。

## 当前能力

- 更新清单仍从配置的 GitHub Release 读取；资产下载地址由配置的仓库和资产名构造，不信任远端清单中的任意下载域名。
- 完整更新 EXE 在大于 16 MiB 时，先以 HTTP `Range: bytes=0-0` 探测服务端支持。
- 探测成功后，按均匀字节区间使用最多 4 个 HTTPS 连接并行下载；连接数、阈值和缓冲区大小通过 `UpdateDownloadOptions` 复用配置。
- 任意分块未返回正确的 `206 Partial Content` 或 `Content-Range` 时，自动删除不完整文件并退回单连接下载。
- 所有下载最终仍校验完整 EXE 的 SHA-256；分块完成不代表更新可信。
- `UpdateDownloadControl` 提供暂停/继续控制；取消由调用方的 `CancellationToken` 终止，并清理本次临时目录。

当前版本不会在应用退出或取消后保留 `.part` 文件，因此尚不提供跨进程断点续传。若以后加入持久断点、镜像择优、增量补丁或测速回退，均必须在本目录内实现，并保持完整 EXE + SHA-256 回退路径。

## 项目接入边界

项目通过链接编译 `src\DesktopUpdateKit\UpdateClient.cs`、`UpdateModels.cs` 和 `UpdateLauncher.cs` 接入：

- 项目 UI 可以传入进度回调、暂停控制和取消令牌。
- 项目 UI 不得创建 HTTP Range 请求、管理分段文件、合并字节块或自行计算更新包完整性。
- 系统 DNS、`hosts`、代理和网络设置由用户及操作系统管理；共享组件不得改写它们，也不得硬编码 GitHub/CDN IP。

## 验收要求

每次修改下载逻辑，至少验证：支持 Range 的资产并行完成、拒绝 Range 的资产自动回退、暂停/继续、取消清理、SHA 不匹配清理，以及中文和带空格安装路径的完整更新流程。
