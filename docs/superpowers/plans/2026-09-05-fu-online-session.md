# FUOnlineSession Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在 EvilClownParadise 中交付可在 UE 5.8 编译、默认支持 LAN、可扩展到 Steam/EOS 的 `FUOnlineSession` Runtime 插件。

**Architecture:** 插件经 OnlineSubsystem v1 的 `IOnlineSession` 管理会话而不依赖 Provider 私有模块。`UFU_OnlineSessionSubsystem` 按当前 World 延迟解析接口并管理委托；`UFU_CheckSessionStatusAsync` 独立管理 Ping 定时器与对象生命周期。

**Tech Stack:** Unreal Engine 5.8、C++、OnlineSubsystem、OnlineSubsystemUtils、Unreal Automation Framework。

**Spec:** `docs/superpowers/specs/2026-09-05-fu-online-session-design.md`

## Global Constraints

- 使用 UE 5.8，并用 `SEARCH_LOBBIES` 替换 `SEARCH_PRESENCE`。
- 公开类型使用 `FU`，Blueprint 函数使用 `FU_` 前缀。
- 仅依赖 `Core`、`CoreUObject`、`Engine`、`OnlineSubsystem`、`OnlineSubsystemUtils`。
- 默认 Provider 为 `Null`，默认会话及搜索均为 LAN；Steam/EOS 仅通过项目配置启用。
- 同步失败、异步完成和 `Deinitialize` 都必须清理 OnlineSubsystem 委托。
- 新增复杂逻辑、OnlineSubsystem 调用、空值保护和客户端/服务器分支都必须有意图注释。

---

## File Structure

| Path | Responsibility |
| --- | --- |
| `Plugins/FUOnlineSession/FUOnlineSession.uplugin` | Runtime 插件声明。 |
| `Plugins/FUOnlineSession/Source/FUOnlineSession/FUOnlineSession.Build.cs` | 引擎和 OnlineSubsystem 依赖。 |
| `Plugins/FUOnlineSession/Source/FUOnlineSession/Public/FUOnlineSessionModule.h` | 模块接口。 |
| `Plugins/FUOnlineSession/Source/FUOnlineSession/Private/FUOnlineSessionModule.cpp` | 模块实现。 |
| `Plugins/FUOnlineSession/Source/FUOnlineSession/Public/FU_OnlineSessionTypes.h` | Blueprint 结果、枚举与委托。 |
| `Plugins/FUOnlineSession/Source/FUOnlineSession/Public/FU_OnlineSessionSubsystem.h` | 会话 Blueprint API。 |
| `Plugins/FUOnlineSession/Source/FUOnlineSession/Private/FU_OnlineSessionSubsystem.cpp` | 会话状态机及回调。 |
| `Plugins/FUOnlineSession/Source/FUOnlineSession/Public/FU_CheckSessionStatusAsync.h` | Ping 异步节点。 |
| `Plugins/FUOnlineSession/Source/FUOnlineSession/Private/FU_CheckSessionStatusAsync.cpp` | Ping 定时器实现。 |
| `Plugins/FUOnlineSession/Source/FUOnlineSession/Private/Tests/FUOnlineSessionAutomationTests.cpp` | 自动化测试。 |
| `Plugins/FUOnlineSession/README.md` | LAN、Steam、EOS 配置说明。 |

### Task 1: 注册 Runtime 插件与默认 LAN 配置

**Files:** Create `Plugins/FUOnlineSession/FUOnlineSession.uplugin`, `Source/FUOnlineSession/FUOnlineSession.Build.cs`, `Public/FUOnlineSessionModule.h`, `Private/FUOnlineSessionModule.cpp`; modify `EvilClownParadise.uproject` and `Config/DefaultEngine.ini`.

**Produces:** 可加载的 `FUOnlineSession` Runtime 模块。

- [ ] **Step 1: 建立构建基线。**运行 `F:\UNREAL\UE_5.8\Engine\Build\BatchFiles\Build.bat EvilClownParadiseEditor Win64 Development -Project="D:\demo\EvilClownParadise\EvilClownParadise.uproject" -WaitMutex -FromMsBuild`；预期退出码为 0。
- [ ] **Step 2: 新建插件描述和模块。**描述符使用一个 `Runtime` 模块 `FUOnlineSession`、`CanContainContent: false`、`FriendlyName: "FU Online Session"`、`LoadingPhase: "Default"`。Build.cs 的 PublicDependencyModuleNames 精确为 `Core`、`CoreUObject`、`Engine`、`OnlineSubsystem`、`OnlineSubsystemUtils`。模块实现 `FFUOnlineSessionModule final : public IModuleInterface` 与 `IMPLEMENT_MODULE(FFUOnlineSessionModule, FUOnlineSession)`。
- [ ] **Step 3: 接入项目。**在既有 Plugins 数组增加 `{ "Name": "FUOnlineSession", "Enabled": true }`，不删除 `ModelingToolsEditorMode`。在 DefaultEngine.ini 仅添加一次 `[OnlineSubsystem]` / `DefaultPlatformService=Null`。
- [ ] **Step 4: 重新构建。**运行 Step 1 命令；预期退出码为 0，日志出现 FUOnlineSession 模块且没有描述符、模块依赖或重复配置错误。
- [ ] **Step 5: 提交。**执行 `git add Plugins/FUOnlineSession EvilClownParadise.uproject Config/DefaultEngine.ini`，随后执行 `git commit -m "feat: add FU online session plugin skeleton"`。

### Task 2: 实现公开类型并验证默认值

**Files:** Create `Public/FU_OnlineSessionTypes.h`, `Private/Tests/FUOnlineSessionAutomationTests.cpp`; modify plugin Build.cs。

**Produces:** `EFU_JoinSessionResult`、`FFU_SessionResult`、`FFU_OnCreateSessionComplete`、`FFU_OnFindSessionComplete`、`FFU_OnJoinSessionComplete`。

- [ ] **Step 1: 先写失败测试。**注册 `FUOnlineSession.Types.DefaultResult`，在类型实现前编译以下断言：`FFU_SessionResult Result; TestEqual(TEXT("Default session id"), Result.SessionId, FString()); TestEqual(TEXT("Default max players"), Result.MaxPlayers, 0); TestEqual(TEXT("Default current players"), Result.CurrentPlayers, 0); TestEqual(TEXT("Default ping"), Result.PingInMs, 0);`；预期编译报缺少 `FFU_SessionResult`。
- [ ] **Step 2: 实现类型。**枚举值精确为 `Success`、`SessionIsFull`、`SessionDoesNotExist`、`CouldNotRetrieveAddress`、`AlreadyInSession`、`UnknownError`。结构体公开 `SessionId`、`RoomName`、`MaxPlayers`、`CurrentPlayers`、`PingInMs`；原始 `FOnlineSessionSearchResult` 只保留在私有状态机。委托签名分别是 `(bool bWasSuccessful)`、`(const TArray<FFU_SessionResult>& Results, bool bWasSuccessful)`、`(EFU_JoinSessionResult Result)`。
- [ ] **Step 3: 构建并执行测试。**先构建，再运行 `& 'F:\UNREAL\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe' 'D:\demo\EvilClownParadise\EvilClownParadise.uproject' -ExecCmds='Automation RunTests FUOnlineSession.Types.DefaultResult; Quit' -unattended -nop4 -nosplash`；预期命名测试通过。
- [ ] **Step 4: 提交。**执行 `git add Plugins/FUOnlineSession/Source/FUOnlineSession`，随后执行 `git commit -m "feat: add FU session blueprint types"`。

### Task 3: 实现 UE 5.8 兼容会话状态机

**Files:** Create `Public/FU_OnlineSessionSubsystem.h`, `Private/FU_OnlineSessionSubsystem.cpp`; modify `Private/Tests/FUOnlineSessionAutomationTests.cpp`。

**Consumes:** Task 2 的公开类型。 **Produces:** `UFU_OnlineSessionSubsystem`。

- [ ] **Step 1: 先写失败验证测试。**为 `FFU_SessionRequestValidation::CanCreate(int32, const FString&)` 添加 `FUOnlineSession.Session.Validation`：零玩家与空房间返回 false，`(2, TEXT("Room"))` 返回 true；先编译，预期报缺少验证器。
- [ ] **Step 2: 声明精确 Blueprint 接口。**实现 `FU_CreateCustomSession(int32 MaxPlayers, const FString& RoomName, const FString& RoomPassword, bool bUseLobbiesIfAvailable = false)`、`FU_FindCustomSession(const FString& RoomName, int32 MaxResults = 20, bool bUseLobbiesIfAvailable = false)`、`FU_JoinCustomSession(const FString& RoomPasswordInput)`、`FU_DestroySession()`；它们均为 `Category = "FU|Online Session"` 的 BlueprintCallable。子系统持有 IOnlineSession、搜索对象、四个委托句柄、待执行请求和私有原始搜索结果缓存。
- [ ] **Step 3: 实现操作与清理。**每次操作中用 `Online::GetSubsystem(GetWorld())` 延迟解析接口；验证 World、首个本地玩家、会话接口、房间名、玩家数和同步请求的 bool 返回值。创建设置 `NumPublicConnections`、`bIsLANMatch = true`、`bShouldAdvertise = true`、`bUsesPresence = true`、`bUseLobbiesIfAvailable`、`RoomName`、`RoomPassword`。搜索设置 `bIsLanQuery = true` 和 `SEARCH_LOBBIES`，绝不使用 `SEARCH_PRESENCE`。创建或加入前销毁 `NAME_GameSession`，只在 Destroy 回调继续；所有拒绝、完成和 Deinitialize 路径清理委托并取消未完成搜索；仅在解析连接字符串成功后 ClientTravel。
- [ ] **Step 4: 构建并验证。**构建后以 UnrealEditor-Cmd 执行 `Automation RunTests FUOnlineSession.Session.Validation`；预期三条断言通过，新增插件代码无 `SEARCH_PRESENCE` 或弃用警告。
- [ ] **Step 5: 提交。**执行 `git add Plugins/FUOnlineSession/Source/FUOnlineSession`，随后执行 `git commit -m "feat: add UE 5.8 compatible session subsystem"`。

### Task 4: 实现 Ping/断线异步 Blueprint 节点

**Files:** Create `Public/FU_CheckSessionStatusAsync.h`, `Private/FU_CheckSessionStatusAsync.cpp`; modify `Private/Tests/FUOnlineSessionAutomationTests.cpp`。

**Consumes:** Task 3 生命周期。 **Produces:** `UFU_CheckSessionStatusAsync::FU_CheckSessionStatus` 及 `OnServer`、`OnClient`、`ClientConnectionOvertime`。

- [ ] **Step 1: 先写失败测试。**添加 `FUOnlineSession.Ping.NullWorld`，断言 `UFU_CheckSessionStatusAsync::FU_CheckSessionStatus(nullptr, 1.0f)` 为 null；先编译，预期报缺少类。
- [ ] **Step 2: 实现节点。**工厂接受 WorldContext 和 RefreshTime，注册 GameInstance 并保存 `TWeakObjectPtr<UWorld>`；拒绝 null 或非正间隔。Activate 中服务端仅广播一次 `OnServer(0.0f)`；客户端由 World Timer 读取 `APlayerState::GetPingInMilliseconds()`。World、控制器或 PlayerState 失效时清除 Timer，广播 `ClientConnectionOvertime(0.0f)`；BeginDestroy 在 Super 之前清理 Timer。
- [ ] **Step 3: 构建并执行测试。**构建后运行 `Automation RunTests FUOnlineSession.Ping.NullWorld`；预期退出码为 0 且命名测试通过。
- [ ] **Step 4: 提交。**执行 `git add Plugins/FUOnlineSession/Source/FUOnlineSession`，随后执行 `git commit -m "feat: add FU session status async node"`。

### Task 5: 文档化 Provider 切换并作集成验证

**Files:** Create `Plugins/FUOnlineSession/README.md`; modify `EvilClownParadise.uproject`, `Config/DefaultEngine.ini` if required.

**Consumes:** Tasks 1-4。 **Produces:** LAN、Steam、EOS 配置说明和构建/联机证据。

- [ ] **Step 1: 编写说明。**记录 Null LAN 默认配置、Steam/EOS Provider 的启用及项目配置替换步骤，并明确 RoomPassword 只能筛选可发现会话、不能充当服务端安全认证。
- [ ] **Step 2: 最终构建。**运行 Task 1 命令；预期退出码为 0、插件已编译加载、无 SEARCH_PRESENCE。
- [ ] **Step 3: 双实例 LAN 验证。**主机调用 `FU_CreateCustomSession(2, "SmokeRoom", "SmokePass", false)`；客户端调用 `FU_FindCustomSession("SmokeRoom", 20, false)`，得到非空结果后调用 `FU_JoinCustomSession("SmokePass")`。预期客户端发现一个结果、加入结果为 Success、ClientTravel 到主机，且 `FU_CheckSessionStatus` 发出 OnClient Ping 回调。
- [ ] **Step 4: 提交并交付证据。**执行 `git add Plugins/FUOnlineSession/README.md EvilClownParadise.uproject Config/DefaultEngine.ini`，随后执行 `git commit -m "docs: add FU online session setup guide"`；最终交付中报告构建、自动化测试和 LAN smoke test 结果。
