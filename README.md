# DesktopUpdateKit

DesktopUpdateKit 是用于 Windows 桌面单 EXE 应用的共享更新组件，源码采用
[MIT License](LICENSE)。代码来源说明见 [PROVENANCE.md](PROVENANCE.md)，NativeAOT
发布涉及的第三方声明见 [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md)。

该仓库可由多个 Windows 单 EXE 项目通过 Git submodule 固定到经过验证的提交。

包含：

- `src\DesktopUpdateKit`：UpdateClient、UpdateLauncher 和更新模型。
- `src\UpdaterStub`：C# NativeAOT 更新 Stub，负责等待退出、备份、替换、健康检查和回滚，不携带第二份 .NET 运行库。
- `tools`：通用构建、资产生成和 GitHub Release 发布脚本。
- `docs\ABOUT_PAGE_GUIDELINES.md`：宿主项目关于页面和更新区域的推荐规范。
- `schemas`：`update.json` 与 `release.config.json` 的结构约束。

宿主项目通过 `release.config.json` 提供仓库名、资产名、构建路径和单文件压缩开关。
共享代码在宿主构建时链接编译；只有显式运行发布脚本时才访问 GitHub。资产准备工具
要求宿主仓库提供 `LICENSE`、`NOTICE` 和 `THIRD-PARTY-NOTICES.md`，并将这些文件与
DesktopUpdateKit 的 MIT 文本一起加入 Release 资产。
