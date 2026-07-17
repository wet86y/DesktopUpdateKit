# DesktopUpdateKit v1 行为契约

## 产品边界

v1 仅服务 Windows 单 EXE 应用。版本源是配置仓库的 GitHub `releases/latest/download/update.json`；
下载对象必须是清单固定 Tag 下的完整 EXE 和对应 SHA-256 文件。第三方节点只能包装固定的官方 URL，
不能改变版本、资产或校验值。

## 共同语义

- 清单的 repository、asset 和 sha256Asset 必须与宿主配置完全一致；版本、Tag、大小、SHA-256 和非空节点列表必须有效。
- `github-direct` 必须存在、启用并规范化为 `{url}`；其他节点必须使用 HTTPS 且模板只包含一个 `{url}`。
- 下载前验证独立 SHA-256 资产与清单一致，下载后验证完整 EXE 的大小和 SHA-256。
- 大文件可在严格验证 `206` 与 `Content-Range` 后并行下载；任何分块错误先在同一节点回退单连接。
- 暂停不销毁会话；取消终止活动请求并清理本次临时目录；关闭 UI 默认暂停，显式后台模式继续。
- 安装 Stub 等待宿主退出，在目标目录暂存并再次验证 EXE，备份旧文件，启动新版并等待健康标记；失败时回滚。
- 更新健康标记参数为 `--update-health <absolute-path>`；更新和改名事务命令分别为 `--transaction` 与 `--rename-transaction`。

## 兼容政策

`update.json` 与 `release.config.json` 保持向后兼容。事务 JSON 是同一版本宿主与其内嵌 Stub 的内部契约；
新增字段必须可选读取。Managed 旧版无安装前复验的事务仍可执行一个兼容周期，新版宿主必须写入
`ExpectedSha256`。任何只在单一语言实现中新增的能力必须先记录为 `host-specific`，或提升共同契约版本。
