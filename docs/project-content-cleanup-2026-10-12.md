# Project 内容整理记录

只清理 `Content/Project`。素材包原件、演示地图及 BMMCSystem 联机大厅保持原路径和文件内容。

## 三张生产关卡

| 用途 | 资产路径 |
| --- | --- |
| 主菜单 | `/Game/Project/World/Maps/LegacyZxx/Test_Map` |
| 联机大厅 | `/Game/zxx/BMMCSystem/Maps/SinglePlayer/SinglePlayerMap` |
| 游戏地图 | `/Game/Project/World/Maps/Demo` |

主菜单仍是启动地图。Demo 设置为现有 `BP_ECGameMode`，使用 `BP_ECPlayertest` 和 `BP_ECPlayerController`。三张地图写入 Packaging 的 MapsToCook。

## 资产与引用

- 从旧 Test_Map 目录复制 Demo 及依赖共 378 个包；环境资源位于 `Project/World/Environment`，由 ECPWorld 内容目录与标签管理。
- 通过 UE AssetTools 的 AdvancedCopy 包头重写接口复制硬、软引用，未用文件重命名方式迁移资产。
- BP_Actor 的门模型改用新目录资源；OpenLevel 参数改为 Demo 的完整新路径，避免原素材包同名地图造成歧义。原有 Box 事件和服务器人数条件保留。
- 删除 `Project/World/Maps/L_Test` 及其独占的 `Interaction/Framework/LegacyGame` 两个资产、`Interaction/Input/LegacyGame` 六个资产，共 9 个包，并移除 GameFeature 标签中的对应条目。
- 独立 AI、可复用动画、原生代码动态加载和测试需要的资产保留。此次没有把“没有关卡直接引用”等同于“没用”。
- `Content/Test_Map` 原件暂未删除。整个 Project 和现有大厅的递归依赖中已没有 `/Game/Test_Map`；后续删除旧目录不会移除本次迁入的生产资源，但也会删除旧目录内原版 Demo/Preview 演示图。

完整新旧路径和删除清单：`project-content-cleanup-2026-10-12.json`。结构校验结果：`project-content-cleanup-verification-2026-10-12.json`。

## 验证结果与范围

- Editor 编译通过。
- 三张生产关卡均通过编辑器加载检查（同一进程依次加载三张地图）。
- 六项自动化通过：GameFeature ManifestValidation、BPActorWiring、CurrentWorldPlayerCount、TwoPlayerBoxPIE、InventoryCount、MeshRootAndFocus。
- Windows Cook 显式禁止旧 `Content/Test_Map` 目录，结果成功：0 errors；1,537 个包完成 Cook，13 个按平台跳过。
- Project 中无旧源目录依赖、无删除资产残留引用、无新增缺失依赖；378 个目标包全部可在注册表中找到。
- Project 外全部 2,099 个 Content 文件 SHA-256 与整理前一致，其中包括 BMMCSystem 的 464 个文件和 Test_Map 的 410 个文件。
- 最终只读复审通过；复审者独立重算上述 2,099 个文件的哈希，结果一致。
- Cook 有 28 条告警：禁用 Steam 的提示、BMMCSystem 原有 GameplayTag，以及 M_Decal_7 缺失输入贴图。原版 M_Decal_7 同样没有贴图依赖，迁入版本保持原内容；该贴花可能显示默认材质。
- 未进行本次三关卡完整联机跳转、跨机器 Steam 或画面验收。双人 PIE 通过的是已有 Box 人数检测测试，不等同于完整关卡流程验收。

验证日志、自动化报告和审计脚本位于 `Saved/ProjectCleanupValidation`。Cook 使用 Zen 存储，不应以输出目录没有逐个 `.umap` 文件判断 Cook 失败。

## 备份与恢复

备份位于 `Saved/ProjectCleanupValidation/Backup`，请在接受整理结果前保留该目录：

- `files.json`：Project、Test_Map、Config 共 684 个整理前文件，全部校验 SHA-256。
- `feature-labels.json`：两个修改前 GameFeature 标签及备份文件。
- `external-content.json`、`protected-bmmc.json`：Project 外及受保护目录的整理前哈希记录。

需要撤回时先关闭 Unreal Editor，并备份整理后新增的修改。按清单从 Backup 的同名相对路径恢复被删除的 9 个包、BP_Actor、DefaultGame.ini 和两个标签；只移除迁移清单 `copy` 中列出的 378 个新目标包。不要覆盖整个 Content，也不要删除素材包或全局修复重定向器。

`ECPProjectCleanupCommandlet` 仅供此次离线迁移审计，固定读取 docs 下的清单；Copy 模式拒绝覆盖已存在目标。当前迁移已经完成，无需再次运行。
