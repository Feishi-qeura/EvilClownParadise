# 技术债与后续任务清单

> 完成一项勾一项，并把对应提交链接附在条目后。新发现的问题也追加到这里。

## 需要在编辑器里完成（代码侧无法代劳）

- [ ] **创建正式启动地图** `/Game/_Game/Maps/L_Startup`（含 GameMode/玩家出生点），把 `DefaultEngine.ini` 中 `EditorStartupMap` / `GameDefaultMap` 两行回填（TODO 注释已标好位置）。
- [ ] **骨架迁移**：把 `/Game/zxx/Character/PolygonSyntyCharacter_Skeleton` 移到 `/Game/_Game/Animations/`（Content Browser 内拖动），右键引用它的 `ABP_ECPCommon` → Fix Up Redirectors，保存全部 → 之后 `Content/zxx` 目录即可整体移出（git 层已出库，磁盘副本在 ECP_Archive）。
- [ ] **清理 CoreRedirects**：打开引用 `my_struct` / `ECPTaskCase` 的资产（StateTree 任务实例数据等），Fix Up Redirectors + 重存，验证 `loadErrors` 为空后删除 DefaultEngine.ini 里两行 redirect。⚠️ 删除前先重存资产，历史上跳过这步曾导致加载崩溃。
- [ ] **TestRobot → 正式资产迁移计划**：`BP_Player`/`BP_ClownBase` 仍引用 `Developers/Zane/Assets/TestRobot` 的网格体与动画蓝图（沙盒目录）。目标：正式角色资产进 `_Game/Characters/`，蓝图重新指定引用，然后 `Content/Developers` 从仓库彻底消失（gitignore 已生效，等迁移完成后 `git rm -r --cached Content/Developers`）。

## 需要决策/协作的事项

- [ ] **远程分支改名/删除**（见 docs/branching.md）：`FEISHI_NULL`、`ZANEN_Test`、`SANC` 建议删除；`ZANEN`/`FEISHI` 成果合入 main 后改名退役；开保护规则。
- [ ] **LFS 首次 push 前确认配额**：GitHub LFS 免费 1GB 存储/月带宽；如需先 `git lfs migrate import --everything` 重写历史彻底瘦身（需要 force push + 全员重新 clone，安排一次同步）。
- [ ] **GameInstance**：接 Steam 大厅时换成自定义 `UGCPGameInstance`（挂 FUOnlineSession），替换 `GameInstanceClass=/Script/Engine.GameInstance`。
- [ ] **`r.Substrate=True` 决策**：Substrate 影响整个材质管线，确认美术管线是否真需要；若不用，整段关闭回到默认。
- [ ] **FUOnlineSession 插件的 `Config/Engine.ini` 归位**：插件携带 Engine.ini 会覆盖项目引擎配置（Steam/P2P 参数）。在打包验证通过的前提下，把键迁移到 `DefaultFUOnlineSession.ini` 或项目 `Config/`，删掉插件里的 Engine.ini。
- [ ] **学习工程独立化**：zxx 素材副本（`ECP_Archive/Content_zxx`）建议将来 push 成独立仓库或迁回各自学习工程；本仓库不再收学习内容。

## 已完成（2026-09-17 正规化改造）

- [x] 学习资产/插件演示内容/IDE 工具出库，651MB 完整归档 `E:\UnrealProjects\ECP_Archive`
- [x] `.gitignore` 白名单重写；`.gitattributes` + Git LFS（196 个二进制）
- [x] 硬件目标 Desktop/Maximum；启动地图解绑个人目录；重复配置段清理；Build.cs 修正
- [x] `.clang-format`/`.editorconfig` 入库；docs/ 六篇规范文档；CI 脚手架
