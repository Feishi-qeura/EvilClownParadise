# Unified Pickup, Load and Ragdoll Carry Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [x]`) syntax for tracking.

**Goal:** 修复手持穿模，共享大小物品实现，增加 200 kg 可扩展负重及服务器权威队友搬运。

**Architecture:** Foundation 提供原生可搬运契约；Character 提供负重运动和布娃娃目标；Interaction 提供物品质量、共享 Physics Handle 组件、输入与库存计重。保留旧中型类和已有物品复制状态，重用完整布娃娃姿态复制。

**Tech Stack:** UE 5.8 / C++ / GameFeatures / CharacterMovement / Chaos / Unreal Automation。

**Spec:** `docs/superpowers/specs/2026-10-09-unified-pickup-load-ragdoll-carry-design.md`

## Global Constraints

- MaxCarryWeightKg 默认 200 kg；BodyWeightKg 默认 80 kg；速度与跳跃初速度倍率为 Clamp(1-W/C,0,1)。
- 服务器权威，不信任客户端目标、重量、距离或卸重下标；没有 Character -> Interaction 依赖。
- 保留进入布娃娃掉出物品、旧类/蓝图兼容、当前工作区其他改动及 BMMC。
- 超重仍可卸重，空中正常下落；队友不能装备、入库、投掷或在被搬运时起身。
- 用户已要求开始实施：采用当前会话直接执行，不再设置中途确认关卡。
- 当前 GameFeature 源码包含未入库内容，另建 HEAD 工作树无法包含真实基线，因此原位实施；Git 缺少身份，保留文件供审核，不伪造身份提交。

## Review Focus

- 首次拾取、旋转和滚轮不能把大物体移过墙：Task 3 的真实物理世界阻挡测试。
- 跑步和起身不得把负重速度恢复为满速：Task 2 的负重切换/锁恢复测试。
- 物品销毁和失败释放不得残留或误扣负重：Task 4 的库存销毁/安全卸重测试。
- 拾取与起身/销毁竞争不能留下双方占用：Task 5 的队友占用/清理测试。
- 未映射引用及旧复制回调不得启动第二套物理：Task 3/5 的状态复制契约及现有时序回归。

## Task 1: 回归基线与缺陷测试

**Files:** 新增 `ECPInteraction/Private/Tests/ECPCarryLoadTests.cpp`（路径均位于对应 GameFeature 的 Source 模块）；日志保存在 `Saved/CarryLoadValidation/`。

**Interfaces:** 测试先使用当前接口/反射观察缺少的属性，避免未声明 API 导致编译错误。

- [x] 新增手持根仍模拟且有碰撞、角色负重属性可用、布娃娃可搬运契约存在的缺陷测试。
- [x] 编译 Editor；执行 ECP.Pickup 与新测试，记录基线与预期失败。

## Task 2: 公共契约和角色负重

**Files:** Foundation `Public/Interaction/ECPCarryTarget.h`；Character `ECPCharBase.h/.cpp`、`ECPCharBaseRagdoll.cpp`、新增 `ECPCharBaseLoad.cpp` 和负重测试。

**Interfaces:** `IECPCarryTarget`: GetCarryBody(), GetCarryBone(), GetCarryMassKg(), GetCarryHolder(), CanBeCarriedBy(const APawn*), TryBeginCarry(APawn*), EndCarry(APawn*). Character: GetCarriedWeightKg(), SetCarriedWeightKg(float), SetMaxCarryWeightKg(float), GetLoadMultiplier(), IsOverloaded(), RefreshLoadMovement().

- [x] 测试 0/100/150/199/200/250 kg、容量扩展、非有限值、跑步及锁恢复。
- [x] 实现复制的负重属性与唯一运动参数刷新；Jump/CanJump 拒绝超重，保持重力。
- [x] 实现身体重量与布娃娃原生契约；占用时拒绝恢复并清理无效搬运者。
- [x] 编译并运行角色负重与时序测试。

## Task 3: 统一物品与物理抓取

**Files:** Interaction `ECPPickupItem.h/.cpp`、`ECPMediumPickupItem.h/.cpp`；新增 `Public/Components/ECPPhysicalCarryComponent.h` 与 Private 实现；更新已有 medium/presentation/placement 测试。

**Interfaces:** 公共物品 GetPickupMassKg(), RefreshPickupMass(), GetPickupResponseSeconds(), GetInspectionTargetWorldTransform(); 公共组件 StartCarry(AActor*), StopCarry(), GetCarryTarget()。

- [x] 扩展回归：手持碰撞、初始位置不瞬移、实际物理约束、阻挡与旋转目标、不再使用客户端伪造物理。
- [x] 质量/能力/跟随配置移入基类，旧中型类仅适配默认配置与旧属性。
- [x] 实现服务器 Physics Handle、安全目标与持有者接触过滤；保持版本、预测目标及释放协议。
- [x] 将旧“直接改 Transform 即到目标”的测试改为物理 Tick 后的真实行为断言。
- [x] 编译并运行 ECP.Pickup，记录未执行的真实网络视觉边界。

## Task 4: 玩家总重量与安全卸重

**Files:** Interaction `ECPPlayerBase.h/.cpp`、新增 `ECPPlayerBaseCarry.cpp`、`ECPPlayerController.cpp`、拾取状态 UI 与测试。

**Interfaces:** RecalculateCarriedWeight(), NotifyOwnedPickupChanged(AECPPickupItem*), GetCarriedTeammate(), ReleaseCarriedTeammate()；物品变更通知触发计重。

- [x] 测试检查/装备/库存重量守恒、对象去重、销毁清理、G 空手库存卸重。
- [x] 状态变更和质量变更触发去重重算，清理不属于当前角色的条目。
- [x] G 按目标上下文卸重；库存放置先找安全空间，失败不修改条目。
- [x] 输入拒绝超重主动移动与起跳；现有检查输入保持当前行为并优先满足重量规则。
- [x] 编译运行 ECP.Pickup.Load 回归。

## Task 5: 布娃娃队友仲裁与生命周期

**Files:** Interaction `ECPPlayerBaseCarry.cpp`；Character 布娃娃占用接口与测试；既有输入/扫描扩展。

**Interfaces:** 队友状态由 Character 的原生契约管理，Interaction 队列按现有 ECPPickupPriority 复核服务器视线并取得唯一占用。

- [x] 测试同阵营、距离遮挡、正常阶段拒绝、唯一占用、被搬运不能恢复、释放减重和销毁清理。
- [x] F 服务器扫描支持物品及骨骼布娃娃，同帧请求仲裁使用当前优先级规则。
- [x] 搬运队友共用抓取组件与现有姿态复制；右键/G 安全释放，2/V 拒绝。
- [x] 死亡、失控、布娃娃进入、目标消失和 EndPlay 幂等清理双方引用及重量。
- [x] 编译并运行相关自动化测试。

## Task 6: 整体验证与审查

**Files:** 验证记录与本计划进度；玩法文件仅修复审查发现的问题。

- [x] 编译 Editor 与 Game；运行整个 ECP 自动化测试集，记录全部失败名称与退出结果。
- [x] 按 executing-plans 要求派发一次独立只读最终代码审查；修复实质问题并增加失败到通过的回归。
- [x] 记录多人/专服/延迟测试真实执行情况，提供可操作验证步骤与角色参数位置。
- [x] 完成差异检查并交付；Git 未配置身份时不提交，不包含其他人的已有暂存资产。
