# 双实现能力矩阵

| 能力 | Managed/.NET | Native C++ | 归属 |
|---|---|---|---|
| GitHub Release 清单与固定 Tag URL | supported | supported | v1 contract |
| repository/资产/大小/SHA-256 严格校验 | supported | supported | v1 contract |
| HTTPS 节点、官方兜底与节点缓存 | supported | supported | v1 contract |
| Range 并行、单路回退、暂停与取消 | supported | supported | v1 contract |
| 进程级后台下载会话 | supported | supported | v1 contract |
| 有超时的停止与活动请求中止 | supported | supported | v1 contract |
| 安装前 SHA-256 复验 | supported | supported | v1 contract |
| 健康检查、替换、改名与回滚 | supported | supported | v1 contract |
| 自定义 Stub 字节来源 | supported | supported | host integration |
| WinForms/WPF/Win32 UI | host-specific | host-specific | host |
| 产品名、资源编号、规范 EXE 名和日志 | host-specific | host-specific | host |
| NuGet、预编译 SDK、稳定 C++ ABI | not applicable | not applicable | v1 out of scope |

Managed 参考宿主是笨蛋表格，Native C++ 参考宿主是超级中键。共享测试夹具位于
`tests/contracts`，两套测试必须读取同一份有效与恶意清单样本。
