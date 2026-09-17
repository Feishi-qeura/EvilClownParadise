# CI 设置（自托管 runner）

`.github/workflows/ci.yml` 会在 push（main/ZANEN）和 PR（→main）时编译 Editor 与 Game 两个目标。因为 GitHub 云端 runner 没有 UE，需要用**自托管 runner**。

## 一次性设置

1. 选一台装好 UE 5.8 的 Windows 机器（建议团队共用一台，或每人一台注册同名 tag）。
2. GitHub 仓库页 → Settings → Actions → Runners → **New self-hosted runner** → Windows x64，按页面命令装好 agent 并注册为 Windows 服务（开机自启）。
3. 注册 runner 时 labels 至少包含：`self-hosted`、`windows`、`ue5`（workflow 用 `runs-on: [self-hosted, windows, ue5]` 匹配）。
4. 在 runner 机器设置环境变量（系统级）：
   - `UE_ROOT` = 引擎根目录，如 `D:\UE_5.8` 或 `C:\Program Files\Epic Games\UE_5.8`
   - workflow 里已写默认值兜底，若引擎不在默认路径必须设置此项。
5. runner 机器需要装 Git LFS（`git lfs install`），checkout 步骤已开 `lfs: true`。

## 带宽与费用注意

- LFS 对象按 GitHub 配额计费（免费 1GB 存储/1GB 月带宽）。首次 push 会把 renormalize 后的全部当前版本资产（约几十 MB～几百 MB）上传，先确认配额。
- runner 上 checkout 会下载 LFS 对象，多次触发 CI 会消耗带宽；低配额阶段可先只跑 `compile-editor` 一个 job。

## 之后可以加的检查（按需）

- `UnrealEditor-Cmd.exe -run=ResavePackages`（资产一致性）
- `-run=CookCommandlet` 试打包（发版前）
- 打包产物归档到 artifact / 内部网盘
