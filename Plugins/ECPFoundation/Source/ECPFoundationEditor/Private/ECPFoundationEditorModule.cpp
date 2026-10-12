#include "Modules/ModuleManager.h"

// 模块装载本身不迁移或保存资产；只有显式运行 commandlet 才进入对应模式。
IMPLEMENT_MODULE(FDefaultModuleImpl, ECPFoundationEditor);
