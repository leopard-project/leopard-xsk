#pragma once

// File-access descriptor bits used by XAPI_QUERY_FILE_ACCESS /
// XAPI_REQUEST_FILE_ACCESS. Mirrors the XACT 2026v5 user-space header.

enum
{
    XFA_READ = 1,
    XFA_WRITE = 2,
    XFA_LIST = 4,
    XFA_STAT = 8,
    XFA_CREATE = 16,
    XFA_REMOVE = 32,
    XFA_NOFOLLOW = 64,
    XFA_EXCLUSIVE = 128,
    XFA_EXPORT = 256
};
