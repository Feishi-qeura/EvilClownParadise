# Game Feature 模块化重构设计

## 已确认的目标

采用双层布局：Game Feature 的 C++、插件描述和 GameFeatureData 放在 `Plugins/GameFeatures/`；游戏蓝图和资源放在 `Content/Project/<功能>/`。保留 FUOnlineSession 已有的 Steam/Lobby 与 NULL/LAN 实现。C++ 管理生命周期、网络权威逻辑和公共契约；蓝图负责资源装配、参数和表现。所有提供给蓝图调用的项目函数必须有用途、参数和网络语义注释。

`Content/zxx/BMMCSystem/` 整体排除迁移，尤其不能改动 `Maps/SinglePlayer/SinglePlayerMap.umap` 的用户修改。

## 迁移前工程核查

- UE 5.8，目前只有 `EvilClownParadise` 一个游戏 Runtime 模块。
- FUOnlineSession 已独立为 Runtime/Editor 插件模块，Runtime 在 PreDefault 加载，使用 GameInstanceSubsystem，不能为了接入 Game Feature 重写 Provider 或重复执行 ClientTravel。
- 默认 GameInstance 仍为 Engine.GameInstance；GameFeatureData 扫描配置已经存在，但项目统一功能注册入口尚未实现。
- 玩家、拾取物、PlayerController 之间存在双向类型引用，不能简单移动文件到三个模块。
- AIController 直接识别 ECPPlayerBase，需要改为公共生命/阵营契约，才能解除对玩家拾取实现的依赖。
- 740 个已跟踪 Content 文件中，BMMCSystem 占 464 个，剩余 276 个需要按实际用途归档。旧文档声称 zxx 已移出，现状不符。
- 当前编辑器打开本工程，原有共享资产大多为只读。资源迁移须走编辑器资产工具，并检查真实保存结果。

## 模块边界

| 层级 | 模块 | 职责 |
| --- | --- | --- |
| 普通 Runtime 插件 | ECPFoundation | 公共角色生命/阵营契约、共享网络类型、日志、功能注册数据与状态接口 |
| 宿主 | EvilClownParadise | 项目启动、功能清单、兼容旧蓝图入口，不持有具体功能实现依赖 |
| Game Feature | ECPCharacter | 共享角色、生命/阵营、移动状态、动画与布娃娃 |
| Game Feature | ECPInteraction | 玩家操控、相机、拾取、检查旋转、装备、库存、投掷和服务器竞争裁决 |
| Game Feature | ECPAI | 怪物、感知、巡逻、追击、攻击及 StateTree |
| Game Feature | ECPOnline | FUOnlineSession 的项目接入、房间界面和本地网络调试表现 |
| Game Feature | ECPWorld | 物理推动器和正式测试场景装配 |

本次保持现有玩家、控制器和拾取物的网络协议，把互相依赖的实现整体归入 Interaction；Character 提供不依赖 Interaction 的公共角色基类。此边界避免在搬迁同时重写已工作的 RPC 和复制顺序。AI 通过公共阵营/生命契约识别目标。Feature 不反向依赖宿主游戏模块。在线后台保持普通插件常驻，Feature 只管理项目接入与表现。

## 统一入口与运行时要求

统一注册入口读取唯一功能清单，验证重复名称、缺失插件和依赖环，按依赖顺序请求引擎注册、加载、激活。蓝图可查询功能状态，并监听全部就绪或失败事件。重复初始化幂等，失败必须返回具体插件和原因。客户端和服务器各自执行本地生命周期；专服不创建 UI，不假设引擎子系统会复制状态。

不能在功能尚未就绪时启动依赖其类和资源的玩法。多 PIE 的插件状态属于引擎进程，不能因一个 GameInstance 退出就停用其他实例仍在使用的功能。玩法中有复制 Actor/组件时，不提供任意热卸载入口。

## 资源与兼容性

`Content/Project/` 下按 Bootstrap、Shared、Character、Interaction、AI、Online、World、Archive 分组；Archive 用于需要保留但不参与正式运行的已有备份。角色专属骨架、模型和动画成组迁移，避免拆断依赖。

使用编辑器资产工具进行重命名/移动和引用修复，不能用文件系统移动 uasset/umap。对受保护 BMMCSystem 的引用保留兼容重定向；未重存受保护引用方之前不得清理这些重定向。类迁移必须区分类、结构、枚举的反射身份，并逐项检查旧蓝图可加载。

GameFeatureData 与打包规则必须覆盖 Project 目录的外部资源，编辑器能加载不等于打包包含。正式资源引用需要覆盖配置和代码中的旧路径。

## 验收

1. Editor 与 Game 目标编译成功；公共蓝图函数注释完整。
2. 所有功能插件有实际实现和 GameFeatureData，统一入口能报告就绪与失败。
3. 加载迁移后的蓝图、StateTree 和场景，检查断链、父类丢失和编译错误。
4. 原有角色、布娃娃、拾取/库存/检查旋转回归测试通过。
5. 双人 PIE 验证服务器权威、远端表现与退出清理；NULL 和 Lobby 保持原会话行为。
6. Cook 验证 Project 与 Game Feature 资源齐全。
7. BMMCSystem 的文件集合和内容校验值与开始时一致，包括用户已修改的 SinglePlayerMap。

## 执行状态

设计已实施：五个 Game Feature 与 Foundation 已集成，276 项资源已迁移并保留旧路径兼容；Editor/Game 编译、正式蓝图、自动化回归、双端 PIE 和 Windows Cook 已验证。Character 限定为共享角色层，玩家操控与拾取协议整体归 Interaction。BMMCSystem 464 个文件指纹保持不变。当前验收范围、历史归档问题和资源锁释放状态见 ../../game-features-progress.md。
