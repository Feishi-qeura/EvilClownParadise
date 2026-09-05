# FUOnlineSession UE 5.8 迁移设计

## 目标

将 BrawlModel 的基于 OnlineSubsystem 的 LAN 会话逻辑迁移为 EvilClownParadise 的独立运行时插件。插件在 UE 5.8 中默认使用 OnlineSubsystemNull 提供局域网创建、搜索、加入和连接状态检测，同时保留切换至 Steam 或 EOS Provider 的扩展点。

## 范围

插件位于 `Plugins/FUOnlineSession`，包含一个 `FUOnlineSession` Runtime 模块。模块只依赖 `Core`、`CoreUObject`、`Engine`、`OnlineSubsystem` 和 `OnlineSubsystemUtils`，不直接依赖 Steam 或 EOS 模块。

插件公开以下 Blueprint 类型和能力：

- `UFU_OnlineSessionSubsystem`：创建、搜索、按密码加入和销毁会话的 `UGameInstanceSubsystem`。
- `FFU_SessionResult`：可传递给 Blueprint 的会话摘要。
- `EFU_JoinSessionResult`：加入结果枚举。
- `UFU_CheckSessionStatusAsync`：持续报告客户端 Ping 或连接超时的异步 Blueprint 节点。
- `FU_CreateCustomSession`、`FU_FindCustomSession`、`FU_JoinCustomSession`：统一带 `FU_` 前缀的 Blueprint 函数。

## 会话与 Provider 策略

会话操作通过 OnlineSubsystem v1 的 `IOnlineSession` 通用接口执行。默认项目配置为 `DefaultPlatformService=Null`，并以 LAN 查询和 LAN 会话参数工作。

插件不在构造函数中访问 World 或缓存 OnlineSubsystem。每个公开操作按当前 World 延迟解析会话接口，从而避免 PIE、多世界和子系统初始化时序问题。

会话配置保留 LAN 所需的 `bIsLANMatch` 和公开广告设置，并提供 `bUseLobbiesIfAvailable` 的 Provider 兼容设置。搜索使用 UE 5.8 的 `SEARCH_LOBBIES`，不使用已移除的 `SEARCH_PRESENCE`。Steam 或 EOS 接入时，只需在项目启用相应 Provider 插件并更新项目配置；核心插件无需修改或重新编译以引用 Provider 私有 API。

## 生命周期与错误处理

创建或加入前如已存在 `NAME_GameSession`，先销毁旧会话，并仅在销毁完成回调后继续后续操作。所有委托句柄在同步调用失败、异步完成及 `Deinitialize` 中清理。

无 OnlineSubsystem、无会话接口、无 World、无本地玩家或请求被 Provider 立即拒绝时，插件会广播明确失败结果。Ping 异步节点持有 WorldContext 的弱引用，在对象销毁、World 无效或调用取消时停止定时器。

`RoomPassword` 保留为会话发现阶段的筛选值，方便 LAN 原型流程，但它不是安全认证。生产环境的权限校验必须由游戏服务端或登录逻辑实现。

## 项目集成

在 `EvilClownParadise.uproject` 启用 `FUOnlineSession`。在项目 `Config/DefaultEngine.ini` 设置 Null Provider 作为默认 LAN 配置。更新根 `.gitignore` 放行 `Plugins/` 与本设计文档目录，使本次生成的插件和设计可提交。

## 验证

1. 使用 UE 5.8 构建 `EvilClownParadiseEditor`，确保不存在 OnlineSubsystem 弃用宏或链接错误。
2. 在编辑器中确认插件已加载，且 Blueprint 可以取得 `UFU_OnlineSessionSubsystem` 并调用所有 `FU_` 接口。
3. 以两个独立游戏实例验证：主机创建 LAN 会话，客户端搜索到会话、按密码加入并完成 ClientTravel。
4. 在客户端执行 `UFU_CheckSessionStatusAsync`，确认 Ping 回调、停止和失联回调在预期条件下发生。
5. 仅在启用对应 Provider 后执行 Steam/EOS 的联机验证；该验证不属于本次 LAN 默认配置的阻塞条件。
