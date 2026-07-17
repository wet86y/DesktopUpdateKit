# DesktopUpdateKit

DesktopUpdateKit 是用于 Windows 桌面单 EXE 应用的共享更新组件，源码采用
[MIT License](LICENSE)。代码来源说明见 [PROVENANCE.md](PROVENANCE.md)，NativeAOT
发布涉及的第三方声明见 [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md)。

该仓库可由多个 Windows 单 EXE 项目通过 Git submodule 固定到经过验证的提交。

仓库提供同一更新协议的两个正式语言前端。它们各自采用宿主语言的自然接口，不通过 DLL、C ABI 或
P/Invoke 相互包装：

- `src\DesktopUpdateKit`：Managed/.NET 的 UpdateClient、UpdateLauncher、会话和更新模型。
- `src\UpdaterStub`：C# NativeAOT 更新 Stub，负责等待退出、备份、替换、健康检查和回滚，不携带第二份 .NET 运行库。
- `native`：C++20/Win32 静态 SDK 与原生更新 Stub。
- `tools`：通用构建、资产生成和 GitHub Release 发布脚本。
- `docs\ABOUT_PAGE_GUIDELINES.md`：宿主项目关于页面和更新区域的推荐规范。
- `docs\BEHAVIOR_CONTRACT.md`：两套实现共同遵守的 v1 行为与兼容契约。
- `docs\COMPATIBILITY_MATRIX.md`：双实现能力矩阵与宿主边界。
- `schemas`：`update.json` 与 `release.config.json` 的结构约束。

当前产品边界固定为 Windows 单 EXE 自更新：从 GitHub Release 获取完整 EXE，验证大小与 SHA-256，
再以独立 Stub 完成替换、健康检查和回滚。DesktopUpdateKit 不是任意 URL 下载 SDK，也不处理多文件安装包。

## 源码接入

.NET 宿主通过 submodule 固定仓库提交，编译 `src\DesktopUpdateKit\*.cs`，并将 NativeAOT Stub 以
`DesktopUpdateKit.Resources.UpdaterStub.exe` 资源名嵌入最终 EXE。C++ 宿主通过
`add_subdirectory(<DesktopUpdateKit>/native)` 链接 `desktop_update_kit_native`，并自行把原生 Stub 字节
作为资源交给 `launch_update`/`launch_rename`。

最小配置在两种语言中均包含 application ID、`owner/repository`、EXE 资产名、SHA-256 资产名和当前版本。
UI、产品名称、资源编号、规范 EXE 文件名、日志和退出流程均由宿主维护。

## 独立验证

```powershell
dotnet test .\tests\DesktopUpdateKit.Tests\DesktopUpdateKit.Tests.csproj -c Release
dotnet publish .\src\UpdaterStub\UpdaterStub.csproj -c Release -r win-x64 --self-contained true -o .\build\managed-stub-test
.\tests\Test-ManagedStub.ps1 -StubPath .\build\managed-stub-test\UpdaterStub.exe
.\native\scripts\build-native.ps1 -Configuration Release
```

宿主项目通过 `release.config.json` 提供仓库名、资产名、构建路径、单文件压缩开关和
`releaseVerificationArguments`。每个验证参数都必须是宿主 EXE 支持的无副作用命令行开关，并以退出码报告结果。
共享构建会先清理宿主 Release 增量输出，发布后执行这些验证；资产准备前会对同一 EXE 再验证一次。
共享代码在宿主构建时链接编译；只有显式运行发布脚本时才访问 GitHub。资产准备工具
要求宿主仓库提供 `LICENSE`、`NOTICE` 和 `THIRD-PARTY-NOTICES.md`，并将这些文件与
DesktopUpdateKit 的 MIT 文本一起加入 Release 资产。
