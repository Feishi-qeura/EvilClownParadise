# BP_Actor 多人关卡同步

BP_Actor 的 GameFeature 父类 `AECPPlayerCountActor` 新增蓝图函数 **同步切换关卡（服务器）**，C++ 名称 `TravelAllPlayersToLevel`，位于 ECPInteraction。

蓝图调用方式：服务器 Box 重叠事件 → 原有角色检查 → **所有玩家是否在 Box 内** → True → **同步切换关卡（服务器）**。在 `Destination Level` 选择目标地图，例如 `/Game/Project/World/Maps/Demo.Demo`。当前 BP_Actor 已替换原 `OpenLevelBySoftObjectPtr` 节点，保留现有执行线及 Delay，目标为生产 Demo。

函数由服务器发起 `UWorld::ServerTravel`，已连接客户端由引擎通知跟随。客户端无需自己执行关卡跳转，也无需给无客户端所有者的地图 Actor 添加 Server RPC。`BlueprintAuthorityOnly` 元数据之外，运行时还检查世界网络模式，阻止非复制地图 Actor 在客户端具有本地 Authority 时误执行。

无目标地图、地图不存在、客户端调用或世界已在跳转时返回 false；true 表示引擎已接受跳转请求，加载完成是异步的。函数不自行判断 Box 到齐，保留蓝图中已有的条件控制。

相对 ServerTravel 保留服务器连接端口；监听服务器继续使用 `listen`，空 `game=` 清除旧大厅的 GameMode URL 覆盖，使目标地图的 WorldSettings 生效。采用引擎普通服务器跳转流程；短暂加载和角色重新生成属于正常行为。

## 验证

- 修复前：新同步函数缺失、蓝图同步连线检查失败。
- Editor / Game Development 编译通过。
- 修改后的 BP_Actor 与三张生产关卡 Windows Cook 通过（0 errors）；28 条既有告警仍是模板 GameplayTag、禁用 Steam 和原贴花缺少贴图。旧 Test_Map 目录在此次 Cook 中被排除。
- `ECP.Interaction.Level` 五项自动化通过：BPActorWiring、CurrentWorldPlayerCount、ServerTravelContract、TwoPlayerBoxPIE、MultiplayerTravelPIE。
- 实际 MultiplayerTravelPIE 使用原大厅 SinglePlayerMap、已保存 BP_Actor 和两名玩家；一名玩家进入 Box 不跳转，第二名玩家进入后触发现有蓝图流程。
- 服务器与客户端均到达生产 Demo；服务器仍有两名玩家，监听端口保持测试用的 17883，GameMode 为 Demo 的 BP_ECGameMode。
- 测试验证了客户端本地 Authority Actor 不能触发该函数，以及无效目标和重复请求不被接受。
- BMMCSystem 原文件保持不变。备份蓝图位于 `Saved/LevelTravelValidation/BP_Actor.before-sync-travel.uasset`；报告和日志位于同目录。

验证范围是同进程监听服务器与远端客户端的真实 PIE 网络连接；未验证跨机器 Steam 或专用服务器部署。
