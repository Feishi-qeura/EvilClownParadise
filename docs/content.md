# Content 目录结构与资产协作

## 目录分层

| 位置 | 性质 | 入库？ |
|---|---|---|
| `Content/_Game/` | 游戏正式资产 | ✅ 入库 |
| `Content/Developers/<个人名>/` | 个人沙盒，随便实验 | ❌ 不入库（本地使用） |
| 学习/演示素材（原 `Content/zxx`） | 归档副本 | ❌ 不入库，副本在 `E:\UnrealProjects\ECP_Archive` |

`_Game` 内按类型分目录，目标是：

```
_Game/
  Maps/          正式关卡（启动地图 L_Startup 也在这里）
  Characters/    角色与动画
  AI/            StateTree、评估器、AI 数据
  Blueprints/    通用蓝图
  Inputs/        IMC 与 IA
  UI/            Widget
  Audio/         音频
  Data/          DataTable、Curve、DataAsset
```

> 现状迁移提示：`BP_Player`/`BP_ClownBase` 目前引用 `Developers/Zane/...` 的测试骨架，这是过渡状态——骨架迁移到 `_Game` 后即可解除（见 docs/tech-debt.md）。

## 命名规范

以 [Epic 官方资产命名表](https://dev.epicgames.com/documentation/unreal-engine/recommended-asset-naming-conventions-in-unreal-engine-projects)为准，本项目常用映射：

| 类型 | 前缀 | 示例 |
|---|---|---|
| GameMode / PlayerController / AIController | GM_ / PC_ / AIC_ | GM_InGame |
| InputMappingContext / InputAction | IMC_ / IA_ | IMC_Player |
| 蓝图 / 动画蓝图 | BP_ / ABP_ | BP_Player |
| 静态/骨架网格体 | SM_ / SK_（Skeletal Mesh 可用 SM_ 或 SKM_） | SKM_Masc |
| 骨架 / 动画序列 / 混合空间 / 动画蒙太奇 | SK_ / A_ / BS_ / MM_ | A_Run_F_Masc |
| 材质 / 材质实例 / 函数 | M_ / MI_ / MF_ | MI_TestRobot |
| 贴图 / 音频 / Curve | T_ / S_ / C_ | T_Polygon_Dummy_01 |

规则：只允许 `数字、字母、下划线`，禁止空格；资产名里不写类型名（蓝图类除外）。

## 资产协作规则（二进制不可合并）

1. **锁再改**：编辑共享资产前先 `git lfs lock Content/_Game/...`，改完 `git lfs unlock`。未锁就改，等于可能丢掉同事的工作。
2. **一人一资产**：同一时间同一资产只有一个人编辑，开改前在群里认领。
3. **先沟通再大改**：要重构别人写的蓝图/关卡，先在 PR 或群里说明拆法。
4. 关卡尽量拆分：持久关卡放流送关卡/World Partition，避免所有人改同一个.umap。

## 三个高频坑（本仓库发生过）

1. **启动地图指向沙盒目录**——`DefaultEngine.ini` 的 EditorStartupMap/GameDefaultMap 只允许指向 `_Game`（或引擎内置地图）。
2. **CoreRedirects 长期化**——redirect 只在迁移期存在，资产处理完就删整行。
3. **编辑器开着时动仓库**——编辑器对文件有锁，移动/删除 Content 目录前先关编辑器。
