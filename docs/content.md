# Content 目录结构与资产协作

## 目录分层

| 位置 | 性质 | 入库？ |
|---|---|---|
| `Content/_Game/` | 游戏正式资产 | ✅ 入库 |
| `Content/Developers/<个人名>/` | 个人沙盒，随便实验 | ❌ 不入库（磁盘保留，仅本机使用） |
| `Content/Collections/` | 本地资产集合（UE 本机数据） | ❌ 不入库 |
| `Content/zxx/` | 学习/演示素材 | ❌ 不入库，且已从磁盘移出 |

> ⚠️ **`Content/Developers` 删不掉，不要试图删。** UE 会在编辑器启动时自动重建
> `Content/Developers/<当前用户名>/Collections`（实测：删掉整个目录后跑一次编辑器即恢复；
> 引擎的 `DisplayDevelopersFolder` 设置只控制 Content Browser 里是否显示，不控制磁盘创建）。
> 让它不进仓库的正确手段就是下面这条 `.gitignore` 规则——已生效，跟踪数为 0。

> ⚠️ **gitignore 不能撤销已跟踪文件。** `Content/Developers` 曾被 `.gitignore` 忽略，但文件
> 早已跟踪，规则对已跟踪文件无效——结果"文档说不入库、实际入库 171 个"。让目录真正出库
> 只有一条路：`git rm -r --cached <路径>`（磁盘文件保留）。

学习素材的完整副本在 `E:\UnrealProjects\ECP_Archive`（仅本地保留，不入库、不随项目分发）。

## `_Game` 现有结构

结构与代码同步维护，改动请一并更新本表：

```
_Game/
  Maps/          关卡（L_Test 为当前启动地图，见 DefaultEngine.ini 的 GameMapsSettings）
  Characters/    角色资产：骨骼网格体、骨架、物理资产、材质、贴图
  Animations/    动画蓝图与动画序列（按用途分 Idle/、Locomotion/ 等子目录）
  Blueprints/    通用蓝图（BP_Player、BP_ClownBase）
  Data/          DataTable、Curve、DataAsset
    AI/          AIController 蓝图、StateTree（ST/ 子目录）
  Inputs/        IMC_ 与 IA_
```

> 尚未建立：`UI/`、`Audio/`、`VFX/`。有内容时再建，不要预建空目录——
> 空目录 git 不跟踪，克隆后不存在，反而让结构和文档不一致。

## 命名规范

以 [Epic 官方资产命名表](https://dev.epicgames.com/documentation/unreal-engine/recommended-asset-naming-conventions-in-unreal-engine-projects)为准。
本项目常用映射：

| 类型 | 前缀 | 示例 |
|---|---|---|
| GameMode / PlayerController / AIController | `GM_` / `PC_` / `AIC_` | `GM_InGame` |
| InputMappingContext / InputAction | `IMC_` / `IA_` | `IMC_Player` |
| 蓝图 / 动画蓝图 | `BP_` / `ABP_` | `BP_Player` |
| **骨骼网格体 / 骨架 / 物理资产** | `SKM_` / `SKEL_` / `PHYS_` | `SKM_TestRobot` |
| 动画序列 / 混合空间 / 动画蒙太奇 | `A_` / `BS_` / `AM_` | `A_Run_F_Masc` |
| 材质 / 材质实例 / 材质函数 | `M_` / `MI_` / `MF_` | `MI_TestRobot` |
| 贴图 / 音频 / Curve | `T_` / `S_` / `C_` | `T_Polygon_Dummy_01` |
| 关卡 | `L_` | `L_Test` |

规则：

- 只允许 `数字、字母、下划线`，禁止空格与中文。
- **不要用 `SK_` 同时表示骨骼网格体和骨架**（本文件早期版本有此自相矛盾）：
  网格体用 `SKM_`，骨架用 `SKEL_`。
- 资产名里不写类型名，蓝图类除外。

> 历史遗留：`PolygonSyntyCharacter` / `_Skeleton` / `_PhysicsAsset` 三个名字缺前缀
> （来自 Polygon Synty 素材包原名）。在编辑器里用 F2 改名即可安全完成（编辑器会
> 自动修正引用），不影响任何逻辑。

## 资产协作规则（二进制不可合并）

### 锁再改 —— 这条规则有硬强制，不是自觉

`.gitattributes` 给 `*.uasset` / `*.umap` 标了 `lockable`，**Git LFS 会把未加锁的这类文件
在工作区设为只读**。表现是：

- 没加锁就改共享资产 → 编辑器保存时报 `Cannot remove '...' as it is read only!`，
  改动**静默丢失**。
- `git status`、`git add` 不会改变只读状态；只有 `git lfs lock` / `git lfs unlock` 会。

```bash
git lfs lock   Content/_Game/Blueprints/BP_Player.uasset   # 加锁 → 文件变为可写
# ... 在编辑器里编辑 ...
git lfs unlock Content/_Game/Blueprints/BP_Player.uasset   # 释放 → 变回只读
```

`git lfs lock` 需要联网。离线时可用 `attrib -R <文件>`（或 git bash 的 `chmod u+w <文件>`）
临时放开，但这等于放弃互斥保护，联网后请补 `git lfs lock`。

其余规则：

1. **一人一资产**：同一时间同一资产只有一个人编辑，开改前在群里认领。
2. **先沟通再大改**：要重构别人写的蓝图/关卡，先在 PR 或群里说明拆法。
3. 关卡尽量拆分：持久关卡放流送关卡 / World Partition，避免所有人改同一个 `.umap`。

### 批量迁移被引用的资产

**不要**用脚本 `rename_asset` 或 `consolidate_assets` 搬迁被引用的资产：

- `rename_asset` 遇到只读文件会退化成"复制 + 留原件"，引用方仍指向旧路径；
- `consolidate_assets` 对 `SkeletalMesh` / `Skeleton` 会**反向操作**，把新资产变成指回
  旧路径的重定向器，并把蓝图的网格体赋值置空。

可靠路径是编辑器内拖拽 / F2 改名。必须用脚本时走"临时包重定向"：

1. 复制文件到新位置，**保留原文件名**（只有包路径变、对象名不变）；
2. `DefaultEngine.ini` 的 `[CoreRedirects]` 加临时
   `+PackageRedirects=(OldName="旧包路径",NewName="新包路径")`；
3. 编辑器内重存引用方**和被迁移资产本身**（后者之间的交叉引用要一起改）；
4. 移除临时重定向。

## 四个高频坑（本仓库都发生过）

1. **启动地图指向沙盒目录** —— `EditorStartupMap` / `GameDefaultMap` 只允许指向 `_Game`
   或引擎内置地图。指向不入库的目录，同事拉库后必然报错。
2. **CoreRedirects 长期化** —— redirect 只在迁移期存在，资产处理完就删整行。
   历史上错误配置曾导致资产加载崩溃。
3. **编辑器开着时动仓库** —— 编辑器对文件有锁，移动 / 删除 Content 目录前先关编辑器。
4. **靠 gitignore 让已跟踪目录"出库"** —— 无效，必须 `git rm -r --cached`。
