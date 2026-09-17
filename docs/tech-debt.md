# 技术债与后续任务清单

> 完成一项勾一项，并把对应提交链接附在条目后。新发现的问题也追加到这里。

## 需要在编辑器里完成（代码侧无法代劳）

- [ ] **资产文件重命名到命名规范**（`docs/content.md` 命名表要求 `SKM_`/`SKEL_`/`PHYS_` 前缀）。
      在 Content Browser 里选中 → F2 改名，编辑器会自动修正引用，无风险：
      | 现名 | 建议名 |
      |---|---|
      | `_Game/Characters/PolygonSyntyCharacter` | `SKM_TestRobot` |
      | `_Game/Characters/PolygonSyntyCharacter_Skeleton` | `SKEL_TestRobot` |
      | `_Game/Characters/PolygonSyntyCharacter_PhysicsAsset` | `PHYS_TestRobot` |
- [ ] **清理 CoreRedirects**：打开引用 `my_struct` / `ECPTaskCase` 的资产（StateTree 任务实例数据等），
      Fix Up Redirectors + 重存，验证 `loadErrors` 为空后删除 `DefaultEngine.ini` 里两行 redirect。
      ⚠️ 删除前先重存资产，历史上跳过这步曾导致加载崩溃。
- [ ] **Cook 打包验证**：跑一次 `-run=CookCommandlet` 或 UAT BuildCookRun，确认打包产物能启动。
      当前 CI 只编译，从未验证过"能不能出包"。
- [ ] **PIE 实机回归**：迁移资产后确认玩家能移动/跳跃/蹲伏/冲刺，怪物能巡逻→发现→追击→攻击。
      （引用已在资产注册表层面验证为 0 残留，但运行时行为需要人眼确认。）

## 需要决策/协作的事项

- [ ] **剩余个人分支清理**（见 docs/branching.md）：`ZANEN`、`ZANEN_Test` 已删除（成果已并入 main
      或经本人确认无用）；`FEISHI`（在线会话工作，应作为功能提 PR 合入 main 后退役）、
      `FEISHI_NULL`、`SANC` 仍待各自处理。
- [ ] **开启 main 分支保护**：Settings → Branches → Add rule（require PR、require review、
      require CI pass）。否则"合并一律走 PR"只是自觉，直推 main 不会被拦。
- [ ] **确认后可删标签 `archive/zanen-test-online`**：它是 `ZANEN_Test` 那份在线登录实验
      （`ECPOnlineLoginAsync` 126 行 + `WBP_TestSteam` UI）的**唯一留存副本**。本人已确认该工作
      无用；若永久不需要，执行
      `git tag -d archive/zanen-test-online && git push origin --delete archive/zanen-test-online`。
- [ ] **LFS 历史瘦身**：`.git` 目前约 662MB，其中 `.git/objects` 609MB —— LFS 只接管了当前版本
      （196 个），历史里的二进制仍在。彻底瘦身需 `git lfs migrate import --everything` +
      force push + 全员重新 clone，安排一次团队同步窗口。
- [ ] **GameInstance**：接 Steam 大厅时换成自定义 `UGCPGameInstance`（挂 FUOnlineSession），
      替换 `GameInstanceClass=/Script/Engine.GameInstance`。
- [ ] **渲染管线决策**：`r.Substrate=True`、`r.RayTracing=True`、`r.PathTracing=True`、
      `r.Lumen.HardwareRayTracing=True` 目前都开着。Substrate 影响整个材质管线，
      光追影响性能预算——在美术管线定型前应一并决策，不要默认沿用。
- [ ] **FUOnlineSession 插件的 `Config/Engine.ini` 归位**：插件携带 Engine.ini 会覆盖项目引擎配置
      （Steam/P2P 参数）。在打包验证通过的前提下，把键迁移到 `DefaultFUOnlineSession.ini`
      或项目 `Config/`，删掉插件里的 Engine.ini。
- [ ] **AI 参数数据驱动化**：怪物（6 个）、移动（约 22 个）、相机（约 30 个）参数目前全是类默认值
      UPROPERTY。出现第二种怪物之前应迁到 DataAsset / DataTable，否则会变成蓝图默认值重复 +
      调参合并冲突。`docs/tech-debt.md` 之外无任何设计记录，建议先出一页方案。
- [ ] **在线子系统未接线**：`OnlineSubsystemSteam` / `SteamSockets` / `SocketSubsystemSteamIP` 已在
      `.uproject` 启用，但 C++ 侧无任何引用；`docs/tech-debt.md` 之外无接线计划。
- [ ] **学习工程独立化**：zxx 素材副本（`ECP_Archive/Content_zxx`）建议将来 push 成独立仓库
      或迁回各自学习工程；本仓库不再收学习内容。

## 已明确不做（本轮决策）

- [ ] **自动化测试：暂不引入（本轮决策）。** 曾写好 `ECP.AI.PerceptionTeamAssignment` /
      `ECP.AI.CharBaseDefaultsToNoTeam` 两个测试并验证通过（用于守住"AI 阵营必须实现在 Pawn 上"
      这条易错接线），按决策移除。将来要引入时的坑：UE 的 `-ExecCmds="Automation RunTests ..."`
      路径下**引擎即使测试失败也返回 0**，必须读 `Saved/Automation/index.json` 的 `failed`
      字段判定，否则等于没测。

## 已完成

### 2026-09-17 第三轮：_Game 分层重组（已推送 main）

- [x] `Characters/TestRobot/` 建为角色自包含目录；4 个动画序列从顶层 `Animations/` 迁入其
      `Animations/` 子目录
- [x] `Animations/ABP_TestRobot` 留在顶层共享层：ABP 跨角色复用、数量有限，不随单个角色退役
- [x] `AI/` 从 `Data/AI/` 提出（`AIC_ClownBase`、`ST_MonsterBase`）
- [x] `Blueprints/` 收拢玩法蓝图：`GM_InGame`、`PC_InGame` 从 `Data/` 迁入；
      `BP_ClownBase` 移入 `Blueprints/Monsters/`
- [x] `Inputs/InputActions/` 更名为 `Inputs/InputAction/`
- [x] 迁移在编辑器内完成，随后 Fix Up Redirectors + Save All；验收残留重定向器 0、
      `IMC_Player` 已指向新路径
- [x] Cook 端到端验证：`Success - 0 error(s), 1 warning(s)`（唯一 warning 是 EOS SDK 联网
      失败的 `libcurl error 35`，与资产无关）
- [x] 推送 main（`ab10b48..dbaf65e`，快进无冲突）

### 2026-09-17 第二轮：代码与资产正规化（分支 `chore/repo-hardening`）

**代码**

- [x] 新增项目级日志类别 `LogECP`（`Public/ECPCore.h`），业务代码 `LogTemp` 用法清零
- [x] 调试输出加 Shipping 保护：`DebugHelper` 迁至 `Private/Debug/`，加 `#if !UE_BUILD_SHIPPING`
      与 `GEngine` 判空；玩法代码里三处 `AddOnScreenDebugMessage` 全部走该封装
- [x] `FECPTaskAttack` 补 `ExitState`（原先进攻击状态把速度设 0 却不恢复，怪物可能永久冻结）
- [x] `FECPTaskChase` 检查 `MoveToActor` 失败（目标不可达时任务失败而不是永远 Running）
- [x] **AI 阵营实现到 `AECPCharBase`**：`GetTeamIdentifier` 只检查 Actor 自身、**不会回退到
      Controller**，实现在 Controller 上对感知完全无效；且 `ShouldSenseTeam` 要求双方都能解析
      阵营，否则一律 Neutral —— 阵营没实现在 Pawn 上时，勾了"只检测敌人"的怪物会**完全看不见
      玩家且不报任何错**。同时把感知收窄为只检测敌人（原先三个全开 + 回调里 Cast 过滤）
- [x] `TakeDamage` 返回实际生效伤害（原先返回请求值，过量击杀时会虚报）
- [x] 死亡表现抽 `ApplyDeathState()`，去掉"手工调 OnRep"的脆写法，胶囊体判空
- [x] 源码结构：`Data/` 拆为 `Framework/`（GameMode、PlayerController）与 `AI/`（AIController）；
      `ECPStateTreeCommon.h` 转 Private；删除零引用死代码 `ECPAnimInstBase`
- [x] `Build.cs` Public 依赖 9 → 5（`EnhancedInput`/`NavigationSystem`/`InputCore` 转 Private，
      删除无引用的 `GameplayTasks`）
- [x] **两个 `Target.cs` 钉住 `IncludeOrderVersion=Unreal5_8`**：UBT 默认值是 `Oldest`（=5.6），
      原项目一直用 5.6 的 include 顺序编译，而 5.6 顺序在 5.9 会被移除
- [x] **修正 `.clang-format`**：`BreakBeforeBraces`/`PointerAlignment` 两项与代码实际风格相反
      （配置 Attach/Right，代码是 Allman/Left = Epic 风格），原配置下跑一次 clang-format 会把
      27 个文件全部改写成另一套风格。已按实际风格修正并全量格式化（幂等验证通过）

**配置**

- [x] 删除 `Config/DefaultEditor.ini`（58KB，全部是引擎默认预览场景配置，单行 19KB 无法 review）
- [x] `DefaultInput.ini` 清掉 12 条 legacy `ActionMappings`/`AxisMappings`（项目已全量 Enhanced Input）
- [x] `DefaultEngine.ini` 清掉 19 行 `r.Mobile.*`（硬件目标是 Desktop/Maximum）
- [x] **禁用 `AndroidFileServer` 插件**：该插件 `EnabledByDefault=true`，编辑器每次保存都把
      带随机 `SecurityToken` 的配置段写回 `DefaultEngine.ini`，手工删段删不掉——只能禁用插件根治
- [x] `DefaultGame.ini` 清掉 CommonUI / GameFeatureData 惰性段，修正 AssetManager 的 Map 扫描路径
- [x] 恢复 `GameFeatureData` 的 AssetManager 条目：删掉它会让资产管理器加载时报错
      （`LoadErrors` / `LogGameFeatures`），该条目必需

**资产**

- [x] 沙盒 10 个角色资产（网格体/骨架/物理资产/2 材质/1 贴图/4 动画）迁入
      `_Game/Characters` 与 `_Game/Animations`
- [x] 3 个引用方（`BP_Player`、`BP_ClownBase`、`ABP_TestRobot`）共 7 处引用重指到 `_Game`
- [x] 删除孤儿 `ABP_ECPCommon`：全仓库零引用，却是**唯一**引用 `/Game/zxx` 的资产；
      删除后 `_Game` 侧 zxx 依赖彻底解除
- [x] 删除 `_Game/Maps/TestLevel` 重定向器（TestLevel→L_Test 改名留下的空壳）
- [x] **`Content/Developers` 出库**（`git rm -r --cached`）并删除本地副本（171 文件 / 50MB）；
      `Content/zxx` 移出磁盘（466MB / 652 文件）。`Content` 磁盘占用 518MB → 3.1MB、24 个资产。
      两份内容的完整副本均在 `E:\UnrealProjects\ECP_Archive`（删除前已逐一核对：本地文件
      全部有归档对应，归档为超集）；删除后重跑 Cook 仍为 `Success - 0 error(s), 0 warning(s)`。
      注：`Content/Developers/<个人名>/` 仍是约定中的个人沙盒位置，只是历史内容已清空。
- [x] `.gitignore` 收敛：取消为 `ABP_ECPCommon` 保留的骨架白名单例外

**验收记录**

- 资产注册表读真实导入表：全部 24 个 `_Game` 资产对 `/Game/Developers` 与 `/Game/zxx`
  的依赖为 **0**
- `BP_Player` / `BP_ClownBase` 的 `SkeletalMesh` 实测解析到
  `/Game/_Game/Characters/PolygonSyntyCharacter`
- Editor + Game 双目标编译通过；clang-format 幂等（`--dry-run` 零差异）

### 2026-09-17 第一轮：出库与规范先行

- [x] 学习资产 / 插件演示内容 / IDE 工具出库，651MB 完整归档 `E:\UnrealProjects\ECP_Archive`
- [x] `.gitignore` 白名单重写；`.gitattributes` + Git LFS（196 个二进制，`*.uasset`/`*.umap` 标 `lockable`）
- [x] 硬件目标 Desktop/Maximum；启动地图解绑个人目录；重复配置段清理；Build.cs 修正
- [x] `.clang-format` / `.editorconfig` 入库；`docs/` 六篇规范文档；CI 脚手架

## 已知副作用与善后（2026-09-17 第二轮操作遗留）

- [x] ~~**Git LFS 锁未释放**~~ —— 已全部释放（14 个）。`git lfs unlock` **没有 `--all` 参数**，
      需按路径逐个解锁；`git lfs locks` 退出码 0 且输出为空即为清空。
- [ ] **引擎安装目录被重存过**：为修重定向跑过一次 `-run=ResavePackages -FixupRedirectors`，
      该命令未限定范围，连 `D:\UE_5.8\Engine\Content` 也重存了部分包并删除了引擎内未引用的
      重定向器。影响面很小，但建议在 Epic Launcher 里对 UE 5.8 执行一次 **Verify** 恢复引擎文件。
- [ ] **本地 `.git/lfs` 膨胀约 110MB**：资产迁移过程中反复迭代，每次 `git add` 都为改动过的
      `.uasset` 生成新的 LFS 对象，本地对象数从 209 涨到 777（`.git/lfs` 53MB → 163MB）。
      完整内容在 HEAD 里是正确的，这只是本地垃圾。联网后执行：
      ```bash
      git lfs prune --dry-run --verbose   # 先看会删什么（当前预估保留 31 个、删除 746 个）
      git lfs prune                        # 确认后执行
      ```
      ⚠️ **离线时不要 prune**：若删掉远端没有、又不被本地 ref 引用的对象，内容无法找回。
      若决定做 LFS 历史瘦身（`git lfs migrate import --everything`），这一步会被一并覆盖。
- [ ] **`Config/DefaultEngine.ini` 是编辑器托管文件**：编辑器重写它会丢弃手写注释、
      并写回插件配置段。**"为什么这样配"的理由要写在 `docs/` 里，不要写在 ini 注释里。**
