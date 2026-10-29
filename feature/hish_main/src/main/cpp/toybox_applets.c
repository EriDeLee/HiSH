// Enumerate exactly the applets selected by the existing Toybox configuration.
#include "toys.h"
#undef NEWTOY
#undef OLDTOY
#define NEWTOY(name, opts, flags) #name " "
#define OLDTOY(name, oldname, flags) #name " "
const char* HiSHBuiltinApplets(void)
{
    return
#include "generated/newtoys.h"
    "";
}
