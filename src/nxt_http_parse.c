
/*
 * Copyright (C) NGINX, Inc.
 * Copyright (C) Valentin V. Bartenev
 */

#include <nxt_main.h>


typedef struct {
    nxt_http_field_handler_t  handler;
    uintptr_t                 data;
    union {
        uint8_t               str[8];
        uint64_t              ui64;
    } key[];
} nxt_http_fields_hash_entry_t;


#define nxt_http_fields_hash_next_entry(entry, n)                             \
    ((nxt_http_fields_hash_entry_t *) ((u_char *) (entry)                     \
                                       + sizeof(nxt_http_fields_hash_entry_t) \
                                       + n * 8))


struct nxt_http_fields_hash_s {
    size_t                        min_length;
    size_t                        max_length;
    void                          *long_fields;
    nxt_http_fields_hash_entry_t  *entries[];
};


static nxt_int_t nxt_http_parse_unusual_target(nxt_http_request_parse_t *rp,
    u_char **pos, u_char *end);
static nxt_int_t nxt_http_parse_request_line(nxt_http_request_parse_t *rp,
    u_char **pos, u_char *end);
static nxt_int_t nxt_http_parse_field_name(nxt_http_request_parse_t *rp,
    u_char **pos, u_char *end);
static nxt_int_t nxt_http_parse_field_value(nxt_http_request_parse_t *rp,
    u_char **pos, u_char *end);
static u_char *nxt_http_lookup_field_end(u_char *p, u_char *end);
static nxt_int_t nxt_http_parse_field_end(nxt_http_request_parse_t *rp,
    u_char **pos, u_char *end);

static nxt_http_fields_hash_entry_t *nxt_http_fields_hash_lookup(
    nxt_http_fields_hash_t *hash, uint64_t *key, nxt_str_t *value);
static nxt_http_fields_hash_entry_t *nxt_http_header_fields_hash_lookup_long(
    nxt_http_fields_hash_t *hash, nxt_str_t *value);


typedef enum {
    NXT_HTTP_TARGET_SPACE = 1,   /* \s  */
    NXT_HTTP_TARGET_HASH,        /*  #  */
    NXT_HTTP_TARGET_AGAIN,
    NXT_HTTP_TARGET_BAD,         /* \0\r\n */

    /* traps below are used for extended check only */

    NXT_HTTP_TARGET_SLASH = 5,   /*  /  */
    NXT_HTTP_TARGET_DOT,         /*  .  */
    NXT_HTTP_TARGET_ARGS_MARK,   /*  ?  */
    NXT_HTTP_TARGET_QUOTE_MARK,  /*  %  */
    NXT_HTTP_TARGET_PLUS,        /*  +  */
} nxt_http_target_traps_e;


static const uint8_t  nxt_http_target_chars[256] nxt_aligned(64) = {
    /* \0                               \n        \r       */
        4, 0, 0, 0,  0, 0, 0, 0,   0, 0, 4, 0,  0, 4, 0, 0,
        0, 0, 0, 0,  0, 0, 0, 0,   0, 0, 0, 0,  0, 0, 0, 0,
    /*
     * \s  !  "  #   $  %  &  '    (  )  *  +   ,  -  .  /
     *  0  1  2  3   4  5  6  7    8  9  :  ;   <  =  >  ?
     */
        1, 0, 0, 2,  0, 8, 0, 0,   0, 0, 0, 9,  0, 0, 6, 5,
        0, 0, 0, 0,  0, 0, 0, 0,   0, 0, 0, 0,  0, 0, 0, 7,
};


nxt_inline nxt_http_target_traps_e
nxt_http_parse_target(u_char **pos, u_char *end)
{
    u_char      *p;
    nxt_uint_t  trap;

    p = *pos;

    for ( ;; ) {
        if (nxt_slow_path(end - p < 10)) {
            return NXT_HTTP_TARGET_AGAIN;
        }

#define nxt_http_parse_target_step                                            \
        {                                                                     \
            trap = nxt_http_target_chars[*p];                                 \
                                                                              \
            if (nxt_slow_path(trap != 0)) {                                   \
                break;                                                        \
            }                                                                 \
                                                                              \
            p++;                                                              \
        }

        nxt_http_parse_target_step
        nxt_http_parse_target_step
        nxt_http_parse_target_step
        nxt_http_parse_target_step

        nxt_http_parse_target_step
        nxt_http_parse_target_step
        nxt_http_parse_target_step
        nxt_http_parse_target_step

        nxt_http_parse_target_step
        nxt_http_parse_target_step

#undef nxt_http_parse_target_step
    }

    *pos = p;

    return trap;
}


nxt_int_t
nxt_http_parse_request(nxt_http_request_parse_t *rp, nxt_buf_mem_t *b)
{
    nxt_int_t  rc;

    if (rp->handler == NULL) {
        rp->handler = &nxt_http_parse_request_line;
    }

    do {
        rc = rp->handler(rp, &b->pos, b->free);
    } while (rc == NXT_OK);

    return rc;
}


static nxt_int_t
nxt_http_parse_request_line(nxt_http_request_parse_t *rp, u_char **pos,
    u_char *end)
{
    u_char                   *p, ch, *after_slash;
    nxt_int_t                rc;
    nxt_http_ver_t           version;
    nxt_http_target_traps_e  trap;

    static const nxt_http_ver_t  http11 = { "HTTP/1.1" };
    static const nxt_http_ver_t  http10 = { "HTTP/1.0" };

    p = *pos;

    rp->method.start = p;

    for ( ;; p++) {

        for ( ;; ) {
            if (nxt_slow_path(end - p < 12)) {
                return NXT_AGAIN;
            }

#define nxt_http_parse_request_line_step                                      \
            {                                                                 \
                ch = *p;                                                      \
                                                                              \
                if (nxt_slow_path(ch < 'A' || ch > 'Z')) {                    \
                    break;                                                    \
                }                                                             \
                                                                              \
                p++;                                                          \
            }

            nxt_http_parse_request_line_step
            nxt_http_parse_request_line_step
            nxt_http_parse_request_line_step
            nxt_http_parse_request_line_step

            nxt_http_parse_request_line_step
            nxt_http_parse_request_line_step
            nxt_http_parse_request_line_step
            nxt_http_parse_request_line_step

#undef nxt_http_parse_request_line_step
        }

        if (nxt_fast_path(ch == ' ')) {
            rp->method.length = p - rp->method.start;
            break;
        }

        if (ch == '_' || ch == '-') {
            continue;
        }

        if (rp->method.start == p && (ch == NXT_CR || ch == NXT_LF)) {
            rp->method.start++;
            continue;
        }

        return NXT_ERROR;
    }

    p++;

    if (nxt_slow_path(p == end)) {
        return NXT_AGAIN;
    }

    /* target */

    ch = *p;

    if (nxt_slow_path(ch != '/')) {
        rc = nxt_http_parse_unusual_target(rp, &p, end);

        if (nxt_slow_path(rc != NXT_OK)) {
            return rc;
        }
    }

    rp->target_start = p;

    after_slash = p + 1;

    for ( ;; ) {
        p++;

        trap = nxt_http_parse_target(&p, end);

        switch (trap) {
        case NXT_HTTP_TARGET_SLASH:
            if (nxt_slow_path(after_slash == p)) {
                rp->complex_target = 1;
                goto rest_of_target;
            }

            after_slash = p + 1;

            rp->exten_start = NULL;
            continue;

        case NXT_HTTP_TARGET_DOT:
            if (nxt_slow_path(after_slash == p)) {
                rp->complex_target = 1;
                goto rest_of_target;
            }

            rp->exten_start = p + 1;
            continue;

        case NXT_HTTP_TARGET_ARGS_MARK:
            rp->args_start = p + 1;
            goto rest_of_target;

        case NXT_HTTP_TARGET_SPACE:
            rp->target_end = p;
            goto space_after_target;

        case NXT_HTTP_TARGET_QUOTE_MARK:
            rp->quoted_target = 1;
            goto rest_of_target;

        case NXT_HTTP_TARGET_PLUS:
            rp->plus_in_target = 1;
            continue;

        case NXT_HTTP_TARGET_HASH:
            rp->complex_target = 1;
            goto rest_of_target;

        case NXT_HTTP_TARGET_AGAIN:
            return NXT_AGAIN;

        case NXT_HTTP_TARGET_BAD:
            return NXT_ERROR;
        }

        nxt_unreachable();
    }

rest_of_target:

    for ( ;; ) {
        p++;

        trap = nxt_http_parse_target(&p, end);

        switch (trap) {
        case NXT_HTTP_TARGET_SPACE:
            rp->target_end = p;
            goto space_after_target;

        case NXT_HTTP_TARGET_HASH:
            rp->complex_target = 1;
            continue;

        case NXT_HTTP_TARGET_AGAIN:
            return NXT_AGAIN;

        case NXT_HTTP_TARGET_BAD:
            return NXT_ERROR;

        default:
            continue;
        }

        nxt_unreachable();
    }

space_after_target:

    if (nxt_slow_path(end - p < 10)) {
        return NXT_AGAIN;
    }

    /* " HTTP/1.1\r\n" or " HTTP/1.1\n" */

    nxt_memcpy(version.str, &p[1], 8);

    if (nxt_fast_path((version.ui64 == http11.ui64
                       || version.ui64 == http10.ui64
                       || (p[1] == 'H'
                           && p[2] == 'T'
                           && p[3] == 'T'
                           && p[4] == 'P'
                           && p[5] == '/'
                           && p[6] >= '0' && p[6] <= '9'
                           && p[7] == '.'
                           && p[8] >= '0' && p[8] <= '9'))
                      && (p[9] == '\r' || p[9] == '\n')))
    {
        rp->version.ui64 = version.ui64;

        if (nxt_fast_path(p[9] == '\r')) {
            p += 10;

            if (nxt_slow_path(p == end)) {
                return NXT_AGAIN;
            }

            if (nxt_slow_path(*p != '\n')) {
                return NXT_ERROR;
            }

            *pos = p + 1;
            return nxt_http_parse_field_name(rp, pos, end);
        }

        *pos = p + 10;
        return nxt_http_parse_field_name(rp, pos, end);
    }

    if (p[1] == ' ') {
        /* surplus space after tartet */
        p++;
        goto space_after_target;
    }

    rp->space_in_target = 1;
    goto rest_of_target;
}


static nxt_int_t
nxt_http_parse_unusual_target(nxt_http_request_parse_t *rp, u_char **pos,
    u_char *end)
{
    u_char  *p, ch;

    p = *pos;

    ch = *p;

    if (ch == ' ') {
        /* skip surplus spaces before target */

        do {
            p++;

            if (nxt_slow_path(p == end)) {
                return NXT_AGAIN;
            }

            ch = *p;

        } while (ch == ' ');

        if (ch == '/') {
            *pos = p;
            return NXT_OK;
        }
    }

    /* absolute path or '*' */

    /* TODO */

    return NXT_ERROR;
}


static nxt_int_t
nxt_http_parse_field_name(nxt_http_request_parse_t *rp, u_char **pos,
    u_char *end)
{
    u_char  *p, ch, c;
    size_t  i, size;

    static const u_char  normal[256]  nxt_aligned(64) =
        "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0"
        "\0\0\0\0\0\0\0\0\0\0\0\0\0-\0\0" "0123456789\0\0\0\0\0\0"

        /* These 64 bytes should reside in one cache line. */
        "\0abcdefghijklmnopqrstuvwxyz\0\0\0\0\0"
        "\0abcdefghijklmnopqrstuvwxyz\0\0\0\0\0"

        "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0"
        "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0"
        "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0"
        "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0";

    p = *pos;

    size = end - p;
    i = rp->offset;

#define nxt_http_parse_field_name_step                                        \
    {                                                                         \
        ch = p[i];                                                            \
        c = normal[ch];                                                       \
                                                                              \
        if (nxt_slow_path(c == '\0')) {                                       \
            goto name_end;                                                    \
        }                                                                     \
                                                                              \
        rp->field_name_key.str[i % 32] = c;                                   \
        i++;                                                                  \
    }

    while (nxt_fast_path(size - i >= 8)) {
        nxt_http_parse_field_name_step
        nxt_http_parse_field_name_step
        nxt_http_parse_field_name_step
        nxt_http_parse_field_name_step

        nxt_http_parse_field_name_step
        nxt_http_parse_field_name_step
        nxt_http_parse_field_name_step
        nxt_http_parse_field_name_step
    }

    while (nxt_fast_path(i != size)) {
        nxt_http_parse_field_name_step
    }

#undef nxt_http_parse_field_name_step

    rp->offset = i;
    rp->handler = &nxt_http_parse_field_name;

    return NXT_AGAIN;

name_end:

    if (nxt_fast_path(ch == ':')) {
        if (nxt_slow_path(i == 0)) {
            return NXT_ERROR;
        }

        *pos = &p[i] + 1;

        rp->field_name.start = p;
        rp->field_name.length = i;

        rp->offset = 0;

        return nxt_http_parse_field_value(rp, pos, end);
    }

    *pos = &p[i];

    rp->field_name.length = 0;

    return nxt_http_parse_field_end(rp, pos, end);
}


static nxt_int_t
nxt_http_parse_field_value(nxt_http_request_parse_t *rp, u_char **pos,
    u_char *end)
{
    u_char  *p, ch;

    p = *pos;

    for ( ;; ) {
        if (nxt_slow_path(p == end)) {
            *pos = p;
            rp->handler = &nxt_http_parse_field_value;
            return NXT_AGAIN;
        }

        if (*p != ' ') {
            break;
        }

        p++;
    }

    *pos = p;

    p += rp->offset;

    for ( ;; ) {
        p = nxt_http_lookup_field_end(p, end);

        if (nxt_slow_path(p == end)) {
            rp->offset = p - *pos;
            rp->handler = &nxt_http_parse_field_value;
            return NXT_AGAIN;
        }

        ch = *p;

        if (nxt_fast_path(ch == '\r' || ch == '\n')) {
            break;
        }

        if (ch == '\0') {
            return NXT_ERROR;
        }
    }

    if (nxt_fast_path(p != *pos)) {
        while (p[-1] == ' ') {
            p--;
        }
    }

    rp->offset = 0;

    rp->field_value.start = *pos;
    rp->field_value.length = p - *pos;

    *pos = p;

    return nxt_http_parse_field_end(rp, pos, end);
}


static u_char *
nxt_http_lookup_field_end(u_char *p, u_char *end)
{
    nxt_uint_t  n;

#define nxt_http_lookup_field_end_step                                        \
    {                                                                         \
        if (nxt_slow_path(*p < 0x10)) {                                       \
            return p;                                                         \
        }                                                                     \
                                                                              \
        p++;                                                                  \
    }

    for (n = (end - p) / 16; nxt_fast_path(n != 0); n--) {
        nxt_http_lookup_field_end_step
        nxt_http_lookup_field_end_step
        nxt_http_lookup_field_end_step
        nxt_http_lookup_field_end_step

        nxt_http_lookup_field_end_step
        nxt_http_lookup_field_end_step
        nxt_http_lookup_field_end_step
        nxt_http_lookup_field_end_step

        nxt_http_lookup_field_end_step
        nxt_http_lookup_field_end_step
        nxt_http_lookup_field_end_step
        nxt_http_lookup_field_end_step

        nxt_http_lookup_field_end_step
        nxt_http_lookup_field_end_step
        nxt_http_lookup_field_end_step
        nxt_http_lookup_field_end_step
    }

    for (n = (end - p) / 4; nxt_fast_path(n != 0); n--) {
        nxt_http_lookup_field_end_step
        nxt_http_lookup_field_end_step
        nxt_http_lookup_field_end_step
        nxt_http_lookup_field_end_step
    }

    switch (end - p) {
    case 3:
        nxt_http_lookup_field_end_step
    case 2:
        nxt_http_lookup_field_end_step
    case 1:
        nxt_http_lookup_field_end_step
    case 0:
        break;
    default:
        nxt_unreachable();
    }

#undef nxt_http_lookup_field_end_step

    return p;
}


static nxt_int_t
nxt_http_parse_field_end(nxt_http_request_parse_t *rp, u_char **pos,
    u_char *end)
{
    u_char                        *p;
    nxt_int_t                     rc;
    nxt_http_fields_hash_entry_t  *entry;

    p = *pos;

    if (nxt_fast_path(*p == '\r')) {
        p++;

        if (nxt_slow_path(p == end)) {
            rp->handler = &nxt_http_parse_field_end;
            return NXT_AGAIN;
        }
    }

    if (nxt_fast_path(*p == '\n')) {
        *pos = p + 1;

        if (rp->field_name.length != 0) {
            entry = nxt_http_fields_hash_lookup(rp->hash,
                                                rp->field_name_key.ui64,
                                                &rp->field_name);

            if (entry != NULL) {
                rc = entry->handler(rp->ctx, &rp->field_name, &rp->field_value,
                                    entry->data);

                if (nxt_slow_path(rc != NXT_OK)) {
                    return NXT_ERROR;
                }
            }

            nxt_memzero(rp->field_name_key.str, 32);

            rp->handler = &nxt_http_parse_field_name;
            return NXT_OK;
        }

        return NXT_DONE;
    }

    return NXT_ERROR;
}


static nxt_http_fields_hash_entry_t *
nxt_http_fields_hash_lookup(nxt_http_fields_hash_t *hash, uint64_t *key,
    nxt_str_t *value)
{
    nxt_http_fields_hash_entry_t  *entry;

    if (hash == NULL || value->length < hash->min_length) {
        return NULL;
    }

    if (value->length > hash->max_length) {
        if (value->length > 32 && hash->long_fields != NULL) {
            return nxt_http_header_fields_hash_lookup_long(hash, value);
        }

        return NULL;
    }

    entry = hash->entries[value->length - hash->min_length];

    if (entry == NULL) {
        return NULL;
    }

    switch ((value->length + 7) / 8) {
    case 1:
        do {
            if (entry->key[0].ui64 == key[0]) {
                return entry;
            }

            entry = nxt_http_fields_hash_next_entry(entry, 1);

        } while (entry->handler != NULL);

        break;

    case 2:
        do {
            if (entry->key[0].ui64 == key[0]
                && entry->key[1].ui64 == key[1])
            {
                return entry;
            }

            entry = nxt_http_fields_hash_next_entry(entry, 2);

        } while (entry->handler != NULL);

        break;

    case 3:
        do {
            if (entry->key[0].ui64 == key[0]
                && entry->key[1].ui64 == key[1]
                && entry->key[2].ui64 == key[2])
            {
                return entry;
            }

            entry = nxt_http_fields_hash_next_entry(entry, 3);

        } while (entry->handler != NULL);

        break;

    case 4:
        do {
            if (entry->key[0].ui64 == key[0]
                && entry->key[1].ui64 == key[1]
                && entry->key[2].ui64 == key[2]
                && entry->key[3].ui64 == key[3])
            {
                return entry;
            }

            entry = nxt_http_fields_hash_next_entry(entry, 4);

        } while (entry->handler != NULL);

        break;

    default:
        nxt_unreachable();
    }

    return NULL;
}


static nxt_http_fields_hash_entry_t *
nxt_http_header_fields_hash_lookup_long(nxt_http_fields_hash_t *hash,
    nxt_str_t *value)
{
    /* TODO */
    return NULL;
}


nxt_http_fields_hash_t *
nxt_http_fields_hash(nxt_http_fields_t *fields, nxt_mem_pool_t *mp)
{
    size_t                        min_length, max_length, length, size;
    nxt_uint_t                    i, j, n;
    nxt_http_fields_hash_t        *hash;
    nxt_http_fields_hash_entry_t  *entry;

    min_length = 32 + 1;
    max_length = 0;

    for (i = 0; fields[i].handler != NULL; i++) {
        length = fields[i].name.length;

        if (length > 32) {
            /* TODO */
            return NULL;
        }

        min_length = nxt_min(length, min_length);
        max_length = nxt_max(length, max_length);
    }

    size = sizeof(nxt_http_fields_hash_t);

    if (min_length <= 32) {
        size += (max_length - min_length + 1)
                * sizeof(nxt_http_fields_hash_entry_t *);
    }

    hash = nxt_mem_zalloc(mp, size);
    if (nxt_slow_path(hash == NULL)) {
        return NULL;
    }

    hash->min_length = min_length;
    hash->max_length = max_length;

    for (i = 0; fields[i].handler != NULL; i++) {
        length = fields[i].name.length;
        entry = hash->entries[length - min_length];

        if (entry != NULL) {
            continue;
        }

        n = 1;

        for (j = i + 1; fields[j].handler != NULL; j++) {
            if (length == fields[j].name.length) {
                n++;
            }
        }

        size = sizeof(nxt_http_fields_hash_entry_t) + nxt_align_size(length, 8);

        entry = nxt_mem_zalloc(mp, n * size
                                   + sizeof(nxt_http_fields_hash_entry_t));

        if (nxt_slow_path(entry == NULL)) {
            return NULL;
        }

        hash->entries[length - min_length] = entry;

        for (j = i; fields[j].handler != NULL; j++) {
            if (length != fields[j].name.length) {
                continue;
            }

            entry->handler = fields[j].handler;
            entry->data = fields[j].data;

            nxt_memcpy_lowcase(entry->key->str, fields[j].name.start, length);

            n--;

            if (n == 0) {
                break;
            }

            entry = (nxt_http_fields_hash_entry_t *) ((u_char *) entry + size);
        }
    }

    return hash;
}
