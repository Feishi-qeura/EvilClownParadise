#include "Modules/ModuleManager.h"

// 模块本身不建立会话、注册在线后端或触发旅行；FUOnlineSession 继续管理其完整生命周期。
IMPLEMENT_MODULE(FDefaultModuleImpl, ECPOnline)
