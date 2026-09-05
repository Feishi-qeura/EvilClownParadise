#include "FUOnlineSessionModule.h"

// The module owns no global session state; game-instance subsystems manage it per world.
IMPLEMENT_MODULE(FFUOnlineSessionModule, FUOnlineSession)
