//
//  uuid.c
//  hfs-freebsd
//
//  Created by jothwolo on 8/16/23.
//  Copyright © 2023-present jothwolo. All rights reserved.
//  This file is covered under the MPL2.0. See LICENSE file for more details.
//

#include <uuid/uuid.h>
#include <sys/systm.h>

void uuid_clear(uuid_t uu)
{
    bzero(uu, sizeof(uuid_t));
}

int uuid_compare(const uuid_t uu1, const uuid_t uu2)
{
    return memcmp(uu1, uu2, sizeof(uuid_t));
}

void uuid_copy(uuid_t dst, const uuid_t src)
{
    memcpy(dst, src, sizeof(uuid_t));
}

void uuid_generate(uuid_t out)
{
    arc4random_buf(out, sizeof(uuid_t));
}

void uuid_generate_random(uuid_t out)
{
    arc4random_buf(out, sizeof(uuid_t));
}

void uuid_generate_time(uuid_t out)
{
    struct uuid store;
    kern_uuidgen(&store, sizeof(store));
    memcpy(out, &store, sizeof(uuid_t));
}

void uuid_generate_early_random(uuid_t out)
{
    arc4random_buf(out, sizeof(uuid_t));
}

int uuid_is_null(const uuid_t uu)
{
    return memcmp(uu, &(uuid_t){0}, sizeof(uuid_t));
}

int uuid_parse(const uuid_string_t in, uuid_t uu)
{
    struct uuid store;
    int ret = parse_uuid(in, &store);
    memcpy(uu, &store, sizeof(uuid_t));
    return ret;
}

void uuid_unparse(const uuid_t uu, uuid_string_t out)
{
    struct uuid store;
    memcpy(&store, uu, sizeof(uuid_t));
    snprintf_uuid(out, sizeof(uuid_string_t), &store);
}

void uuid_unparse_lower(const uuid_t uu, uuid_string_t out)
{
    panic("%s: unsupported function", __func__);
}

void uuid_unparse_upper(const uuid_t uu, uuid_string_t out)
{
    panic("%s: unsupported function", __func__);
}

