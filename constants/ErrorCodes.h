//
// Created by Emmanuel Addo-Odame on 19/05/2026.
//

#ifndef WSSDAPI_ERRORCODES_H
#define WSSDAPI_ERRORCODES_H

namespace wssd_api::constants {

    enum ErrorCode {
        ERR_DB_CONNECTION = 1001,         // Database connection error
        ERR_DB_QUERY = 1002,              // Database query error
        ERR_DB_NOT_FOUND = 1003,          // Resource not found in database
        ERR_RESOURCE_NOT_FOUND = 1004, // General resource not found error
        ERR_VALIDATION = 1101,            // Input validation failed
        ERR_AUTH_INVALID_CREDENTIALS = 1201, // Invalid username or password
        ERR_AUTH_LOCKED_OUT = 1202,       // User account is locked out
        ERR_AUTH_INACTIVE = 1203,         // User account is inactive
        ERR_AUTH_TOKEN_EXPIRED = 1204,    // JWT token expired
        ERR_AUTH_TOKEN_INVALID = 1205,    // JWT token is invalid
        ERR_DUPLICATE_RESOURCE = 1301,    // Resource already exists
        ERR_INTERNAL = 1500,              // Internal server error
        ERR_PERMISSION_DENIED = 1401,     // Insufficient permissions
        ERR_MISSING_PARAMETER = 1102,     // Required parameter is missing
        ERR_UNSUPPORTED_OPERATION = 1601  // Operation not supported
    };
}
#endif //WSSDAPI_ERRORCODES_H