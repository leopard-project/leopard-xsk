#pragma once

#include "krlibc.h"
#include "llist.h"

struct membuf {
    void  *buffer;
    size_t size;
};

struct vecbuf {
    struct llist_header components;
    struct membuf       buf;
    size_t              acc_sz;
};

/* struct vecbuf is used by AHCI/device scatter-gather paths. The former
 * vbuf_* helpers had no implementation or caller in this tree; removed. */
