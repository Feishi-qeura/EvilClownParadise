# Content 目录结构与资产协作

## 目录分层

| 位置 | 性质 | 入库？ |
|---|---|---|
| `Content/_Game/` | 游戏正式资产 | ✅ 入库 |
| `Content/Developers/<个人名>/` | 个人沙盒，随便实验 | ❌ 不入库（磁盘保留，仅本机使用） |
| `Content/Collections/` | 本地资产集合（UE 本机数据） | ❌ 不入库 |
| `Content/zxx/` | 学习/演示素材 | ❌ 不入库，已从磁盘移出 |

> ⚠️ **`Content/Developers` 删不掉，不要试图删。** UE 会在编辑器启动时自动重建
> `Content/Developers/<当前用户名>/Collections`（实测：删掉整个目录后跑一次编辑器即恢复；
> 引擎的 `DisplayDevelopersFolder` 只控制 Content Browser 里是否显示，不控制磁盘创建）。
> 让它不进仓库的正确手段是 `.gitignore` 规则——已生效，跟踪数为 0。

> ⚠️ **gitignore 不能撤销已跟踪文件。** `Content/Developers` 曾被 `.gitignore` 忽略，但文件
> 早已跟踪，规则对已跟踪文件无效——结果"文档说不入库、实际入库 171 个"。让目录真正出库
> 只有一条路：`git rm -r --cached <路径>`。

学习素材的完整副本在 `E:\UnrealProjects\ECP_Archive`（仅本地保留，不入库、不随项目分发）。

## `_Game` 目标结构（按角色自包含）

**分层原则：角色的美术资产自包含在一个目录里；玩法蓝图与 AI 数据按职责单独分层。**

```
_Game/
  Characters/
    TestRobot/                  ← 每个角色一个目录，自包含
      SKM_TestRobot             ← 骨骼网格体
      SKEL_TestRobot            ← 骨架
      PHYS_TestRobot            ← 物理资产
      M_TestRobot               ← 主材质
      MI_TestRobot              ← 材质实例
      T_Polygon_Dummy_01        ← 贴图
      ABP_TestRobot             ← 动画蓝图
      Animations/               ← 该角色的动画序列
        A_Idle_Standing_Masc
        A_Run_F_Masc
        A_Walk_F_Masc
        A_Crouch_FwdStrafeFL_Masc
    <下一个角色>/                ← 加角色 = 加目录，不碰任何现有目录

  Blueprints/                   ← 玩法蓝图（非角色资产）
    BP_Player
    BP_ClownBase

  AI/                           ← AI 决策与控制器
    AIC_ClownBase
    StateTree/ST_MonsterBase

  Data/                         ← DataTable / Curve / DataAsset（有内容时再建）
  Maps/                         ← L_Test（当前启动地图，见 DefaultEngine.ini 的 GameMapsSettings）
  Inputs/                       ← IMC_Player、InputActions/IA_*
  UI/  Audio/  VFX/             ← 有内容时再建
```

**为什么按角色分而不是按资产类型分**（`Characters/` 放所有网格体、`Materials/` 放所有材质那种）：

1. 角色的网格体 / 骨架 / 材质 / 动画**强耦合**——骨架是把它们串起来的枢纽，动画绑定骨架、
   材质绑定网格体、物理资产绑定网格体。按类型拆开后，每加一个角色都要在 4~5 个目录里各放
   一份，还得靠命名去对应。
2. **加角色 = 加目录**，不修改任何现有目录，从结构上避免二进制资产冲突（uasset 不可合并，
   冲突只能丢弃一边）。
3. **退役角色 = 删一个目录**，不会有孤儿资产散落在各处。
4. 找人找东西是"小丑的东西在 `Characters/Clown/`"，而不是"去 `Characters/` 里翻"。
5. `git lfs lock` 的锁定范围与工作范围天然重合。

**动画归属**：随角色。动画绑定骨架，`A_Run_F_Masc` 只能给 `SKEL_TestRobot` 用，它就是这个
角色资产集的一部分。当动画变多（预计 100+）时，在 `Animations/` 内按用途再分子目录，直接复用
原沙盒那套已经被验证好用的分类：

```
Animations/
  Idle/
  Locomotion/{Walk,Run,Sprint,Crouch,Shuffle,Turn}/
  InAir/
  Transitions/<From>_To<To>/
```

> **关于跨角色共用动画**：将来若希望多个类人角色共用一套移动动画，走引擎的骨骼重定向
> （Retarget），那时再决定是否把它提升为 `Characters/Shared/`。**现在不要提前建空目录**——
> 空目录 git 不跟踪，克隆后不存在，反而让结构和文档不一致。

> **当前现实**：`BP_Player` 与 `BP_ClownBase` 共用同一套模型/骨架/动画（`TestRobot` 是占位资产）。
> 真实模型到位后各自建 `Characters/Player/`、`Characters/Clown/`，`TestRobot` 即可整体删除。

## 在编辑器里移动 / 重命名资产的标准做法

> **前置：先解锁，否则改动会静默丢失。** `.gitattributes` 给 `*.uasset`/`*.umap` 标了 `lockable`，
> Git LFS 会把未加锁的文件设成只读，编辑器的移动 / 保存会失败且只报
> `Cannot remove '...' as it is read only!`。
>
> ```bash
> # 把要动的资产一次性锁上（联网时用规范做法；离线可临时 chmod u+w）
> git lfs lock Content/_Game/Characters/*.uasset
> ```

**移动 + 改名一次做完**（编辑器会同时修正所有引用，不必手工重指）：

1. Content Browser 里选中资产 → 按 **F2**
2. 在名字框里输入**完整新路径**，例如
   `PolygonSyntyCharacter` 改为 `/Game/_Game/Characters/TestRobot/SKM_TestRobot`
   （填路径即同时完成移动与改名）

   或者分两步：拖到目标文件夹 → F2 改短名。两者等价。

3. 全部改完后，右键 `_Game` 文件夹 → **Fix Up Redirectors**（清理旧路径上的重定向器）
4. **Ctrl+Shift+S 保存全部**
5. 检查：`BP_Player` / `BP_ClownBase` 的 Mesh 组件仍指向网格体，动画蓝图仍有 Target Skeleton
6. 解锁：`git lfs unlock <路径>`（或 `git lfs unlock --all`）

**顺序建议**：先动被引用最多的（骨架、网格体），再动动画——这样中间态的重定向链最短。
不过编辑器都会自动处理，顺序不影响正确性。

### 为什么不要在编辑器外动这些资产

实测结论（本仓库踩过）：

| 做法 | 结果 |
|---|---|
| 脚本 `rename_asset` | 遇到只读文件退化成"复制 + 留原件"，引用方仍指向旧路径 |
| 脚本 `consolidate_assets` | 对 `SkeletalMesh` / `Skeleton` **反向操作**：把新资产变成指回旧路径的重定向器，并把蓝图的网格体赋值**置空** |
| 纯文件系统 `mv`/`cp` | 包名能从路径推导，但引用方存的是旧路径字符串，全部断链 |
| 临时 `+PackageRedirects` | **纯移动可以**（实测通过）；但**改名不行**——对象名也变了，导入里 `包.对象` 的对象部分找不到，需要对象级重定向，而 `+ObjectRedirects` 在 `FixupImportMap` 阶段实测未能匹配 |

**结论：移动和改名就在编辑器里做，别绕路。**

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
- **不要用 `SK_` 同时表示骨骼网格体和骨架**：网格体用 `SKM_`，骨架用 `SKEL_`。
- 资产名里不写类型名，蓝图类除外。
- 角色的资产用角色名做后缀（`SKM_TestRobot`、`ABP_Clown`），角色名换模型时不必改名。

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

## 五个高频坑（本仓库都发生过）

1. **启动地图指向沙盒目录** —— `EditorStartupMap` / `GameDefaultMap` 只允许指向 `_Game`
   或引擎内置地图。指向不入库的目录，同事拉库后必然报错。
2. **CoreRedirects 长期化** —— redirect 只在迁移期存在，资产处理完就删整行。
   历史上错误配置曾导致资产加载崩溃。
3. **编辑器开着时动仓库** —— 编辑器对文件有锁，移动 / 删除 Content 目录前先关编辑器。
4. **靠 gitignore 让已跟踪目录"出库"** —— 无效，必须 `git rm -r --cached`。
5. **加了锁才动资产** —— 见上；只读状态下移动/保存失败只给一行日志，很容易漏掉。
