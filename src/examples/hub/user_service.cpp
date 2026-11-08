#include "user_service.h"

#include "request_types.h"
#include "user_normal.h"
#include "user_staff.h"

namespace example {
namespace {

std::shared_ptr<IUser> makeStaff() {
    return std::make_shared<UserStaff>();
}

std::shared_ptr<IUser> makeNormal() {
    return std::make_shared<UserNormal>();
}

} // namespace

#define USER_SERVICE_FACTORY_CASES(X) \
    X(StaffRequest, makeStaff) \
    X(NormalRequest, makeNormal)

WIREKIT_DEFINE_FACTORY(UserService, IUser, USER_SERVICE_FACTORY_CASES)

#undef USER_SERVICE_FACTORY_CASES
} // namespace example
