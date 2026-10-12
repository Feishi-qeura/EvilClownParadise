#include "Modules/ModuleManager.h"

// 模块只注册原生类型；功能激活与退出由统一 Game Feature 生命周期入口管理。
IMPLEMENT_MODULE(FDefaultModuleImpl, ECPAI);
