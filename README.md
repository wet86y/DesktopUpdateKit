# DesktopUpdateKit

本目录是 `D:\项目开发` 下多个 Windows 单 EXE 项目共用的本地更新工具。

包含：

- `src\DesktopUpdateKit`：UpdateClient、UpdateLauncher 和更新模型。
- `src\UpdaterStub`：C# NativeAOT 更新 Stub，负责等待退出、备份、替换、健康检查和回滚，不携带第二份 .NET 运行库。
- `tools`：通用构建、资产生成和 GitHub Release 发布脚本。
- `docs\ABOUT_PAGE_GUIDELINES.md`：宿主项目关于页面和更新区域的推荐规范。
- `schemas`：`update.json` 与 `release.config.json` 的结构约束。

项目通过 `release.config.json` 提供仓库名、资产名、构建路径和单文件压缩开关。共享代码只在本地被引用；只有显式运行发布脚本时，才会访问 GitHub。
