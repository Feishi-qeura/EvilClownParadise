# Game Feature 模块化实施计划

> **For agentic workers:** Use superpowers:executing-plans to implement and verify each deliverable. Independent file groups may be delegated; do not overwrite another worker's files.

**Goal:** 将现有功能迁入真实 Game Feature 插件，统一注册启动，资源归入 Content/Project，保持 BMMCSystem 与现有多人协议兼容。

**Architecture:** Foundation 提供公共声明和引擎级注册策略；Character 提供共享角色层；Interaction 和 AI 依赖 Character；World 依赖公共网络策略；Online 包装 FUOnlineSession。宿主只依赖 Foundation。

**Tech Stack:** UE 5.8、C++、Blueprint、GameFeatures、ModularGameplay、FUOnlineSession、Git LFS。

**Spec:** ../specs/2026-09-23-game-features-design.md

## Global Constraints

- 用户已确认设计并要求继续实施。
- BMMCSystem 所有 464 个文件保持原位和内容不变，保留 SinglePlayerMap 已有修改。
- 对受保护资源保留精确重定向，不能全局重存或清理。
- 蓝图函数逐一注释用途、参数、网络语义。保持 Steam/Lobby、NULL/LAN 后端实现。
- 不重写拾取复制协议；PlayerBase、Controller、PickupItem 整体归 Interaction，Character 只提供共享角色/移动状态/动画/布娃娃。此精确边界已在会话说明。

## Review Focus

1. 旧 Class/Struct/Enum 路径在类迁移后仍可解析。
2. 注册必须等待真实完成，错误含插件名和原因。
3. 多 PIE 共享状态，单个实例退出不能停用其他实例使用的 Feature。
4. 资产迁移不能隐式保存 BMMCSystem 的反向引用方。
5. Project 外部内容及间接 BMMC 依赖必须被 Cook。

## 1. 注册入口

- [x] 新建 `Plugins/ECPFoundation/`，迁入 ECPCore 日志/阵营声明，提供共享网络采样策略。
- [x] 实现 UECPGameFeatureSettings 唯一清单及 UECPGameFeaturePolicy 引擎级注册入口。
- [x] 校验重复名、缺失插件、未知依赖和依赖环，按依赖顺序注册激活。
- [x] 提供有注释的蓝图状态查询、就绪和失败通知，不开放玩法期间热卸载。
- [x] 验证启动前必需功能就绪，避免异步初始化晚于蓝图父类解析。

## 2. C++ 迁移

- [x] ECPCharacter：ECPCharBase、ECPCharBaseRagdoll、ECPRagdollTypes、ECPRagdollAnimInstance 与相关测试。
- [x] ECPInteraction：ECPPlayerBase、ECPPickupItem、ECPPickupTypes、ECPPlayerController、ECPGameMode 与相关测试。
- [x] ECPAI：ECPMonsterBase、ECPAIController、Task/Evaluator；移除 PlayerBase Cast，改为公共生命/阵营判断。
- [x] ECPWorld：ECPPhysicsPusher，常量经 Foundation 共享。
- [x] 更新导出宏/Build.cs，精确 Class/Struct/Enum CoreRedirects，逐个补齐蓝图函数注释。

## 3. 在线项目层

- [x] ECPOnline 只依赖 Foundation 与 FU 公共接口；Feature 就绪和 LocalPlayer 就绪分别检查。
- [x] 提供蓝图取得当前 GameInstance FU 子系统的入口，保持 Join 内部 ClientTravel，不重复旅行。
- [x] 项目 UI 归 Online；角色硬编码资源改为项目装配配置。保持后台常驻。

## 4. Project 资源

- [x] 恢复编辑器连接，核对未保存包、LFS 锁和 Asset Registry 引用图。
- [x] 为 BMMCSystem 建立文件集合及内容指纹，生成逐资产源/目标迁移清单。
- [x] 用编辑器资产工具迁移 276 个非模板文件，保留旧路径重定向器、备份和同名独立资产。
- [x] 为五个插件建立真实 GameFeatureData，配置 Project 内容扫描与打包。
- [x] 更新默认地图、C++/测试路径及协作文档，验证迁移后的蓝图和 StateTree 可加载。

## 5. 验收

- [x] Editor/Game 编译与模块依赖校验。
- [x] 角色、动画、拾取、复制回归及 FU 既有测试。
- [x] 双人 PIE、NULL 联机、可用条件下的 Lobby 验证；双机条件不足时如实记录。
- [x] Cook 验证；BMMCSystem 指纹核对；最终 Git diff 检查。

上述验收依据及范围见 ../../game-features-progress.md：项目 27 个独立用例和 FU 36 个用例通过，Windows Cook 成功；真实 FU 创建/搜索/加入及双机 Lobby 条件不足，未声明通过。Archive 仅验证历史保存及重定向元数据，不声明旧图可编译。

- [ ] 本次 276 个 Git LFS 资源锁释放并核对远端结果（107 项已确认释放，169 项因网络超时待核对，见 ../../game-feature-lfs-locks.json）。
