# FU Online Session 2.1.1

`FUOnlineSession` 是面向 Unreal Engine 5.8、Win64 的运行时联机插件。它用同一套模板化实现分别特化 Steam Lobby 与 OnlineSubsystemNULL/LAN，并把创建、搜索、加入、销毁、环境预检和诊断能力公开给 Blueprint。

## 启用与依赖

只需要在项目的 Plugins 面板启用 `FU Online Session` 并重启编辑器。插件描述符会同时声明并启用：

- `OnlineSubsystem`
- `OnlineSubsystemUtils`
- `OnlineSubsystemNull`
- `OnlineSubsystemSteam`
- `SteamSockets`

不要复制或修改 UE 安装目录中的 Steamworks DLL。插件依赖引擎随 UE 5.8 提供的 Steamworks SDK，并只支持 Win64 x64。

## 配置所有权

插件的 `Config/Engine.ini` 作为插件配置层自动参与引擎配置合并：默认平台服务保持 `Null`，Steam 子系统同时可由插件按名称取得；Steam 会话传输使用 `SteamSocketsNetDriver`，LAN 会话使用 `IpNetDriver`。

插件不会为当前配置改写项目的 `Config/DefaultEngine.ini`。启用插件后配置层生效；禁用插件并重启进程后，该层自然退出配置层级，新安装项目的原文件始终不变。

`bAutoConfigureProject` 现在只控制一次性兼容迁移：默认会删除旧版本曾写入、且带有 FU 成对标记的历史受管块，并在 `Saved/FUOnlineSession/ConfigBackups` 保留原始备份；关闭它后迁移器不会读取或改写项目文件。该历史清理不是“禁用插件时回滚”，因为当前版本已不再生成这种项目块。

UE 5.8 的 P2P 兼容参数位于 `[SocketSubsystemSteamIP]`，而不是旧的 `[OnlineSubsystemSteam]`：

```ini
[OnlineSubsystem]
DefaultPlatformService=Null

[OnlineSubsystemSteam]
bEnabled=true
bUseSteamNetworking=false

[SocketSubsystemSteamIP]
bAllowP2PPacketRelay=true
P2PConnectionTimeout=90.0
P2PCleanupTimeout=1.5
```

`SocketSubsystemSteamIP` 是旧兼容层而非本插件必需依赖；实际游戏连接由 `SteamSockets` 完成。若项目自行启用了旧兼容插件，上述配置只负责消除它的迁移警告，禁用 FU 不会替项目改动 `.uproject` 中独立维护的插件开关。

开发配置保存在 `Config/DefaultFUOnlineSession.ini`，也可以在 `Project Settings -> Plugins -> FU Online Session` 查看。非 Shipping 构建会在 Steam 子系统创建前，将 `SteamDevAppId` 注入当前进程内的动态 Engine 配置层；它不写项目 ini，也不修改 UE 安装目录，并在模块退出时只撤销自己拥有的动态层。

`480` 是 Valve Spacewar 公共测试 AppID，只适合开发联调。Shipping 构建不会注入开发 AppID：发布前必须设置 `ExpectedShippingSteamAppId`，并由正式 Steam 启动/部署环境提供实际 AppID。

## Blueprint 快速接入

从 Game Instance 取得 `FU Online Session Subsystem`，先绑定完成事件，再调用对应 Provider 的入口。

Steam：

- `CheckSteamProviderStatus`
- `CreateSteamSession(MaxPlayers, RoomName, RoomPassword)`
- `FindSteamSessions(RoomName, MaxResults)`
- `JoinSteamSession(SessionId, RoomPasswordInput)`
- `DestroySteamSession`

LAN/NULL：

- `CheckLanProviderStatus`
- `CreateLanSession(MaxPlayers, RoomName, RoomPassword)`
- `FindLanSessions(RoomName, MaxResults)`
- `JoinLanSession(SessionId, RoomPasswordInput)`
- `DestroyLanSession`

共同完成事件：

- `OnCreateSessionCompleteV2(Provider, bWasSuccessful)`
- `OnFindSessionCompleteV2(Provider, Results, bWasSuccessful)`
- `OnJoinSessionCompleteV2(Provider, Result)`
- `OnDestroySessionComplete(Provider, bWasSuccessful)`
- `OnOnlineConnectionFailure(Provider, FailureType, Message)`

搜索结果自带 `Provider` 和 `SessionId`。加入时必须把用户选择结果的 `SessionId` 交给相同 Provider 的 Join 节点，不能把 Steam 结果交给 LAN，反之亦然。

## 加入前查询房间 Ping

取得 `FU Online Session Subsystem`，在 `OnFindSessionCompleteV2` 成功后，将目标结果的 `Provider` 和 `SessionId` 传入纯函数 `Get Session Ping`：

- `PingMs`：最近一次搜索缓存里的延迟，单位毫秒；可直接拼接 `ms` 显示。
- 找不到房间、空 ID、非法 Provider、无效原生结果、负 Ping 或 UE 的 `9999` 等不可用值，统一返回 **999**。
- 有效 Ping 原样返回，保留有效的 `0ms` 和 `1200ms` 等数值；不会把真实高延迟截断成 999。
- `Is Estimated`：有效 Steam 结果为 true，表示 SteamSockets 中继线路估算；LAN 和兜底值为 false。
- 本函数只查缓存，不加入房间、不发探测包、不创建 timer。重复调用不会重新测速；再次搜索才更新数据。查询不到并不单独返回“未知”或“未找到”状态。

`Provider` 必须与搜索结果一致；两个 Provider 的缓存相互独立。开始下一次搜索会清空该 Provider 的旧缓存，因此请在新一轮搜索成功回调后更新房间列表。返回值用于显示搜索时的延迟，不保证房间此刻仍在线。

专项测试入口：`Automation RunTests FUOnlineSession.SessionPing`。

## 持续 Ping 监测

在已进入联网地图的客户端调用一次 `FU_CheckSessionStatus(WorldContextObject, RefreshTime)`，例如 `RefreshTime=1.0` 表示每秒更新。无需在 Tick 中重复创建节点；每次调用都会创建一个独立监测器。

- 单机、Listen Server 房主、Dedicated Server：仅触发一次 `On Server(0)`。
- 远端客户端：就绪后立即触发 `On Client(PingValue)`，随后按刷新间隔持续触发；Ping 单位为毫秒。
- 初始化时 PC、PlayerState 或连接尚未就绪：允许等待，使用当前 NetDriver 的 `InitialConnectTimeout`。
- 首次成功采样后，状态短暂缺失：使用 `ConnectionTimeout` 宽限，恢复后继续更新。就绪等待没有有效配置时使用 30 秒。
- 持续未收包：比较 NetDriver 时钟与连接的 `LastReceiveTime`，使用连接的 `GetTimeoutValue()` 判断；遵循开发用的 `bNoTimeouts`。
- 本 World 的游戏连接明确失败或等待超时：仅触发一次 `Client Connection Overtime(0)` 并结束监测。单次高 Ping 不直接等同断线。
- 正常切图或结束 PIE：静默清理 timer、网络事件监听和 GameInstance 保活；需要在新 World 再次调用。菜单中的单机节点不会自动转换为加入房间后的客户端节点。

节点在游戏线程定时读取 UE 已有 Ping 数据，不额外发送探测包。每帧最多执行一次定时采样，避免长帧补发；它仍受 World 定时器的暂停、时间缩放和游戏线程调度影响，不提供独立线程的实时心跳保证。`RefreshTime` 必须是有限正数，只决定采样频率；采样型超时在下一次 timer 回调检测，明确的网络失败事件会直接结束监测。

自动化测试入口：`Automation RunTests FUOnlineSession.Ping`。真实网络验收还应在 LAN/Steam 两端检查客户端连续更新、短时丢包恢复、持续断网仅一次超时、切图后重新启动及房主一次输出。

## Steam 搜索语义

`bWasSuccessful=true` 只表示 Steam 已正常完成查询，不表示一定存在房间。插件按以下顺序搜索：

1. 用完整、版本化的项目关键字精确查询。
2. 第一遍成功但原始结果为零时，自动执行且只执行一次降级查询；指定房间名时第二遍只用独立房间名键，无名浏览时改用数值项目协议标识。
3. 所有结果在进入缓存和 Blueprint 前，再用完整项目关键字、房间元数据与可选房间名精确校验。

这能绕开单一自定义查询键异常，同时避免公共 AppID 480 的其他项目占用结果列表。若第二遍仍为零，诊断会产生 `FU.FindSessions.Fallback.Empty`，并提示检查房主在线/空位、Steam 区域以及索引可见性。

UE 5.8 的公开 OnlineSubsystemSteam 搜索 API 没有暴露 Lobby 距离选项，引擎默认只查询相同或附近区域。两台测试机跨区域时，应先将 Steam 下载区域调整到相同或相近区域再复测；插件不会修改引擎源码来绕过该限制。

## 诊断系统

诊断从同一脱敏事件同时输出到：

- `LogFUOnlineSession` / UE 日志；
- 按 Provider 自动追加的 UTF-8 日志文件；
- 游戏内 Slate 浮层；
- Blueprint 的 `OnOnlineDiagnosticEvent`；
- `GetDiagnosticHistory`、`BuildDiagnosticReport` 与 `SaveDiagnosticReport`。

屏幕浮层默认只显示 Warning 及以上，三秒后自动消失。整个浮层及其子控件均为鼠标命中透明，因此不会夺走底层 UMG 的点击或 hover；Blueprint 事件和历史记录不受浮层显示时长影响。

`RunProviderDiagnostics(Provider)` 只读取环境并生成一次快照，不提交 Session 请求。出现问题时，保留双方同一时段的完整游戏日志，并分别调用 `SaveDiagnosticReport`；报告固定写入 `Project/Saved/Logs/FUOnlineSession`。

### Provider 自动日志（2.1.1）

启用默认配置 `bEnableDiagnosticLog=true` 后，无需调用保存节点，每条经过统一诊断入口的事件（包括环境快照）都会自动写入：

```text
Saved/Logs/FUOnlineSession/OnlineSubsystemLog/
├── steam_log/FUOnlineSession-<UTC时间>-<实例GUID>.log
└── lan_log/FUOnlineSession-<UTC时间>-<实例GUID>.log
```

路径基于当前游戏的 `FPaths::ProjectLogDir()`，不是 `Plugins/FUOnlineSession` 安装目录。编辑器项目通常使用项目 `Saved/Logs`，打包后以 UE 实际可写日志目录为准；Blueprint 的 `GetProviderLogPath(Provider)` 返回当前 GameInstance 的完整绝对路径。查询本身不创建文件：该 Provider 首次有诊断输出时才创建，关闭日志或写入失败时返回的路径可能尚不存在。

文件严格按事件的 `Provider` 路由，NULL/LAN 写入 `lan_log`。同一进程同时产生两种 Provider 的诊断时会出现两个目录；初始化事件也按自身 Provider 归档，不以最后一次点击的联机模式重新归类。每个 GameInstance 使用不同 GUID 文件名，防止不同进程或 PIE 实例互相覆盖；同一实例持续追加，清理内存历史或屏幕提示过期均不会清除磁盘记录。

自动日志保留事件 UTC 时间、序号、Provider、OperationId、操作阶段、世界/PIE 上下文、错误码、原因、处理建议及安全字段；文件写入前使用与 Blueprint/UE_LOG 相同的脱敏规则。它记录插件的结构化诊断流，不是 Steam SDK 或整个引擎 `LogOnline` 的完整镜像，调查底层问题时仍需同时提供游戏主日志。

`bEnableDiagnosticLog=false` 同时关闭 UE_LOG 诊断出口及自动文件，不关闭历史和 Blueprint。自动文件写入不依赖 Shipping 的 UE_LOG 编译开关。目录创建或追加失败时，会通过仍可用的诊断通道发出一次 `FU.Diagnostics.ProviderLogWriteFailed`，停止当前 GameInstance 中该 Provider 的文件写入以避免重复 IO/刷屏；另一 Provider 不受影响。修复权限、磁盘空间或文件占用后，重启游戏/PIE 恢复记录。

`SaveDiagnosticReport` 仍是手动导出近期有界历史的汇总报告，不替代自动追加文件，也不会更改旧的报告路径。自动日志目前不按条数淘汰、不自动删除旧文件，长期测试后可在退出游戏时手动归档。

## 双电脑验收顺序

Steam 测试必须满足：

1. 两台电脑运行同一次 Win64 打包产物，并登录两个不同 Steam 账号。
2. Steam 客户端已启动；两端 `RunProviderDiagnostics(Steam)` 都返回 `Ready`。
3. 房主创建后保持进程和 Listen 地图在线，客户端再搜索。
4. 搜索指定房间名时，两端输入必须完全一致；空字符串表示浏览当前项目房间。
5. 若日志出现 `Fallback.Empty`，确认房间未满、房主未退出，并核对两端 Steam 下载区域。
6. 搜索得到结果后，以该结果的 `SessionId` 加入；再检查 `OnJoinSessionCompleteV2` 和 `OnOnlineConnectionFailure`。

LAN 测试需要两台电脑位于允许 UDP 广播的同一局域网，Windows 防火墙必须允许打包程序。Steam 和 LAN 可以在同一插件中分别搜索，但一个游戏 World 同时只允许一种活动 NetDriver；切换 Provider 前应先退出联网地图并销毁原会话。

## 打包检查

推荐至少验证 Development 和 Shipping 两种 Win64 配置。Development 可使用 AppID 480；Shipping 必须使用项目自己的 Steam AppID。BuildPlugin 分发包应包含两个源 Config；最终游戏包应通过合并后的有效配置与启动日志验证其生效，而不要求源 ini 以松散文件存在。还应确认 Runtime 模块、`steam_api64.dll` 与 SteamSockets 相关模块进入 Stage，且启动日志没有 `SubsystemUnavailable`、AppID mismatch 或 P2P 配置迁移警告。

## 密码说明

`RoomPassword` 是可发现会话元数据，只用于加入前的便利筛选，不是安全认证。需要真正的访问控制时，应在客户端连接后由服务器执行登录或玩法授权；不要把账号凭据、票据或 Token 放进 SessionSettings。
