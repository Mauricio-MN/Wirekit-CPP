#ifndef EXAMPLE_USER_SERVICE_H
#define EXAMPLE_USER_SERVICE_H

#include "wirekit.h"
#include "iuser.h"

namespace example {

// The factory is the only layer that needs to know concrete service classes.
WIREKIT_DECLARE_FACTORY(UserService, IUser);

} // namespace example

#endif
