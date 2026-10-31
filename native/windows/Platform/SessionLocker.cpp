#include "SessionLocker.h"

#include <windows.h>

namespace telaviva {

bool Win32SessionLocker::lockNow()
{
    return LockWorkStation() != 0;
}

}  // namespace telaviva
