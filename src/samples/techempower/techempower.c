/*
 * lwan - simple web server
 * Copyright (c) 2014 Leandro A. F. Pereira <leandro@hardinfo.org>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include <stdlib.h>
#include <string.h>

#include "lwan.h"
#include "lwan-config.h"
#include "lwan-template.h"

#include "database.h"
#include "json.h"

static const char hello_world[] = "Hello, World!";
static const char random_number_query[] = "SELECT randomNumber FROM World WHERE id=?";

struct Fortune {
    struct {
        coro_function_t generator;

        int id;
        char *message;
    } item;
};

DEFINE_ARRAY_TYPE_INLINEFIRST(fortune_array, struct Fortune)

static const char fortunes_template_str[] = "<!DOCTYPE html>" \
"<html>" \
"<head><title>Fortunes</title></head>" \
"<body>" \
"<table>" \
"<tr><th>id</th><th>message</th></tr>" \
"{{#item}}" \
"<tr><td>{{item.id}}</td><td>{{item.message}}</td></tr>" \
"{{/item}}" \
"</table>" \
"</body>" \
"</html>";

static int fortune_list_generator(struct coro *coro, void *data);

#undef TPL_STRUCT
#define TPL_STRUCT struct Fortune
static const struct lwan_var_descriptor fortune_item_desc[] = {
    TPL_VAR_INT(item.id),
    TPL_VAR_STR_ESCAPE(item.message),
    TPL_VAR_SENTINEL,
};

static const struct lwan_var_descriptor fortune_desc[] = {
    TPL_VAR_SEQUENCE(item, fortune_list_generator, fortune_item_desc),
    TPL_VAR_SENTINEL,
};

static struct db *database;
static struct lwan_tpl *fortune_tpl;

static enum lwan_http_status
json_response(struct lwan_response *response, JsonNode *node)
{
    size_t length;
    char *serialized;

    serialized = json_stringify_length(node, NULL, &length);
    json_delete(node);
    if (UNLIKELY(!serialized))
        return HTTP_INTERNAL_ERROR;

    lwan_strbuf_set(response->buffer, serialized, length);
    free(serialized);

    response->mime_type = "application/json";
    return HTTP_OK;
}

LWAN_HANDLER(json)
{
    JsonNode *hello = json_mkobject();
    if (UNLIKELY(!hello))
        return HTTP_INTERNAL_ERROR;

    json_append_member(hello, "message", json_mkstring(hello_world));

    return json_response(response, hello);
}

static JsonNode *
db_query(struct db_stmt *stmt, struct db_row rows[], struct db_row results[])
{
    JsonNode *object = NULL;
    int id = rand() % 10000;

    rows[0].u.i = id;

    if (UNLIKELY(!db_stmt_bind(stmt, rows, 1)))
        goto out;

    if (UNLIKELY(!db_stmt_step(stmt, results)))
        goto out;

    object = json_mkobject();
    if (UNLIKELY(!object))
        goto out;

    json_append_member(object, "id", json_mknumber(id));
    json_append_member(object, "randomNumber", json_mknumber(results[0].u.i));

out:
    return object;
}

LWAN_HANDLER(db)
{
    struct db_row rows[1] = {{ .kind = 'i' }};
    struct db_row results[] = {{ .kind = 'i' }, { .kind = '\0' }};
    struct db_stmt *stmt = db_prepare_stmt(database, random_number_query,
            sizeof(random_number_query) - 1);
    if (UNLIKELY(!stmt))
        return HTTP_INTERNAL_ERROR;

    JsonNode *object = db_query(stmt, rows, results);
    db_stmt_finalize(stmt);

    if (UNLIKELY(!object))
        return HTTP_INTERNAL_ERROR;

    return json_response(response, object);
}

LWAN_HANDLER(queries)
{
    const char *queries_str = lwan_request_get_query_param(request, "queries");
    long queries;

    if (LIKELY(queries_str)) {
        queries = parse_long(queries_str, -1);
        if (UNLIKELY(queries <= 0))
            queries = 1;
        else if (UNLIKELY(queries > 500))
            queries = 500;
    } else {
        queries = 1;
    }

    struct db_stmt *stmt = db_prepare_stmt(database, random_number_query,
            sizeof(random_number_query) - 1);
    if (UNLIKELY(!stmt))
        return HTTP_INTERNAL_ERROR;

    JsonNode *array = json_mkarray();
    if (UNLIKELY(!array))
        goto out_no_array;

    struct db_row rows[1] = {{ .kind = 'i' }};
    struct db_row results[] = {{ .kind = 'i' }, { .kind = '\0' }};
    while (queries--) {
        JsonNode *object = db_query(stmt, rows, results);
        
        if (UNLIKELY(!object))
            goto out_array;

        json_append_element(array, object);
    }

    db_stmt_finalize(stmt);
    return json_response(response, array);

out_array:
    json_delete(array);
out_no_array:
    db_stmt_finalize(stmt);
    return HTTP_INTERNAL_ERROR;
}

LWAN_HANDLER(plaintext)
{
    lwan_strbuf_set_static(response->buffer, hello_world, sizeof(hello_world) - 1);

    response->mime_type = "text/plain";
    return HTTP_OK;
}

static int fortune_compare(const void *a, const void *b)
{
    const struct Fortune *fortune_a = (const struct Fortune *)a;
    const struct Fortune *fortune_b = (const struct Fortune *)b;
    size_t a_len = strlen(fortune_a->item.message);
    size_t b_len = strlen(fortune_b->item.message);

    if (!a_len || !b_len)
        return a_len > b_len;

    size_t min_len = a_len < b_len ? a_len : b_len;
    int cmp = memcmp(fortune_a->item.message, fortune_b->item.message, min_len);
    if (cmp == 0)
        return a_len > b_len;

    return cmp > 0;
}

static bool append_fortune(struct coro *coro, struct fortune_array *fortunes,
                           int id, const char *message)
{
    struct Fortune *fortune;
    char *message_copy;

    message_copy = coro_strdup(coro, message);
    if (UNLIKELY(!message_copy))
        return false;

    fortune = fortune_array_append(fortunes);
    if (UNLIKELY(!fortune))
        return false;

    fortune->item.id = id;
    fortune->item.message = message_copy;

    return true;
}

static int fortune_list_generator(struct coro *coro, void *data)
{
    static const char fortune_query[] = "SELECT * FROM Fortune";
    char fortune_buffer[256];
    struct Fortune *fortune = data;
    struct fortune_array fortunes;
    struct db_stmt *stmt;
    size_t i;

    stmt = db_prepare_stmt(database, fortune_query, sizeof(fortune_query) - 1);
    if (UNLIKELY(!stmt))
        return 0;

    fortune_array_init(&fortunes);

    struct db_row results[] = {
        { .kind = 'i' },
        { .kind = 's', .u.s = fortune_buffer, .buffer_length = sizeof(fortune_buffer) },
        { .kind = '\0' }
    };
    while (db_stmt_step(stmt, results)) {
        if (!append_fortune(coro, &fortunes, results[0].u.i, results[1].u.s))
            goto out;
    }

    if (!append_fortune(coro, &fortunes, 0,
                            "Additional fortune added at request time."))
        goto out;

    fortune_array_sort(&fortunes, fortune_compare);

    for (i = 0; i < fortunes.base.elements; i++) {
        struct Fortune *f = &((struct Fortune *)fortunes.base.base)[i];
        fortune->item.id = f->item.id;
        fortune->item.message = f->item.message;
        coro_yield(coro, 1);
    }

out:
    fortune_array_reset(&fortunes);
    db_stmt_finalize(stmt);
    return 0;
}

LWAN_HANDLER(fortunes)
{
    struct Fortune fortune;

    if (UNLIKELY(!lwan_tpl_apply_with_buffer(fortune_tpl,
                                             response->buffer, &fortune)))
       return HTTP_INTERNAL_ERROR;

    response->mime_type = "text/html; charset=UTF-8";
    return HTTP_OK;
}

int
main(void)
{
    static const struct lwan_url_map url_map[] = {
        { .prefix = "/json", .handler = LWAN_HANDLER_REF(json) },
        { .prefix = "/db", .handler = LWAN_HANDLER_REF(db) },
        { .prefix = "/queries", .handler = LWAN_HANDLER_REF(queries) },
        { .prefix = "/plaintext", .handler = LWAN_HANDLER_REF(plaintext) },
        { .prefix = "/fortunes", .handler = LWAN_HANDLER_REF(fortunes) },
        { .prefix = NULL }
    };
    struct lwan l;

    lwan_init(&l);

    srand((unsigned int)time(NULL));

    if (getenv("USE_MYSQL")) {
        const char *user = getenv("MYSQL_USER");
        const char *password = getenv("MYSQL_PASS");
        const char *hostname = getenv("MYSQL_HOST");
        const char *db = getenv("MYSQL_DB");

        if (!user)
            lwan_status_critical("No MySQL user provided");
        if (!password)
            lwan_status_critical("No MySQL password provided");
        if (!hostname)
            lwan_status_critical("No MySQL hostname provided");
        if (!db)
            lwan_status_critical("No MySQL database provided");

        database = db_connect_mysql(hostname, user, password, db);
    } else {
        const char *pragmas[] = {
            "PRAGMA mmap_size=44040192",
            "PRAGMA journal_mode=OFF",
            "PRAGMA locking_mode=EXCLUSIVE",
            NULL
        };
        database = db_connect_sqlite("techempower.db", true, pragmas);
    }

    if (!database)
        lwan_status_critical("Could not connect to the database");

    fortune_tpl = lwan_tpl_compile_string(fortunes_template_str, fortune_desc);
    if (!fortune_tpl)
        lwan_status_critical("Could not compile fortune templates");

    lwan_set_url_map(&l, url_map);
    lwan_main_loop(&l);

    lwan_tpl_free(fortune_tpl);
    db_disconnect(database);
    lwan_shutdown(&l);

    return 0;
}
