#include "ECPCore.h"
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY(LogECP);

/** 策略和跨地图契约持有进程级状态，不允许在多个 PIE 实例存活时热卸载。 */
class FECPFoundationModule final : public IModuleInterface
{
public:
	virtual bool SupportsDynamicReloading() override { return false; }
};

IMPLEMENT_MODULE(FECPFoundationModule, ECPFoundation)
