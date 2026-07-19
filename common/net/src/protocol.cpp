#include "net/protocol.h"

namespace net {

const char* toString(AuthResult result) {
    switch (result) {
        case AuthResult::Success: return "success";
        case AuthResult::InvalidCredentials: return "invalid username or password";
        case AuthResult::VersionMismatch: return "client/server protocol version mismatch";
        case AuthResult::AccountExists: return "account already exists";
        case AuthResult::ServerError: return "server error";
        case AuthResult::MalformedRequest: return "malformed request";
        case AuthResult::NotAuthorised: return "not authorised (bad admin secret)";
        case AuthResult::NoSuchAccount: return "no such account";
        case AuthResult::InsufficientPermission: return "your account lacks permission";
        case AuthResult::SessionExpired: return "session expired -- log in again";
    }
    return "unknown";
}

}  // namespace net
