/*
 * Copyright 2021-2024 D'Arcy Smith.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "p101_env/env.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

/* One tracked descriptor, with the site that opened it. */
struct p101_fd_record
{
    int                    fd;
    const char            *file_name;     /* non-owned */
    const char            *function_name; /* non-owned */
    int                    line_number;
    struct p101_fd_record *next;
};

/* Separately allocated so the ledger head stays mutable through a const env. */
struct p101_fd_ledger
{
    struct p101_fd_record *head;
};

struct p101_env_fault_state
{
    unsigned long target_call;
    unsigned long calls_seen;
    int           errnum;
    char         *call_name;
    FILE         *log_stream;
    char         *log_path;
    int           log_owned;
};

struct p101_env_event_state
{
    unsigned long long next_sequence;
};

struct p101_env
{
    p101_env_tracer              tracer;
    p101_env_tracer              exit_tracer;
    void                        *tracer_data; /* non-owned; state for the installed tracer */
    const char                  *label;       /* non-owned */
    p101_env_fault_injector      fault_injector;
    void                        *fault_data; /* non-owned; state for the injector */
    struct p101_fd_ledger       *fd_ledger;  /* NULL unless tracking is enabled */
    p101_env_fd_observer         fd_observer;
    void                        *fd_observer_data; /* non-owned; state for the observer */
    p101_env_alloc_observer      alloc_observer;
    void                        *alloc_observer_data; /* non-owned; state for the observer */
    p101_env_call_observer       call_observer;
    void                        *call_observer_data; /* non-owned; state for the observer */
    unsigned                     call_log_options;
    int                          event_log_version;
    struct p101_env_event_state *event_state;
    FILE                        *owned_fd_log_stream;
    char                        *owned_fd_log_path;
    FILE                        *owned_call_log_stream;
    char                        *owned_call_log_path;
    struct p101_env_fault_state *owned_fault_state;
};

enum
{
    P101_EVENT_LOG_FIXED_SIZE   = 512,
    P101_CALL_LOG_FIELD_COUNT   = 5,
    P101_NUMBER_BUF_LEN         = 64,
    P101_POINTER_BUF_LEN        = 64,
    P101_NANOSECONDS_PER_SECOND = 1000000000,
    P101_ASCII_DELETE           = 0x7F,
    P101_ENV_NUMBER_BASE        = 10,
    P101_EXEC_SCAN_FD_FALLBACK  = 65536,
    P101_EVENT_PARSE_FD_MAX     = 1048576,
    P101_DEFAULT_FAULT_ERRNO    = EIO
};

static void                         p101_env_init(struct p101_env *env, p101_env_tracer tracer);
static void                         p101_env_init_event_state(struct p101_env *env, struct p101_error *err);
static void                         p101_env_configure_from_environment(struct p101_env *env, struct p101_error *err);
static void                         p101_env_configure_event_log_version_from_environment(struct p101_env *env, struct p101_error *err);
static void                         p101_env_configure_fault_from_environment(struct p101_env *env, struct p101_error *err);
static void                         p101_env_configure_fd_log_from_environment(struct p101_env *env, struct p101_error *err);
static void                         p101_env_configure_call_log_from_environment(struct p101_env *env, struct p101_error *err);
static void                         p101_env_configure_resource_log_path(struct p101_env *env, struct p101_error *err, const char *path, int enable_fd, int enable_alloc);
static void                         p101_env_configure_call_log_path(struct p101_env *env, struct p101_error *err, const char *path, unsigned options);
static FILE                        *p101_env_open_log_from_environment(struct p101_error *err, const char *path, int *owned);
static char                        *p101_env_copy_text(struct p101_error *err, const char *text);
static void                         p101_env_close_owned_resource_log(struct p101_env *env);
static void                         p101_env_close_owned_resource_log_if_unused(struct p101_env *env);
static void                         p101_env_close_owned_call_log(struct p101_env *env);
static struct p101_env_fault_state *p101_env_fault_state_dup(struct p101_error *err, const struct p101_env_fault_state *source);
static void                         p101_env_fault_state_destroy(struct p101_env_fault_state *state);
static void                         p101_env_log_fault_hit(const struct p101_env_fault_state *state, const char *call_name);
static int                          p101_env_environment_fault_injector(const struct p101_env *env, const char *call_name, void *user_data);
static unsigned long                p101_env_parse_unsigned_environment(const char *text, unsigned long default_value, int *ok);
static int                          p101_env_parse_int_environment(const char *text, int default_value, int *ok);
static int                          p101_env_flag_on(const char *name, int default_value);
static int                          p101_env_supported_event_log_version(long version);
static int                          p101_env_effective_event_log_version(const struct p101_env *env);
static unsigned long long           p101_env_next_event_sequence(const struct p101_env *env);
static void                         p101_env_timestamp_text(char text[], size_t text_size, clockid_t clock_id);
static void                         p101_env_log_append_event_prefix(const struct p101_env *env, char line[], size_t line_size, size_t *offset, const char *magic, long pid);
static void                         p101_env_fd_notify(const struct p101_env *env, p101_env_fd_event event, int fd, const char *file_name, const char *function_name, int line_number);
static void                         p101_env_fd_log_observer(const struct p101_env *env, p101_env_fd_event event, int fd, const char *file_name, const char *function_name, int line_number, void *user_data);
static void                         p101_env_fork_log(const struct p101_env *env, FILE *stream, long parent_pid, long child_pid, const char *file_name, const char *function_name, int line_number);
static void                         p101_env_spawn_log(const struct p101_env *env, FILE *stream, long parent_pid, long child_pid, const char *target, const char *file_name, const char *function_name, int line_number);
static long                         p101_env_exec_scan_limit(void);
static void                         p101_env_exec_fd_log(const struct p101_env *env, FILE *stream, int fd, int cloexec, const char *target, const char *file_name, const char *function_name, int line_number);
static void                         p101_env_exec_failure_log(const struct p101_env *env, FILE *stream, const char *target, const char *file_name, const char *function_name, int line_number);
static void                         p101_env_alloc_notify(const struct p101_env *env, p101_env_alloc_event event, const void *ptr, const void *new_ptr, size_t size, const char *file_name, const char *function_name, int line_number);
static void                         p101_env_alloc_log_observer(const struct p101_env *env, p101_env_alloc_event event, const void *ptr, const void *new_ptr, size_t size, const char *file_name, const char *function_name, int line_number, void *user_data);
static const char                  *p101_env_alloc_event_name(p101_env_alloc_event event);
static void                         p101_env_call_notify(const struct p101_env *env, p101_env_call_event event, const char *call_name, const char *arguments, const char *result, const char *file_name, const char *function_name, int line_number);
static void   p101_env_call_log_observer(const struct p101_env *env, p101_env_call_event event, const char *call_name, const char *arguments, const char *result, const char *file_name, const char *function_name, int line_number, void *user_data);
static void   p101_env_log_append_char(char line[], size_t line_size, size_t *offset, char ch);
static void   p101_env_log_append_text(char line[], size_t line_size, size_t *offset, const char *text);
static void   p101_env_log_append_field(char line[], size_t line_size, size_t *offset, const char *text);
static size_t p101_env_log_finish_record(char line[], size_t line_size, size_t offset);
static char  *p101_env_log_buffer_create(const char *const fields[], size_t field_count, size_t *line_size);
static void   p101_env_event_unescape_char(char **read_cursor, char **write_cursor);
static int    p101_env_event_parse_long_field(const char *text, long min, long max, long *out);
static int    p101_env_event_parse_optional_size_field(const char *text, size_t *out, int *available);
static p101_env_event_parse_status p101_env_event_parse_version_metadata(char **cursor, struct p101_env_event_record *record);
static p101_env_event_parse_status p101_env_event_parse_fd(char *line, struct p101_env_event_record *record);
static p101_env_event_parse_status p101_env_event_parse_alloc(char *line, struct p101_env_event_record *record);
static p101_env_event_parse_status p101_env_event_parse_fork(char *line, struct p101_env_event_record *record);
static p101_env_event_parse_status p101_env_event_parse_spawn(char *line, struct p101_env_event_record *record);
static p101_env_event_parse_status p101_env_event_parse_exec(char *line, struct p101_env_event_record *record);
static p101_env_event_parse_status p101_env_event_parse_exec_failure(char *line, struct p101_env_event_record *record);
static p101_env_event_parse_status p101_env_event_parse_call(char *line, struct p101_env_event_record *record);

struct p101_env *p101_env_create(struct p101_error *err, p101_env_tracer tracer)
{
    struct p101_env *env;

    env = (struct p101_env *)malloc(sizeof(struct p101_env));    // NOLINT(clang-analyzer-unix.Malloc)

    if(env == NULL)
    {
        P101_ERROR_RAISE_ERRNO(err, errno);
    }
    else
    {
        p101_env_init(env, tracer);
        p101_env_init_event_state(env, err);
    }

    if(env != NULL && p101_error_has_error(err))
    {
        p101_env_destroy(env);
        env = NULL;
    }

    if(env != NULL)
    {
        p101_env_configure_from_environment(env, err);
    }

    if(env != NULL && p101_error_has_error(err))
    {
        p101_env_destroy(env);
        env = NULL;
    }

    return env;
}

struct p101_env *p101_env_dup(struct p101_error *err, const struct p101_env *env)
{
    struct p101_env *new_env;

    if(env == NULL)
    {
        P101_ERROR_RAISE_CHECK(err);

        return NULL;
    }

    new_env = (struct p101_env *)malloc(sizeof(struct p101_env));    // NOLINT(clang-analyzer-unix.Malloc)

    if(new_env == NULL)
    {
        P101_ERROR_RAISE_ERRNO(err, errno);
    }
    else
    {
        /* Copy the behaviour of the source env. The fd ledger is deliberately
         * NOT shared or copied -- it is per-env (see the header). */
        p101_env_init(new_env, env->tracer);
        p101_env_init_event_state(new_env, err);

        if(p101_error_has_error(err))
        {
            p101_env_destroy(new_env);
            return NULL;
        }

        new_env->exit_tracer    = env->exit_tracer;
        new_env->tracer_data    = env->tracer_data;
        new_env->label          = env->label;
        new_env->fault_injector = env->fault_injector;
        new_env->fault_data     = env->fault_data;

        /* The observer IS inherited -- a log is a destination, not state. */
        new_env->fd_observer         = env->fd_observer;
        new_env->fd_observer_data    = env->fd_observer_data;
        new_env->alloc_observer      = env->alloc_observer;
        new_env->alloc_observer_data = env->alloc_observer_data;

        /* The structured call observer is also a destination and is inherited. */
        new_env->call_observer         = env->call_observer;
        new_env->call_observer_data    = env->call_observer_data;
        new_env->call_log_options      = env->call_log_options;
        new_env->event_log_version     = env->event_log_version;
        new_env->owned_fd_log_stream   = NULL;
        new_env->owned_fd_log_path     = NULL;
        new_env->owned_call_log_stream = NULL;
        new_env->owned_call_log_path   = NULL;
        new_env->owned_fault_state     = NULL;

        if(env->owned_fault_state != NULL)
        {
            new_env->owned_fault_state = p101_env_fault_state_dup(err, env->owned_fault_state);
            new_env->fault_injector    = p101_env_environment_fault_injector;
            new_env->fault_data        = new_env->owned_fault_state;
        }

        if(env->owned_fd_log_stream != NULL)
        {
            int enable_fd;
            int enable_alloc;

            enable_fd    = (env->fd_observer == p101_env_fd_log_observer && env->fd_observer_data == env->owned_fd_log_stream) ? 1 : 0;
            enable_alloc = (env->alloc_observer == p101_env_alloc_log_observer && env->alloc_observer_data == env->owned_fd_log_stream) ? 1 : 0;
            p101_env_configure_resource_log_path(new_env, err, env->owned_fd_log_path, enable_fd, enable_alloc);
        }

        if(env->owned_call_log_stream != NULL)
        {
            p101_env_configure_call_log_path(new_env, err, env->owned_call_log_path, env->call_log_options);
        }

        if(p101_error_has_error(err))
        {
            p101_env_destroy(new_env);
            new_env = NULL;
        }
    }

    return new_env;
}

void p101_env_destroy(struct p101_env *env)
{
    if(env != NULL && env->fd_ledger != NULL)
    {
        struct p101_fd_record *cur = env->fd_ledger->head;

        while(cur != NULL)
        {
            struct p101_fd_record *next = cur->next;

            free(cur);
            cur = next;
        }

        free(env->fd_ledger);
    }

    if(env != NULL && env->owned_fd_log_stream != NULL)
    {
        p101_env_close_owned_resource_log(env);
    }

    if(env != NULL && env->owned_call_log_stream != NULL)
    {
        p101_env_close_owned_call_log(env);
    }

    if(env != NULL)
    {
        p101_env_fault_state_destroy(env->owned_fault_state);
        free(env->event_state);
    }

    free(env);
}

static void p101_env_init(struct p101_env *env, p101_env_tracer tracer)
{
    env->tracer                = tracer;
    env->exit_tracer           = NULL;
    env->tracer_data           = NULL;
    env->label                 = NULL;
    env->fault_injector        = NULL;
    env->fault_data            = NULL;
    env->fd_ledger             = NULL;
    env->fd_observer           = NULL;
    env->fd_observer_data      = NULL;
    env->alloc_observer        = NULL;
    env->alloc_observer_data   = NULL;
    env->call_observer         = NULL;
    env->call_observer_data    = NULL;
    env->call_log_options      = P101_ENV_CALL_LOG_DEFAULT;
    env->event_log_version     = P101_ENV_EVENT_LOG_VERSION_2;
    env->event_state           = NULL;
    env->owned_fd_log_stream   = NULL;
    env->owned_fd_log_path     = NULL;
    env->owned_call_log_stream = NULL;
    env->owned_call_log_path   = NULL;
    env->owned_fault_state     = NULL;
}

static void p101_env_init_event_state(struct p101_env *env, struct p101_error *err)
{
    env->event_state = (struct p101_env_event_state *)malloc(sizeof(struct p101_env_event_state));

    if(env->event_state == NULL)
    {
        P101_ERROR_RAISE_ERRNO(err, errno);
        return;
    }

    env->event_state->next_sequence = 0ULL;
}

static void p101_env_configure_from_environment(struct p101_env *env, struct p101_error *err)
{
    p101_env_configure_event_log_version_from_environment(env, err);

    if(p101_error_has_error(err))
    {
        return;
    }

    p101_env_configure_fault_from_environment(env, err);

    if(p101_error_has_error(err))
    {
        return;
    }

    p101_env_configure_fd_log_from_environment(env, err);

    if(p101_error_has_error(err))
    {
        return;
    }

    p101_env_configure_call_log_from_environment(env, err);
}

static void p101_env_configure_event_log_version_from_environment(struct p101_env *env, struct p101_error *err)
{
    const char *version_text;
    int         version;
    int         ok;

    version_text = getenv("P101_EVENT_LOG_VERSION");

    if(version_text == NULL || version_text[0] == '\0')
    {
        return;
    }

    version = p101_env_parse_int_environment(version_text, P101_ENV_EVENT_LOG_VERSION_2, &ok);

    if(!ok || !p101_env_supported_event_log_version(version))
    {
        P101_ERROR_RAISE_ERRNO(err, EINVAL);
        return;
    }

    env->event_log_version = version;
}

static void p101_env_configure_fault_from_environment(struct p101_env *env, struct p101_error *err)
{
    const char                  *target_text;
    const char                  *errnum_text;
    const char                  *log_text;
    struct p101_env_fault_state *state;
    unsigned long                target_call;
    int                          fault_errno;
    int                          log_owned;
    int                          ok;

    target_text = getenv("P101_FAULT_CALL");

    if(target_text == NULL || target_text[0] == '\0')
    {
        return;
    }

    target_call = p101_env_parse_unsigned_environment(target_text, 0, &ok);

    if(!ok || target_call == 0)
    {
        P101_ERROR_RAISE_ERRNO(err, EINVAL);

        return;
    }

    errnum_text = getenv("P101_FAULT_ERRNO");
    fault_errno = p101_env_parse_int_environment(errnum_text, P101_DEFAULT_FAULT_ERRNO, &ok);

    if(!ok || fault_errno <= 0)
    {
        P101_ERROR_RAISE_ERRNO(err, EINVAL);

        return;
    }

    state = (struct p101_env_fault_state *)malloc(sizeof(struct p101_env_fault_state));

    if(state == NULL)
    {
        P101_ERROR_RAISE_ERRNO(err, errno);

        return;
    }

    state->target_call = target_call;
    state->calls_seen  = 0;
    state->errnum      = fault_errno;
    state->call_name   = NULL;
    state->log_stream  = NULL;
    state->log_path    = NULL;
    state->log_owned   = 0;

    target_text = getenv("P101_FAULT_NAME");
    if(target_text != NULL && target_text[0] != '\0')
    {
        state->call_name = p101_env_copy_text(err, target_text);
        if(state->call_name == NULL)
        {
            p101_env_fault_state_destroy(state);
            return;
        }
    }

    log_text = getenv("P101_FAULT_LOG");

    if(log_text != NULL && log_text[0] != '\0')
    {
        state->log_stream = p101_env_open_log_from_environment(err, log_text, &log_owned);

        if(state->log_stream == NULL)
        {
            p101_env_fault_state_destroy(state);
            return;
        }

        state->log_owned = log_owned;
        if(log_owned)
        {
            state->log_path = p101_env_copy_text(err, log_text);
            if(state->log_path == NULL)
            {
                p101_env_fault_state_destroy(state);
                return;
            }
        }
    }

    env->fault_injector    = p101_env_environment_fault_injector;
    env->fault_data        = state;
    env->owned_fault_state = state;
}

static void p101_env_configure_fd_log_from_environment(struct p101_env *env, struct p101_error *err)
{
    const char *path;

    path = getenv("P101_RESOURCE_LOG");

    if(path == NULL || path[0] == '\0')
    {
        return;
    }

    p101_env_configure_resource_log_path(env, err, path, 1, 1);
}

static void p101_env_configure_call_log_from_environment(struct p101_env *env, struct p101_error *err)
{
    const char *path;
    unsigned    options;

    path = getenv("P101_CALL_LOG");

    if(path == NULL || path[0] == '\0')
    {
        return;
    }

    options = 0;

    if(p101_env_flag_on("P101_CALL_LOG_ENTER", 1))
    {
        options |= P101_ENV_CALL_LOG_ENTER;
    }

    if(p101_env_flag_on("P101_CALL_LOG_EXIT", 1))
    {
        options |= P101_ENV_CALL_LOG_EXIT;
    }

    if(p101_env_flag_on("P101_CALL_LOG_ARGS", 0))
    {
        options |= P101_ENV_CALL_LOG_ARGUMENTS;
    }

    if(p101_env_flag_on("P101_CALL_LOG_RESULT", 0))
    {
        options |= P101_ENV_CALL_LOG_RESULT;
    }

    p101_env_configure_call_log_path(env, err, path, options);
}

static void p101_env_configure_resource_log_path(struct p101_env *env, struct p101_error *err, const char *path, int enable_fd, int enable_alloc)
{
    FILE *stream;
    char *path_copy;
    int   owned;

    if(path == NULL || (!enable_fd && !enable_alloc))
    {
        return;
    }

    stream = p101_env_open_log_from_environment(err, path, &owned);
    if(stream == NULL)
    {
        return;
    }

    path_copy = NULL;
    if(owned)
    {
        path_copy = p101_env_copy_text(err, path);
        if(path_copy == NULL)
        {
            fclose(stream);    // NOLINT(cert-err33-c)
            return;
        }
        setvbuf(stream, NULL, _IOLBF, 0);    // NOLINT(cert-err33-c)
        env->owned_fd_log_stream = stream;
        env->owned_fd_log_path   = path_copy;
    }

    if(enable_fd)
    {
        env->fd_observer      = p101_env_fd_log_observer;
        env->fd_observer_data = stream;
    }

    if(enable_alloc)
    {
        env->alloc_observer      = p101_env_alloc_log_observer;
        env->alloc_observer_data = stream;
    }
}

static void p101_env_configure_call_log_path(struct p101_env *env, struct p101_error *err, const char *path, unsigned options)
{
    FILE *stream;
    char *path_copy;
    int   owned;

    if(path == NULL)
    {
        return;
    }

    stream = p101_env_open_log_from_environment(err, path, &owned);
    if(stream == NULL)
    {
        return;
    }

    path_copy = NULL;
    if(owned)
    {
        path_copy = p101_env_copy_text(err, path);
        if(path_copy == NULL)
        {
            fclose(stream);    // NOLINT(cert-err33-c)
            return;
        }
        setvbuf(stream, NULL, _IOLBF, 0);    // NOLINT(cert-err33-c)
        env->owned_call_log_stream = stream;
        env->owned_call_log_path   = path_copy;
    }

    env->call_observer      = p101_env_call_log_observer;
    env->call_observer_data = stream;
    env->call_log_options   = options;
}

static FILE *p101_env_open_log_from_environment(struct p101_error *err, const char *path, int *owned)
{
    FILE *stream;

    *owned = 0;

    if(strcmp(path, "-") == 0)
    {
        return stderr;
    }

    stream = fopen(path, "ae");

    if(stream == NULL)
    {
        P101_ERROR_RAISE_ERRNO(err, errno);

        return NULL;
    }

    *owned = 1;

    return stream;
}

static char *p101_env_copy_text(struct p101_error *err, const char *text)
{
    char  *copy;
    size_t length;

    length = strlen(text);
    copy   = (char *)malloc(length + 1U);
    if(copy == NULL)
    {
        P101_ERROR_RAISE_ERRNO(err, errno);
        return NULL;
    }

    memcpy(copy, text, length + 1U);
    return copy;
}

static void p101_env_close_owned_resource_log(struct p101_env *env)
{
    FILE *stream;

    if(env == NULL || env->owned_fd_log_stream == NULL)
    {
        return;
    }

    stream = env->owned_fd_log_stream;

    if(env->fd_observer_data == stream)
    {
        env->fd_observer      = NULL;
        env->fd_observer_data = NULL;
    }

    if(env->alloc_observer_data == stream)
    {
        env->alloc_observer      = NULL;
        env->alloc_observer_data = NULL;
    }

    fclose(stream);    // NOLINT(cert-err33-c)
    env->owned_fd_log_stream = NULL;
    free(env->owned_fd_log_path);
    env->owned_fd_log_path = NULL;
}

static void p101_env_close_owned_resource_log_if_unused(struct p101_env *env)
{
    FILE *stream;

    if(env == NULL || env->owned_fd_log_stream == NULL)
    {
        return;
    }

    stream = env->owned_fd_log_stream;
    if(env->fd_observer_data == stream || env->alloc_observer_data == stream)
    {
        return;
    }

    fclose(stream);    // NOLINT(cert-err33-c)
    env->owned_fd_log_stream = NULL;
    free(env->owned_fd_log_path);
    env->owned_fd_log_path = NULL;
}

static void p101_env_close_owned_call_log(struct p101_env *env)
{
    FILE *stream;

    if(env == NULL || env->owned_call_log_stream == NULL)
    {
        return;
    }

    stream = env->owned_call_log_stream;
    if(env->call_observer_data == stream)
    {
        env->call_observer      = NULL;
        env->call_observer_data = NULL;
    }

    fclose(stream);    // NOLINT(cert-err33-c)
    env->owned_call_log_stream = NULL;
    free(env->owned_call_log_path);
    env->owned_call_log_path = NULL;
}

static struct p101_env_fault_state *p101_env_fault_state_dup(struct p101_error *err, const struct p101_env_fault_state *source)
{
    struct p101_env_fault_state *state;
    int                          log_owned;

    if(source == NULL)
    {
        return NULL;
    }

    state = (struct p101_env_fault_state *)malloc(sizeof(struct p101_env_fault_state));
    if(state == NULL)
    {
        P101_ERROR_RAISE_ERRNO(err, errno);
        return NULL;
    }

    state->target_call = source->target_call;
    state->calls_seen  = 0;
    state->errnum      = source->errnum;
    state->call_name   = NULL;
    state->log_stream  = NULL;
    state->log_path    = NULL;
    state->log_owned   = 0;

    if(source->call_name != NULL)
    {
        state->call_name = p101_env_copy_text(err, source->call_name);
        if(state->call_name == NULL)
        {
            p101_env_fault_state_destroy(state);
            return NULL;
        }
    }

    if(source->log_stream != NULL && source->log_owned)
    {
        if(source->log_path == NULL)
        {
            P101_ERROR_RAISE_CHECK(err);
            p101_env_fault_state_destroy(state);
            return NULL;
        }

        state->log_stream = p101_env_open_log_from_environment(err, source->log_path, &log_owned);
        if(state->log_stream == NULL)
        {
            p101_env_fault_state_destroy(state);
            return NULL;
        }
        state->log_owned = log_owned;
        state->log_path  = p101_env_copy_text(err, source->log_path);
        if(state->log_path == NULL)
        {
            p101_env_fault_state_destroy(state);
            return NULL;
        }
    }
    else
    {
        state->log_stream = source->log_stream;
    }

    return state;
}

static void p101_env_fault_state_destroy(struct p101_env_fault_state *state)
{
    if(state == NULL)
    {
        return;
    }

    if(state->log_owned && state->log_stream != NULL)
    {
        fclose(state->log_stream);    // NOLINT(cert-err33-c)
    }

    free(state->call_name);
    free(state->log_path);
    free(state);
}

static void p101_env_log_fault_hit(const struct p101_env_fault_state *state, const char *call_name)
{
    if(state == NULL || state->log_stream == NULL)
    {
        return;
    }

    fprintf(state->log_stream, "P101FAULT\t1\t%" PRIdMAX "\t%lu\t%s\t%d\n", (intmax_t)getpid(), state->calls_seen, (call_name == NULL) ? "?" : call_name, state->errnum);    // NOLINT(cert-err33-c)
    fflush(state->log_stream);                                                                                                                                               // NOLINT(cert-err33-c)
}

static int p101_env_environment_fault_injector(const struct p101_env *env, const char *call_name, void *user_data)
{
    struct p101_env_fault_state *state;

    (void)env;
    state = (struct p101_env_fault_state *)user_data;

    if(state == NULL || state->target_call == 0)
    {
        return 0;
    }

    if(state->call_name != NULL)
    {
        if(call_name == NULL || strcmp(state->call_name, call_name) != 0)
        {
            return 0;
        }
    }

    state->calls_seen++;

    if(state->calls_seen == state->target_call)
    {
        p101_env_log_fault_hit(state, call_name);
        return state->errnum;
    }

    return 0;
}

static unsigned long p101_env_parse_unsigned_environment(const char *text, unsigned long default_value, int *ok)
{
    char         *end;
    unsigned long value;

    *ok = 1;

    if(text == NULL || text[0] == '\0')
    {
        return default_value;
    }

    if(text[0] == '-')
    {
        *ok = 0;

        return default_value;
    }

    errno = 0;
    value = strtoul(text, &end, P101_ENV_NUMBER_BASE);

    if(errno == ERANGE || end == text || *end != '\0')
    {
        *ok = 0;

        return default_value;
    }

    return value;
}

static int p101_env_parse_int_environment(const char *text, int default_value, int *ok)
{
    unsigned long value;

    value = p101_env_parse_unsigned_environment(text, (unsigned long)default_value, ok);

    if(!*ok || value > (unsigned long)INT_MAX)
    {
        *ok = 0;

        return default_value;
    }

    return (int)value;
}

static int p101_env_flag_on(const char *name, int default_value)
{
    const char *value;

    value = getenv(name);

    if(value == NULL || value[0] == '\0')
    {
        return default_value;
    }

    if(strcmp(value, "0") == 0 || strcmp(value, "off") == 0 || strcmp(value, "false") == 0 || strcmp(value, "no") == 0)
    {
        return 0;
    }

    return 1;
}

p101_env_tracer p101_env_get_tracer(const struct p101_env *env)
{
    p101_env_trace(env, __FILE__, __func__, __LINE__);

    if(env == NULL)
    {
        return NULL;
    }

    return env->tracer;
}

void p101_env_set_tracer(struct p101_env *env, p101_env_tracer tracer)
{
    p101_env_trace(env, __FILE__, __func__, __LINE__);

    if(env == NULL)
    {
        return;
    }

    env->tracer = tracer;
}

void p101_env_set_tracer_data(struct p101_env *env, void *data)
{
    p101_env_trace(env, __FILE__, __func__, __LINE__);

    if(env == NULL)
    {
        return;
    }

    env->tracer_data = data;
}

void *p101_env_get_tracer_data(const struct p101_env *env)
{
    /* Deliberately not traced: tracers call this for their state, and tracing
     * here would recurse into the tracer. */
    if(env == NULL)
    {
        return NULL;
    }

    return env->tracer_data;
}

void p101_env_set_exit_tracer(struct p101_env *env, p101_env_tracer tracer)
{
    p101_env_trace(env, __FILE__, __func__, __LINE__);

    if(env == NULL)
    {
        return;
    }

    env->exit_tracer = tracer;
}

p101_env_tracer p101_env_get_exit_tracer(const struct p101_env *env)
{
    p101_env_trace(env, __FILE__, __func__, __LINE__);

    if(env == NULL)
    {
        return NULL;
    }

    return env->exit_tracer;
}

void p101_env_trace_exit(const struct p101_env *env, const char *file_name, const char *function_name, int line_number)
{
    if(env != NULL && env->exit_tracer != NULL)
    {
        env->exit_tracer(env, file_name, function_name, line_number);
    }

    p101_env_call_notify(env, P101_ENV_CALL_EXIT, function_name, NULL, NULL, file_name, function_name, line_number);
}

void p101_env_set_label(struct p101_env *env, const char *label)
{
    p101_env_trace(env, __FILE__, __func__, __LINE__);

    if(env == NULL)
    {
        return;
    }

    env->label = label;
}

const char *p101_env_get_label(const struct p101_env *env)
{
    /* Not traced: the default tracer calls this, and tracing would recurse. */
    if(env == NULL)
    {
        return NULL;
    }

    return env->label;
}

void p101_env_default_tracer(const struct p101_env *env, const char *file_name, const char *function_name, int line_number)
{
    const char *label;
    const char *reported_file;
    const char *reported_function;

    label             = p101_env_get_label(env);
    reported_file     = (file_name == NULL) ? "?" : file_name;
    reported_function = (function_name == NULL) ? "?" : function_name;

    if(label != NULL)
    {
        fprintf(stdout, "TRACE (pid=%" PRIdMAX ", %s): %s : %s : @ %d\n", (intmax_t)getpid(), label, reported_file, reported_function, line_number);    // NOLINT(cert-err33-c)
    }
    else
    {
        fprintf(stdout, "TRACE (pid=%" PRIdMAX "): %s : %s : @ %d\n", (intmax_t)getpid(), reported_file, reported_function, line_number);    // NOLINT(cert-err33-c)
    }
}

void p101_env_trace(const struct p101_env *env, const char *file_name, const char *function_name, int line_number)
{
    if(env != NULL && env->tracer != NULL)
    {
        env->tracer(env, file_name, function_name, line_number);
    }

    p101_env_call_notify(env, P101_ENV_CALL_ENTER, function_name, NULL, NULL, file_name, function_name, line_number);
}

/* cppcheck-suppress funcArgNamesDifferentUnnamed */
void p101_env_set_call_observer(struct p101_env *env, p101_env_call_observer observer, void *user_data)
{
    p101_env_trace(env, __FILE__, __func__, __LINE__);

    if(env == NULL)
    {
        return;
    }

    if(env->owned_call_log_stream != NULL)
    {
        p101_env_close_owned_call_log(env);
    }

    env->call_observer      = observer;
    env->call_observer_data = user_data;
    env->call_log_options   = P101_ENV_CALL_LOG_DEFAULT;
}

void p101_env_set_call_log(struct p101_env *env, FILE *stream, unsigned options)
{
    p101_env_trace(env, __FILE__, __func__, __LINE__);

    if(env == NULL)
    {
        return;
    }

    if(env->owned_call_log_stream != NULL)
    {
        p101_env_close_owned_call_log(env);
    }

    if(stream == NULL)
    {
        env->call_observer      = NULL;
        env->call_observer_data = NULL;
        env->call_log_options   = P101_ENV_CALL_LOG_DEFAULT;

        return;
    }

    env->call_observer      = p101_env_call_log_observer;
    env->call_observer_data = stream;
    env->call_log_options   = options;
}

void p101_env_trace_call(const struct p101_env *env, const char *call_name, const char *arguments, const char *file_name, const char *function_name, int line_number)
{
    if(env != NULL && env->tracer != NULL)
    {
        env->tracer(env, file_name, function_name, line_number);
    }

    p101_env_call_notify(env, P101_ENV_CALL_ENTER, call_name, arguments, NULL, file_name, function_name, line_number);
}

void p101_env_trace_call_exit(const struct p101_env *env, const char *call_name, const char *result, const char *file_name, const char *function_name, int line_number)
{
    if(env != NULL && env->exit_tracer != NULL)
    {
        env->exit_tracer(env, file_name, function_name, line_number);
    }

    p101_env_call_notify(env, P101_ENV_CALL_EXIT, call_name, NULL, result, file_name, function_name, line_number);
}

p101_env_event_line_status p101_env_read_event_line(struct p101_error *err, FILE *stream, char *line, size_t line_size)
{
    bool   saw_byte;
    bool   malformed;
    size_t length;

    if(line != NULL && line_size > 0U)
    {
        line[0] = '\0';
    }

    if(stream == NULL || line == NULL || line_size == 0U)
    {
        P101_ERROR_RAISE_CHECK(err);

        return P101_ENV_EVENT_LINE_ERROR;
    }

    saw_byte  = false;
    malformed = false;
    length    = 0U;

    while(true)
    {
        int ch;

        ch = fgetc(stream);

        if(ch == EOF)
        {
            if(ferror(stream) != 0)
            {
                line[length] = '\0';
                P101_ERROR_RAISE_ERRNO(err, errno == 0 ? EIO : errno);

                return P101_ENV_EVENT_LINE_ERROR;
            }

            break;
        }

        saw_byte = true;

        if(ch == '\0')
        {
            malformed = true;
        }

        if(length + 1U < line_size)
        {
            line[length] = (char)ch;
            length++;
        }
        else
        {
            malformed = true;
        }

        if(ch == '\n')
        {
            break;
        }
    }

    if(!saw_byte)
    {
        return P101_ENV_EVENT_LINE_EOF;
    }

    line[length] = '\0';

    if(malformed)
    {
        return P101_ENV_EVENT_LINE_MALFORMED;
    }

    return P101_ENV_EVENT_LINE_OK;
}

p101_env_event_parse_status p101_env_parse_event_line(char *line, struct p101_env_event_record *record)
{
    p101_env_event_parse_status status;
    size_t                      length;

    status = P101_ENV_EVENT_PARSE_MALFORMED;

    if(line == NULL || record == NULL)
    {
        goto done;
    }

    if(!p101_env_event_line_is_ours(line))
    {
        status = P101_ENV_EVENT_PARSE_OTHER;
        goto done;
    }

    memset(record, 0, sizeof(*record));
    record->fd          = -1;
    record->child_pid   = -1;
    record->line_number = -1;

    length = strlen(line);
    while(length > 0U && (line[length - 1U] == '\n' || line[length - 1U] == '\r'))
    {
        length--;
        line[length] = '\0';
    }

    status = p101_env_event_parse_fd(line, record);
    if(status != P101_ENV_EVENT_PARSE_OTHER)
    {
        goto done;
    }

    status = p101_env_event_parse_alloc(line, record);
    if(status != P101_ENV_EVENT_PARSE_OTHER)
    {
        goto done;
    }

    status = p101_env_event_parse_fork(line, record);
    if(status != P101_ENV_EVENT_PARSE_OTHER)
    {
        goto done;
    }

    status = p101_env_event_parse_spawn(line, record);
    if(status != P101_ENV_EVENT_PARSE_OTHER)
    {
        goto done;
    }

    status = p101_env_event_parse_exec(line, record);
    if(status != P101_ENV_EVENT_PARSE_OTHER)
    {
        goto done;
    }

    status = p101_env_event_parse_exec_failure(line, record);
    if(status != P101_ENV_EVENT_PARSE_OTHER)
    {
        goto done;
    }

    status = p101_env_event_parse_call(line, record);

done:
    return status;
}

int p101_env_event_line_is_ours(const char *line)
{
    int result;

    result = 0;

    if(line == NULL)
    {
        goto done;
    }

    if(strncmp(line, "P101FD\t", strlen("P101FD\t")) == 0)
    {
        result = 1;
        goto done;
    }

    if(strncmp(line, "P101ALLOC\t", strlen("P101ALLOC\t")) == 0)
    {
        result = 1;
        goto done;
    }

    if(strncmp(line, "P101FORK\t", strlen("P101FORK\t")) == 0)
    {
        result = 1;
        goto done;
    }

    if(strncmp(line, "P101SPAWN\t", strlen("P101SPAWN\t")) == 0)
    {
        result = 1;
        goto done;
    }

    if(strncmp(line, "P101EXEC\t", strlen("P101EXEC\t")) == 0)
    {
        result = 1;
        goto done;
    }

    if(strncmp(line, "P101EXECFAIL\t", strlen("P101EXECFAIL\t")) == 0)
    {
        result = 1;
        goto done;
    }

    if(strncmp(line, "P101CALL\t", strlen("P101CALL\t")) == 0)
    {
        result = 1;
        goto done;
    }

done:
    return result;
}

const char *p101_env_event_parse_status_name(p101_env_event_parse_status status)
{
    const char *name;

#ifdef __clang__
    #pragma clang diagnostic push
    #pragma clang diagnostic ignored "-Wcovered-switch-default"
#endif
    switch(status)
    {
        case P101_ENV_EVENT_PARSE_OTHER:
        {
            name = "not a p101 event record";
            break;
        }
        case P101_ENV_EVENT_PARSE_OK:
        {
            name = "ok";
            break;
        }
        case P101_ENV_EVENT_PARSE_MALFORMED:
        {
            name = "malformed record";
            break;
        }
        case P101_ENV_EVENT_PARSE_BAD_VERSION:
        {
            name = "unsupported record version";
            break;
        }
        default:
        {
            name = "unknown event parse status";
            break;
        }
    }
#ifdef __clang__
    #pragma clang diagnostic pop
#endif

    return name;
}

int p101_env_set_event_log_version(struct p101_env *env, int version)
{
    int result;

    p101_env_trace(env, __FILE__, __func__, __LINE__);

    result = EINVAL;

    if(env != NULL && p101_env_supported_event_log_version(version))
    {
        env->event_log_version = version;
        result                 = 0;
    }

    return result;
}

int p101_env_get_event_log_version(const struct p101_env *env)
{
    int version;

    p101_env_trace(env, __FILE__, __func__, __LINE__);

    version = P101_ENV_EVENT_LOG_VERSION_2;

    if(env != NULL)
    {
        version = env->event_log_version;
    }

    return version;
}

/* cppcheck-suppress funcArgNamesDifferentUnnamed */
void p101_env_set_fault_injector(struct p101_env *env, p101_env_fault_injector injector, void *user_data)
{
    p101_env_trace(env, __FILE__, __func__, __LINE__);

    if(env == NULL)
    {
        return;
    }

    p101_env_fault_state_destroy(env->owned_fault_state);
    env->owned_fault_state = NULL;
    env->fault_injector    = injector;
    env->fault_data        = user_data;
}

int p101_env_check_fault(const struct p101_env *env, const char *call_name)
{
    /* Not traced: this runs inside every wrapper, right after its own
     * P101_TRACE, so tracing here would double every entry. */
    if(env == NULL || env->fault_injector == NULL)
    {
        return 0;
    }

    return env->fault_injector(env, call_name, env->fault_data);
}

void p101_env_enable_fd_tracking(struct p101_env *env, struct p101_error *err)
{
    struct p101_fd_ledger *ledger;

    p101_env_trace(env, __FILE__, __func__, __LINE__);

    if(env == NULL || env->fd_ledger != NULL)
    {
        return;
    }

    ledger = (struct p101_fd_ledger *)malloc(sizeof(struct p101_fd_ledger));

    if(ledger == NULL)
    {
        P101_ERROR_RAISE_ERRNO(err, errno);

        return;
    }

    ledger->head   = NULL;
    env->fd_ledger = ledger;
}

/* cppcheck-suppress funcArgNamesDifferentUnnamed */
void p101_env_set_fd_observer(struct p101_env *env, p101_env_fd_observer observer, void *user_data)
{
    p101_env_trace(env, __FILE__, __func__, __LINE__);

    if(env == NULL)
    {
        return;
    }

    env->fd_observer      = observer;
    env->fd_observer_data = user_data;
    p101_env_close_owned_resource_log_if_unused(env);
}

void p101_env_set_fd_log(struct p101_env *env, FILE *stream)
{
    p101_env_trace(env, __FILE__, __func__, __LINE__);

    if(env == NULL)
    {
        return;
    }

    if(stream == NULL)
    {
        env->fd_observer      = NULL;
        env->fd_observer_data = NULL;
        p101_env_close_owned_resource_log_if_unused(env);

        return;
    }

    env->fd_observer      = p101_env_fd_log_observer;
    env->fd_observer_data = stream;
    p101_env_close_owned_resource_log_if_unused(env);
}

/* cppcheck-suppress funcArgNamesDifferentUnnamed */
void p101_env_set_alloc_observer(struct p101_env *env, p101_env_alloc_observer observer, void *user_data)
{
    p101_env_trace(env, __FILE__, __func__, __LINE__);

    if(env == NULL)
    {
        return;
    }

    env->alloc_observer      = observer;
    env->alloc_observer_data = user_data;
    p101_env_close_owned_resource_log_if_unused(env);
}

void p101_env_set_alloc_log(struct p101_env *env, FILE *stream)
{
    p101_env_trace(env, __FILE__, __func__, __LINE__);

    if(env == NULL)
    {
        return;
    }

    if(stream == NULL)
    {
        env->alloc_observer      = NULL;
        env->alloc_observer_data = NULL;
        p101_env_close_owned_resource_log_if_unused(env);

        return;
    }

    env->alloc_observer      = p101_env_alloc_log_observer;
    env->alloc_observer_data = stream;
    p101_env_close_owned_resource_log_if_unused(env);
}

static int p101_env_supported_event_log_version(long version)
{
    return (version == P101_ENV_EVENT_LOG_VERSION_2) ? 1 : 0;
}

static int p101_env_effective_event_log_version(const struct p101_env *env)
{
    if(env == NULL)
    {
        return P101_ENV_EVENT_LOG_VERSION_2;
    }

    return env->event_log_version;
}

static unsigned long long p101_env_next_event_sequence(const struct p101_env *env)
{
    unsigned long long sequence;

    sequence = 0ULL;

    if(env != NULL && env->event_state != NULL)
    {
        env->event_state->next_sequence++;
        sequence = env->event_state->next_sequence;
    }

    return sequence;
}

static void p101_env_timestamp_text(char text[], size_t text_size, clockid_t clock_id)
{
    struct timespec    ts;
    char               formatted[P101_NUMBER_BUF_LEN];
    unsigned long long seconds;
    unsigned long long nanoseconds;
    size_t             index;

    if(text_size == 0U)
    {
        return;
    }
    text[0] = '\0';

    if(clock_gettime(clock_id, &ts) != 0)
    {
        if(text_size > 1U)
        {
            text[0] = '-';
            text[1] = '\0';
        }
        return;
    }

    seconds     = (unsigned long long)ts.tv_sec;
    nanoseconds = (seconds * P101_NANOSECONDS_PER_SECOND) + (unsigned long long)ts.tv_nsec;
    snprintf(formatted, sizeof(formatted), "%llu", nanoseconds);    // NOLINT(cert-err33-c)

    index = 0U;
    while((index + 1U) < text_size && formatted[index] != '\0')
    {
        text[index] = formatted[index];
        index++;
    }
    text[index] = '\0';
}

static void p101_env_log_append_event_prefix(const struct p101_env *env, char line[], size_t line_size, size_t *offset, const char *magic, long pid)
{
    char version[P101_NUMBER_BUF_LEN];
    char number[P101_NUMBER_BUF_LEN];
    int  log_version;

    log_version = p101_env_effective_event_log_version(env);

    p101_env_log_append_text(line, line_size, offset, magic);
    p101_env_log_append_char(line, line_size, offset, '\t');
    snprintf(version, sizeof(version), "%d", log_version);    // NOLINT(cert-err33-c)
    p101_env_log_append_text(line, line_size, offset, version);
    p101_env_log_append_char(line, line_size, offset, '\t');
    snprintf(number, sizeof(number), "%ld", pid);    // NOLINT(cert-err33-c)
    p101_env_log_append_text(line, line_size, offset, number);
    p101_env_log_append_char(line, line_size, offset, '\t');

    if(log_version == P101_ENV_EVENT_LOG_VERSION_2)
    {
        unsigned long long sequence;
        char               monotonic[P101_NUMBER_BUF_LEN];
        char               wall[P101_NUMBER_BUF_LEN];

        sequence = p101_env_next_event_sequence(env);
        snprintf(number, sizeof(number), "%llu", sequence);    // NOLINT(cert-err33-c)
        p101_env_log_append_text(line, line_size, offset, number);
        p101_env_log_append_char(line, line_size, offset, '\t');
        p101_env_timestamp_text(monotonic, sizeof(monotonic), CLOCK_MONOTONIC);
        p101_env_log_append_text(line, line_size, offset, monotonic);
        p101_env_log_append_char(line, line_size, offset, '\t');
        p101_env_timestamp_text(wall, sizeof(wall), CLOCK_REALTIME);
        p101_env_log_append_text(line, line_size, offset, wall);
        p101_env_log_append_char(line, line_size, offset, '\t');
    }
}

static void p101_env_fd_log_observer(const struct p101_env *env, p101_env_fd_event event, int fd, const char *file_name, const char *function_name, int line_number, void *user_data)
{
    const char *fields[2];
    FILE       *stream;
    char       *line;
    char        number[P101_NUMBER_BUF_LEN];
    size_t      line_size;
    size_t      offset;
    size_t      length;

    stream = (FILE *)user_data;

    if(stream == NULL)
    {
        return;
    }

    fields[0] = function_name;
    fields[1] = file_name;
    line      = p101_env_log_buffer_create(fields, 2U, &line_size);
    if(line == NULL)
    {
        return;
    }

    /* getpid() on every line rather than once at install time: after a fork
     * the child keeps writing to the same stream, and the analyzer has to be
     * able to tell the two processes' descriptors apart. */
    offset  = 0;
    line[0] = '\0';
    p101_env_log_append_event_prefix(env, line, line_size, &offset, "P101FD", (long)getpid());
    p101_env_log_append_text(line, line_size, &offset, (event == P101_ENV_FD_OPEN) ? "OPEN" : "CLOSE");
    p101_env_log_append_char(line, line_size, &offset, '\t');
    snprintf(number, sizeof(number), "%d", fd);    // NOLINT(cert-err33-c)
    p101_env_log_append_text(line, line_size, &offset, number);
    p101_env_log_append_char(line, line_size, &offset, '\t');
    snprintf(number, sizeof(number), "%d", line_number);    // NOLINT(cert-err33-c)
    p101_env_log_append_text(line, line_size, &offset, number);
    p101_env_log_append_char(line, line_size, &offset, '\t');
    p101_env_log_append_field(line, line_size, &offset, function_name);
    p101_env_log_append_char(line, line_size, &offset, '\t');
    p101_env_log_append_field(line, line_size, &offset, file_name);
    p101_env_log_append_char(line, line_size, &offset, '\n');
    length = p101_env_log_finish_record(line, line_size, offset);

    fwrite(line, 1, length, stream);    // NOLINT(cert-err33-c)
    fflush(stream);                     // NOLINT(cert-err33-c)
    free(line);
}

static void p101_env_fork_log(const struct p101_env *env, FILE *stream, long parent_pid, long child_pid, const char *file_name, const char *function_name, int line_number)
{
    const char *fields[2];
    char       *line;
    char        number[P101_NUMBER_BUF_LEN];
    size_t      line_size;
    size_t      offset;
    size_t      length;

    if(stream == NULL)
    {
        return;
    }

    fields[0] = function_name;
    fields[1] = file_name;
    line      = p101_env_log_buffer_create(fields, 2U, &line_size);
    if(line == NULL)
    {
        return;
    }

    offset  = 0;
    line[0] = '\0';
    p101_env_log_append_event_prefix(env, line, line_size, &offset, "P101FORK", parent_pid);
    snprintf(number, sizeof(number), "%ld", child_pid);    // NOLINT(cert-err33-c)
    p101_env_log_append_text(line, line_size, &offset, number);
    p101_env_log_append_char(line, line_size, &offset, '\t');
    snprintf(number, sizeof(number), "%d", line_number);    // NOLINT(cert-err33-c)
    p101_env_log_append_text(line, line_size, &offset, number);
    p101_env_log_append_char(line, line_size, &offset, '\t');
    p101_env_log_append_field(line, line_size, &offset, function_name);
    p101_env_log_append_char(line, line_size, &offset, '\t');
    p101_env_log_append_field(line, line_size, &offset, file_name);
    p101_env_log_append_char(line, line_size, &offset, '\n');
    length = p101_env_log_finish_record(line, line_size, offset);

    fwrite(line, 1, length, stream);    // NOLINT(cert-err33-c)
    fflush(stream);                     // NOLINT(cert-err33-c)
    free(line);
}

static void p101_env_spawn_log(const struct p101_env *env, FILE *stream, long parent_pid, long child_pid, const char *target, const char *file_name, const char *function_name, int line_number)
{
    const char *fields[3];
    char       *line;
    char        number[P101_NUMBER_BUF_LEN];
    size_t      line_size;
    size_t      offset;

    if(stream == NULL)
    {
        return;
    }

    fields[0] = function_name;
    fields[1] = file_name;
    fields[2] = target;
    line      = p101_env_log_buffer_create(fields, 3U, &line_size);
    if(line == NULL)
    {
        return;
    }

    offset  = 0;
    line[0] = '\0';
    p101_env_log_append_event_prefix(env, line, line_size, &offset, "P101SPAWN", parent_pid);
    snprintf(number, sizeof(number), "%ld", child_pid);    // NOLINT(cert-err33-c)
    p101_env_log_append_text(line, line_size, &offset, number);
    p101_env_log_append_char(line, line_size, &offset, '\t');
    snprintf(number, sizeof(number), "%d", line_number);    // NOLINT(cert-err33-c)
    p101_env_log_append_text(line, line_size, &offset, number);
    p101_env_log_append_char(line, line_size, &offset, '\t');
    p101_env_log_append_field(line, line_size, &offset, function_name);
    p101_env_log_append_char(line, line_size, &offset, '\t');
    p101_env_log_append_field(line, line_size, &offset, file_name);
    p101_env_log_append_char(line, line_size, &offset, '\t');
    p101_env_log_append_field(line, line_size, &offset, target);
    p101_env_log_append_char(line, line_size, &offset, '\n');
    offset = p101_env_log_finish_record(line, line_size, offset);

    fwrite(line, 1, offset, stream);    // NOLINT(cert-err33-c)
    fflush(stream);                     // NOLINT(cert-err33-c)
    free(line);
}

static long p101_env_exec_scan_limit(void)
{
    long          limit;
    struct rlimit rl;

    limit = -1;

    if(getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY)
    {
        if(rl.rlim_cur > (rlim_t)INT_MAX)
        {
            limit = INT_MAX;
        }
        else
        {
            limit = (long)rl.rlim_cur;
        }
    }

    if(limit < 0)
    {
        long open_max;

        open_max = sysconf(_SC_OPEN_MAX);

        if(open_max > 0)
        {
            limit = open_max;
        }
    }

    if(limit < 0)
    {
        limit = P101_EXEC_SCAN_FD_FALLBACK;
    }

    return limit;
}

static void p101_env_exec_fd_log(const struct p101_env *env, FILE *stream, int fd, int cloexec, const char *target, const char *file_name, const char *function_name, int line_number)
{
    const char *fields[3];
    char       *line;
    char        number[P101_NUMBER_BUF_LEN];
    size_t      line_size;
    size_t      offset;

    if(stream == NULL)
    {
        return;
    }

    fields[0] = function_name;
    fields[1] = file_name;
    fields[2] = target;
    line      = p101_env_log_buffer_create(fields, 3U, &line_size);
    if(line == NULL)
    {
        return;
    }

    offset  = 0;
    line[0] = '\0';

    p101_env_log_append_event_prefix(env, line, line_size, &offset, "P101EXEC", (long)getpid());
    snprintf(number, sizeof(number), "%d", fd);    // NOLINT(cert-err33-c)
    p101_env_log_append_text(line, line_size, &offset, number);
    p101_env_log_append_char(line, line_size, &offset, '\t');
    snprintf(number, sizeof(number), "%d", cloexec);    // NOLINT(cert-err33-c)
    p101_env_log_append_text(line, line_size, &offset, number);
    p101_env_log_append_char(line, line_size, &offset, '\t');
    snprintf(number, sizeof(number), "%d", line_number);    // NOLINT(cert-err33-c)
    p101_env_log_append_text(line, line_size, &offset, number);
    p101_env_log_append_char(line, line_size, &offset, '\t');
    p101_env_log_append_field(line, line_size, &offset, function_name);
    p101_env_log_append_char(line, line_size, &offset, '\t');
    p101_env_log_append_field(line, line_size, &offset, file_name);
    p101_env_log_append_char(line, line_size, &offset, '\t');
    p101_env_log_append_field(line, line_size, &offset, target);
    p101_env_log_append_char(line, line_size, &offset, '\n');
    offset = p101_env_log_finish_record(line, line_size, offset);

    fwrite(line, 1, offset, stream);    // NOLINT(cert-err33-c)
    fflush(stream);                     // NOLINT(cert-err33-c)
    free(line);
}

static void p101_env_exec_failure_log(const struct p101_env *env, FILE *stream, const char *target, const char *file_name, const char *function_name, int line_number)
{
    const char *fields[3];
    char       *line;
    char        number[P101_NUMBER_BUF_LEN];
    size_t      line_size;
    size_t      offset;

    if(stream == NULL)
    {
        return;
    }

    fields[0] = function_name;
    fields[1] = file_name;
    fields[2] = target;
    line      = p101_env_log_buffer_create(fields, 3U, &line_size);
    if(line == NULL)
    {
        return;
    }

    offset  = 0;
    line[0] = '\0';
    p101_env_log_append_event_prefix(env, line, line_size, &offset, "P101EXECFAIL", (long)getpid());
    snprintf(number, sizeof(number), "%d", line_number);    // NOLINT(cert-err33-c)
    p101_env_log_append_text(line, line_size, &offset, number);
    p101_env_log_append_char(line, line_size, &offset, '\t');
    p101_env_log_append_field(line, line_size, &offset, function_name);
    p101_env_log_append_char(line, line_size, &offset, '\t');
    p101_env_log_append_field(line, line_size, &offset, file_name);
    p101_env_log_append_char(line, line_size, &offset, '\t');
    p101_env_log_append_field(line, line_size, &offset, target);
    p101_env_log_append_char(line, line_size, &offset, '\n');
    offset = p101_env_log_finish_record(line, line_size, offset);

    fwrite(line, 1, offset, stream);    // NOLINT(cert-err33-c)
    fflush(stream);                     // NOLINT(cert-err33-c)
    free(line);
}

static void p101_env_alloc_log_observer(const struct p101_env *env, p101_env_alloc_event event, const void *ptr, const void *new_ptr, size_t size, const char *file_name, const char *function_name, int line_number, void *user_data)
{
    const char *fields[2];
    FILE       *stream;
    char       *line;
    char        number[P101_NUMBER_BUF_LEN];
    char        pointer[P101_POINTER_BUF_LEN];
    size_t      line_size;
    size_t      offset;

    (void)env;
    stream = (FILE *)user_data;

    if(stream == NULL)
    {
        return;
    }

    fields[0] = function_name;
    fields[1] = file_name;
    line      = p101_env_log_buffer_create(fields, 2U, &line_size);
    if(line == NULL)
    {
        return;
    }

    offset  = 0;
    line[0] = '\0';

    p101_env_log_append_event_prefix(env, line, line_size, &offset, "P101ALLOC", (long)getpid());
    p101_env_log_append_text(line, line_size, &offset, p101_env_alloc_event_name(event));
    p101_env_log_append_char(line, line_size, &offset, '\t');
    snprintf(pointer, sizeof(pointer), "%p", ptr);    // NOLINT(cert-err33-c)
    p101_env_log_append_text(line, line_size, &offset, pointer);
    p101_env_log_append_char(line, line_size, &offset, '\t');

    if(new_ptr == NULL)
    {
        p101_env_log_append_char(line, line_size, &offset, '-');
    }
    else
    {
        snprintf(pointer, sizeof(pointer), "%p", new_ptr);    // NOLINT(cert-err33-c)
        p101_env_log_append_text(line, line_size, &offset, pointer);
    }

    p101_env_log_append_char(line, line_size, &offset, '\t');
    snprintf(number, sizeof(number), "%zu", size);    // NOLINT(cert-err33-c)
    p101_env_log_append_text(line, line_size, &offset, number);
    p101_env_log_append_char(line, line_size, &offset, '\t');
    snprintf(number, sizeof(number), "%d", line_number);    // NOLINT(cert-err33-c)
    p101_env_log_append_text(line, line_size, &offset, number);
    p101_env_log_append_char(line, line_size, &offset, '\t');
    p101_env_log_append_field(line, line_size, &offset, function_name);
    p101_env_log_append_char(line, line_size, &offset, '\t');
    p101_env_log_append_field(line, line_size, &offset, file_name);
    p101_env_log_append_char(line, line_size, &offset, '\n');
    offset = p101_env_log_finish_record(line, line_size, offset);

    fwrite(line, 1, offset, stream);    // NOLINT(cert-err33-c)
    fflush(stream);                     // NOLINT(cert-err33-c)
    free(line);
}

static const char *p101_env_alloc_event_name(p101_env_alloc_event event)
{
#ifdef __clang__
    #pragma clang diagnostic push
    #pragma clang diagnostic ignored "-Wcovered-switch-default"
#endif
    switch(event)
    {
        case P101_ENV_ALLOC_ALLOC:
        {
            return "ALLOC";
        }
        case P101_ENV_ALLOC_FREE:
        {
            return "FREE";
        }
        case P101_ENV_ALLOC_REALLOC:
        {
            return "REALLOC";
        }
        default:
        {
            return "UNKNOWN";
        }
    }
#ifdef __clang__
    #pragma clang diagnostic pop
#endif
}

static void p101_env_call_log_observer(const struct p101_env *env, p101_env_call_event event, const char *call_name, const char *arguments, const char *result, const char *file_name, const char *function_name, int line_number, void *user_data)
{
    const char *fields[P101_CALL_LOG_FIELD_COUNT];
    FILE       *stream;
    char       *line;
    char        number[P101_NUMBER_BUF_LEN];
    const char *logged_arguments;
    const char *logged_result;
    size_t      line_size;
    size_t      offset;

    if(env == NULL)
    {
        return;
    }

    if(event == P101_ENV_CALL_ENTER && (env->call_log_options & P101_ENV_CALL_LOG_ENTER) == 0U)
    {
        return;
    }

    if(event == P101_ENV_CALL_EXIT && (env->call_log_options & P101_ENV_CALL_LOG_EXIT) == 0U)
    {
        return;
    }

    stream = (FILE *)user_data;

    if(stream == NULL)
    {
        return;
    }

    logged_arguments = ((env->call_log_options & P101_ENV_CALL_LOG_ARGUMENTS) == 0U) ? NULL : arguments;
    logged_result    = ((env->call_log_options & P101_ENV_CALL_LOG_RESULT) == 0U) ? NULL : result;
    fields[0]        = function_name;
    fields[1]        = (call_name == NULL) ? function_name : call_name;
    fields[2]        = logged_arguments;
    fields[3]        = logged_result;
    fields[4]        = file_name;
    line             = p101_env_log_buffer_create(fields, P101_CALL_LOG_FIELD_COUNT, &line_size);
    if(line == NULL)
    {
        return;
    }

    offset  = 0;
    line[0] = '\0';

    p101_env_log_append_event_prefix(env, line, line_size, &offset, "P101CALL", (long)getpid());
    p101_env_log_append_text(line, line_size, &offset, (event == P101_ENV_CALL_ENTER) ? "ENTER" : "EXIT");
    p101_env_log_append_char(line, line_size, &offset, '\t');
    snprintf(number, sizeof(number), "%d", line_number);    // NOLINT(cert-err33-c)
    p101_env_log_append_text(line, line_size, &offset, number);
    p101_env_log_append_char(line, line_size, &offset, '\t');
    p101_env_log_append_field(line, line_size, &offset, function_name);
    p101_env_log_append_char(line, line_size, &offset, '\t');
    p101_env_log_append_field(line, line_size, &offset, (call_name == NULL) ? function_name : call_name);
    p101_env_log_append_char(line, line_size, &offset, '\t');
    p101_env_log_append_field(line, line_size, &offset, logged_arguments);
    p101_env_log_append_char(line, line_size, &offset, '\t');
    p101_env_log_append_field(line, line_size, &offset, logged_result);
    p101_env_log_append_char(line, line_size, &offset, '\t');
    p101_env_log_append_field(line, line_size, &offset, file_name);
    p101_env_log_append_char(line, line_size, &offset, '\n');
    offset = p101_env_log_finish_record(line, line_size, offset);

    fwrite(line, 1, offset, stream);    // NOLINT(cert-err33-c)
    fflush(stream);                     // NOLINT(cert-err33-c)
    free(line);
}

static void p101_env_call_notify(const struct p101_env *env, p101_env_call_event event, const char *call_name, const char *arguments, const char *result, const char *file_name, const char *function_name, int line_number)
{
    if(env != NULL && env->call_observer != NULL)
    {
        env->call_observer(env, event, call_name, arguments, result, file_name, function_name, line_number, env->call_observer_data);
    }
}

static void p101_env_log_append_char(char line[], size_t line_size, size_t *offset, char ch)
{
    if(*offset + 1U < line_size)
    {
        line[*offset] = ch;
        (*offset)++;
        line[*offset] = '\0';
    }
}

static void p101_env_log_append_text(char line[], size_t line_size, size_t *offset, const char *text)
{
    if(text == NULL)
    {
        return;
    }

    while(*text != '\0')
    {
        p101_env_log_append_char(line, line_size, offset, *text);
        text++;
    }
}

static size_t p101_env_log_finish_record(char line[], size_t line_size, size_t offset)
{
    if(line_size == 0U)
    {
        return 0U;
    }

    if(offset == 0U)
    {
        line[0] = '\n';
        return 1U;
    }

    if(line[offset - 1U] != '\n')
    {
        if(offset + 1U < line_size)
        {
            line[offset] = '\n';
            offset++;
            line[offset] = '\0';
        }
        else
        {
            line[offset - 1U] = '\n';
        }
    }

    return offset;
}

static char *p101_env_log_buffer_create(const char *const fields[], size_t field_count, size_t *line_size)
{
    char  *line;
    size_t required;

    required = P101_EVENT_LOG_FIXED_SIZE;

    for(size_t index = 0U; index < field_count; index++)
    {
        size_t length;

        if(fields[index] == NULL)
        {
            continue;
        }

        length = strlen(fields[index]);
        if(length > (SIZE_MAX - required) / 2U)
        {
            *line_size = 0U;
            return NULL;
        }
        required += length * 2U;
    }

    line = (char *)malloc(required);
    if(line == NULL)
    {
        *line_size = 0U;
        return NULL;
    }

    line[0]    = '\0';
    *line_size = required;
    return line;
}

static void p101_env_log_append_field(char line[], size_t line_size, size_t *offset, const char *text)
{
    if(text == NULL)
    {
        p101_env_log_append_char(line, line_size, offset, '-');

        return;
    }

    while(*text != '\0')
    {
        unsigned char ch;

        ch = (unsigned char)*text;

        switch(ch)
        {
            case '\t':
            {
                p101_env_log_append_text(line, line_size, offset, "\\t");
                break;
            }
            case '\n':
            {
                p101_env_log_append_text(line, line_size, offset, "\\n");
                break;
            }
            case '\r':
            {
                p101_env_log_append_text(line, line_size, offset, "\\r");
                break;
            }
            case '\\':
            {
                p101_env_log_append_text(line, line_size, offset, "\\\\");
                break;
            }
            default:
            {
                if(ch < ' ' || ch == P101_ASCII_DELETE)
                {
                    p101_env_log_append_char(line, line_size, offset, '?');
                }
                else
                {
                    p101_env_log_append_char(line, line_size, offset, (char)ch);
                }
                break;
            }
        }

        text++;
    }
}

char *p101_env_event_split(char **cursor)
{
    char *start;
    char *tab;

    start = *cursor;

    if(start == NULL)
    {
        goto done;
    }

    tab = start;

    while(*tab != '\0' && *tab != '\t')
    {
        tab++;
    }

    if(*tab == '\0')
    {
        *cursor = NULL;
    }
    else
    {
        *tab    = '\0';
        *cursor = tab + 1;
    }

done:
    return start;
}

static void p101_env_event_unescape_char(char **read_cursor, char **write_cursor)
{
    (*read_cursor)++;
    if(**read_cursor == 't')
    {
        **write_cursor = '\t';
    }
    else if(**read_cursor == 'n')
    {
        **write_cursor = '\n';
    }
    else if(**read_cursor == 'r')
    {
        **write_cursor = '\r';
    }
    else
    {
        **write_cursor = **read_cursor;
    }
    (*write_cursor)++;
    (*read_cursor)++;
}

void p101_env_event_unescape_field(char *field)
{
    char *read_cursor;
    char *write_cursor;

    if(field == NULL)
    {
        goto done;
    }

    read_cursor  = field;
    write_cursor = field;
    while(*read_cursor != '\0')
    {
        if(read_cursor[0] == '\\' && read_cursor[1] != '\0')
        {
            p101_env_event_unescape_char(&read_cursor, &write_cursor);
        }
        else
        {
            *write_cursor = *read_cursor;
            write_cursor++;
            read_cursor++;
        }
    }
    *write_cursor = '\0';

done:
    return;
}

static int p101_env_event_parse_long_field(const char *text, long min, long max, long *out)
{
    const char *cursor;
    long        value;
    int         negative;
    int         result;

    cursor = text;
    value  = 0;
    result = 0;

    if(cursor == NULL || *cursor == '\0')
    {
        goto done;
    }

    negative = (*cursor == '-') ? 1 : 0;
    if(negative)
    {
        cursor++;
    }

    if(*cursor == '\0')
    {
        goto done;
    }

    while(*cursor != '\0')
    {
        int digit;

        if(*cursor < '0' || *cursor > '9')
        {
            goto done;
        }

        digit = *cursor - '0';
        if(value > (LONG_MAX - (long)digit) / P101_ENV_NUMBER_BASE)
        {
            goto done;
        }

        value = (value * P101_ENV_NUMBER_BASE) + digit;
        cursor++;
    }

    if(negative)
    {
        value = -value;
    }

    if(value < min || value > max)
    {
        goto done;
    }

    *out   = value;
    result = 1;

done:
    return result;
}

int p101_env_event_parse_size_field(const char *text, size_t *out)
{
    const char *cursor;
    size_t      value;
    int         result;

    cursor = text;
    value  = 0U;
    result = 0;

    if(cursor == NULL || *cursor == '\0')
    {
        goto done;
    }

    while(*cursor != '\0')
    {
        size_t digit;

        if(*cursor < '0' || *cursor > '9')
        {
            goto done;
        }

        digit = (size_t)(*cursor - '0');
        if(value > (SIZE_MAX - digit) / (size_t)P101_ENV_NUMBER_BASE)
        {
            goto done;
        }

        value = (value * (size_t)P101_ENV_NUMBER_BASE) + digit;
        cursor++;
    }

    *out   = value;
    result = 1;

done:
    return result;
}

static int p101_env_event_parse_optional_size_field(const char *text, size_t *out, int *available)
{
    int result;

    result     = 0;
    *out       = 0U;
    *available = 0;

    if(text == NULL || text[0] == '\0')
    {
        goto done;
    }

    if(strcmp(text, "-") == 0)
    {
        result = 1;
        goto done;
    }

    if(!p101_env_event_parse_size_field(text, out))
    {
        goto done;
    }

    *available = 1;
    result     = 1;

done:
    return result;
}

static p101_env_event_parse_status p101_env_event_parse_version_metadata(char **cursor, struct p101_env_event_record *record)
{
    const char *version_text;
    const char *pid_text;
    const char *sequence_text;
    const char *monotonic_text;
    const char *wall_text;
    long        version;

    version_text   = p101_env_event_split(cursor);
    pid_text       = p101_env_event_split(cursor);
    sequence_text  = p101_env_event_split(cursor);
    monotonic_text = p101_env_event_split(cursor);
    wall_text      = p101_env_event_split(cursor);

    if(!p101_env_event_parse_long_field(version_text, 0, LONG_MAX, &version))
    {
        return P101_ENV_EVENT_PARSE_MALFORMED;
    }

    if(version != P101_ENV_EVENT_LOG_VERSION_2)
    {
        return P101_ENV_EVENT_PARSE_BAD_VERSION;
    }

    if(!p101_env_event_parse_long_field(pid_text, 0, LONG_MAX, &record->pid))
    {
        return P101_ENV_EVENT_PARSE_MALFORMED;
    }

    if(!p101_env_event_parse_size_field(sequence_text, &record->sequence))
    {
        return P101_ENV_EVENT_PARSE_MALFORMED;
    }

    if(!p101_env_event_parse_optional_size_field(monotonic_text, &record->monotonic_ns, &record->monotonic_ns_available))
    {
        return P101_ENV_EVENT_PARSE_MALFORMED;
    }

    if(!p101_env_event_parse_optional_size_field(wall_text, &record->wall_unix_ns, &record->wall_unix_ns_available))
    {
        return P101_ENV_EVENT_PARSE_MALFORMED;
    }

    return P101_ENV_EVENT_PARSE_OK;
}

static p101_env_event_parse_status p101_env_event_parse_fd(char *line, struct p101_env_event_record *record)
{
    char                       *cursor;
    const char                 *kind_text;
    const char                 *fd_text;
    const char                 *line_text;
    long                        fd;
    long                        line_number;
    p101_env_event_parse_status status;

    if(strncmp(line, "P101FD\t", strlen("P101FD\t")) != 0)
    {
        return P101_ENV_EVENT_PARSE_OTHER;
    }

    cursor = line + strlen("P101FD\t");
    status = p101_env_event_parse_version_metadata(&cursor, record);
    if(status != P101_ENV_EVENT_PARSE_OK)
    {
        return status;
    }

    kind_text             = p101_env_event_split(&cursor);
    fd_text               = p101_env_event_split(&cursor);
    line_text             = p101_env_event_split(&cursor);
    record->function_name = p101_env_event_split(&cursor);
    record->file_name     = cursor;

    if(kind_text == NULL || fd_text == NULL || line_text == NULL || record->function_name == NULL || record->file_name == NULL)
    {
        return P101_ENV_EVENT_PARSE_MALFORMED;
    }

    if(strcmp(kind_text, "OPEN") == 0)
    {
        record->fd_kind = P101_ENV_EVENT_FD_OPEN;
    }
    else if(strcmp(kind_text, "CLOSE") == 0)
    {
        record->fd_kind = P101_ENV_EVENT_FD_CLOSE;
    }
    else
    {
        return P101_ENV_EVENT_PARSE_MALFORMED;
    }

    if(!p101_env_event_parse_long_field(fd_text, 0, P101_EVENT_PARSE_FD_MAX, &fd) || !p101_env_event_parse_long_field(line_text, 0, INT_MAX, &line_number))
    {
        return P101_ENV_EVENT_PARSE_MALFORMED;
    }

    record->record_kind = P101_ENV_EVENT_RECORD_FD;
    record->fd          = (int)fd;
    record->line_number = (int)line_number;
    p101_env_event_unescape_field(record->function_name);
    p101_env_event_unescape_field(record->file_name);

    return P101_ENV_EVENT_PARSE_OK;
}

static p101_env_event_parse_status p101_env_event_parse_alloc(char *line, struct p101_env_event_record *record)
{
    char                       *cursor;
    const char                 *kind_text;
    char                       *new_ptr_text;
    const char                 *size_text;
    const char                 *line_text;
    long                        line_number;
    p101_env_event_parse_status status;

    if(strncmp(line, "P101ALLOC\t", strlen("P101ALLOC\t")) != 0)
    {
        return P101_ENV_EVENT_PARSE_OTHER;
    }

    cursor = line + strlen("P101ALLOC\t");
    status = p101_env_event_parse_version_metadata(&cursor, record);
    if(status != P101_ENV_EVENT_PARSE_OK)
    {
        return status;
    }

    kind_text             = p101_env_event_split(&cursor);
    record->ptr           = p101_env_event_split(&cursor);
    new_ptr_text          = p101_env_event_split(&cursor);
    size_text             = p101_env_event_split(&cursor);
    line_text             = p101_env_event_split(&cursor);
    record->function_name = p101_env_event_split(&cursor);
    record->file_name     = cursor;

    if(kind_text == NULL || record->ptr == NULL || new_ptr_text == NULL || size_text == NULL || line_text == NULL || record->function_name == NULL || record->file_name == NULL)
    {
        return P101_ENV_EVENT_PARSE_MALFORMED;
    }

    if(strcmp(kind_text, "ALLOC") == 0)
    {
        record->alloc_kind = P101_ENV_EVENT_ALLOC_ALLOC;
    }
    else if(strcmp(kind_text, "FREE") == 0)
    {
        record->alloc_kind = P101_ENV_EVENT_ALLOC_FREE;
    }
    else if(strcmp(kind_text, "REALLOC") == 0)
    {
        record->alloc_kind = P101_ENV_EVENT_ALLOC_REALLOC;
    }
    else
    {
        return P101_ENV_EVENT_PARSE_MALFORMED;
    }

    if(!p101_env_event_parse_size_field(size_text, &record->size) || !p101_env_event_parse_long_field(line_text, 0, INT_MAX, &line_number))
    {
        return P101_ENV_EVENT_PARSE_MALFORMED;
    }

    record->record_kind = P101_ENV_EVENT_RECORD_ALLOC;
    record->new_ptr     = (strcmp(new_ptr_text, "-") == 0) ? NULL : new_ptr_text;
    record->line_number = (int)line_number;
    p101_env_event_unescape_field(record->ptr);
    p101_env_event_unescape_field(record->new_ptr);
    p101_env_event_unescape_field(record->function_name);
    p101_env_event_unescape_field(record->file_name);

    return P101_ENV_EVENT_PARSE_OK;
}

static p101_env_event_parse_status p101_env_event_parse_fork(char *line, struct p101_env_event_record *record)
{
    char                       *cursor;
    const char                 *child_pid_text;
    const char                 *line_text;
    long                        line_number;
    p101_env_event_parse_status status;

    if(strncmp(line, "P101FORK\t", strlen("P101FORK\t")) != 0)
    {
        return P101_ENV_EVENT_PARSE_OTHER;
    }

    cursor = line + strlen("P101FORK\t");
    status = p101_env_event_parse_version_metadata(&cursor, record);
    if(status != P101_ENV_EVENT_PARSE_OK)
    {
        return status;
    }

    child_pid_text        = p101_env_event_split(&cursor);
    line_text             = p101_env_event_split(&cursor);
    record->function_name = p101_env_event_split(&cursor);
    record->file_name     = cursor;

    if(child_pid_text == NULL || line_text == NULL || record->function_name == NULL || record->file_name == NULL)
    {
        return P101_ENV_EVENT_PARSE_MALFORMED;
    }

    if(!p101_env_event_parse_long_field(child_pid_text, 0, LONG_MAX, &record->child_pid) || !p101_env_event_parse_long_field(line_text, 0, INT_MAX, &line_number))
    {
        return P101_ENV_EVENT_PARSE_MALFORMED;
    }

    record->record_kind = P101_ENV_EVENT_RECORD_FORK;
    record->line_number = (int)line_number;
    p101_env_event_unescape_field(record->function_name);
    p101_env_event_unescape_field(record->file_name);

    return P101_ENV_EVENT_PARSE_OK;
}

static p101_env_event_parse_status p101_env_event_parse_spawn(char *line, struct p101_env_event_record *record)
{
    char                       *cursor;
    const char                 *child_pid_text;
    const char                 *line_text;
    long                        line_number;
    p101_env_event_parse_status status;

    if(strncmp(line, "P101SPAWN\t", strlen("P101SPAWN\t")) != 0)
    {
        return P101_ENV_EVENT_PARSE_OTHER;
    }

    cursor = line + strlen("P101SPAWN\t");
    status = p101_env_event_parse_version_metadata(&cursor, record);
    if(status != P101_ENV_EVENT_PARSE_OK)
    {
        return status;
    }

    child_pid_text        = p101_env_event_split(&cursor);
    line_text             = p101_env_event_split(&cursor);
    record->function_name = p101_env_event_split(&cursor);
    record->file_name     = p101_env_event_split(&cursor);
    record->target        = cursor;

    if(child_pid_text == NULL || line_text == NULL || record->function_name == NULL || record->file_name == NULL || record->target == NULL)
    {
        return P101_ENV_EVENT_PARSE_MALFORMED;
    }

    if(!p101_env_event_parse_long_field(child_pid_text, 0, LONG_MAX, &record->child_pid) || !p101_env_event_parse_long_field(line_text, 0, INT_MAX, &line_number))
    {
        return P101_ENV_EVENT_PARSE_MALFORMED;
    }

    record->record_kind = P101_ENV_EVENT_RECORD_SPAWN;
    record->line_number = (int)line_number;
    p101_env_event_unescape_field(record->function_name);
    p101_env_event_unescape_field(record->file_name);
    p101_env_event_unescape_field(record->target);

    return P101_ENV_EVENT_PARSE_OK;
}

static p101_env_event_parse_status p101_env_event_parse_exec(char *line, struct p101_env_event_record *record)
{
    char                       *cursor;
    const char                 *fd_text;
    const char                 *cloexec_text;
    const char                 *line_text;
    long                        fd;
    long                        cloexec;
    long                        line_number;
    p101_env_event_parse_status status;

    if(strncmp(line, "P101EXEC\t", strlen("P101EXEC\t")) != 0)
    {
        return P101_ENV_EVENT_PARSE_OTHER;
    }

    cursor = line + strlen("P101EXEC\t");
    status = p101_env_event_parse_version_metadata(&cursor, record);
    if(status != P101_ENV_EVENT_PARSE_OK)
    {
        return status;
    }

    fd_text               = p101_env_event_split(&cursor);
    cloexec_text          = p101_env_event_split(&cursor);
    line_text             = p101_env_event_split(&cursor);
    record->function_name = p101_env_event_split(&cursor);
    record->file_name     = p101_env_event_split(&cursor);
    record->target        = cursor;

    if(fd_text == NULL || cloexec_text == NULL || line_text == NULL || record->function_name == NULL || record->file_name == NULL || record->target == NULL)
    {
        return P101_ENV_EVENT_PARSE_MALFORMED;
    }

    if(!p101_env_event_parse_long_field(fd_text, 0, P101_EVENT_PARSE_FD_MAX, &fd) || !p101_env_event_parse_long_field(cloexec_text, 0, 1, &cloexec) || !p101_env_event_parse_long_field(line_text, 0, INT_MAX, &line_number))
    {
        return P101_ENV_EVENT_PARSE_MALFORMED;
    }

    record->record_kind = P101_ENV_EVENT_RECORD_EXEC;
    record->fd          = (int)fd;
    record->cloexec     = (int)cloexec;
    record->line_number = (int)line_number;
    p101_env_event_unescape_field(record->function_name);
    p101_env_event_unescape_field(record->file_name);
    p101_env_event_unescape_field(record->target);

    return P101_ENV_EVENT_PARSE_OK;
}

static p101_env_event_parse_status p101_env_event_parse_exec_failure(char *line, struct p101_env_event_record *record)
{
    char                       *cursor;
    const char                 *line_text;
    long                        line_number;
    p101_env_event_parse_status status;

    if(strncmp(line, "P101EXECFAIL\t", strlen("P101EXECFAIL\t")) != 0)
    {
        return P101_ENV_EVENT_PARSE_OTHER;
    }

    cursor = line + strlen("P101EXECFAIL\t");
    status = p101_env_event_parse_version_metadata(&cursor, record);
    if(status != P101_ENV_EVENT_PARSE_OK)
    {
        return status;
    }

    line_text             = p101_env_event_split(&cursor);
    record->function_name = p101_env_event_split(&cursor);
    record->file_name     = p101_env_event_split(&cursor);
    record->target        = cursor;

    if(line_text == NULL || record->function_name == NULL || record->file_name == NULL || record->target == NULL || !p101_env_event_parse_long_field(line_text, 0, INT_MAX, &line_number))
    {
        return P101_ENV_EVENT_PARSE_MALFORMED;
    }

    record->record_kind = P101_ENV_EVENT_RECORD_EXEC_FAIL;
    record->line_number = (int)line_number;
    p101_env_event_unescape_field(record->function_name);
    p101_env_event_unescape_field(record->file_name);
    p101_env_event_unescape_field(record->target);

    return P101_ENV_EVENT_PARSE_OK;
}

static p101_env_event_parse_status p101_env_event_parse_call(char *line, struct p101_env_event_record *record)
{
    char                       *cursor;
    const char                 *kind_text;
    const char                 *line_text;
    long                        line_number;
    p101_env_event_parse_status status;

    if(strncmp(line, "P101CALL\t", strlen("P101CALL\t")) != 0)
    {
        return P101_ENV_EVENT_PARSE_OTHER;
    }

    cursor = line + strlen("P101CALL\t");
    status = p101_env_event_parse_version_metadata(&cursor, record);
    if(status != P101_ENV_EVENT_PARSE_OK)
    {
        return status;
    }

    kind_text             = p101_env_event_split(&cursor);
    line_text             = p101_env_event_split(&cursor);
    record->function_name = p101_env_event_split(&cursor);
    record->call_name     = p101_env_event_split(&cursor);
    record->arguments     = p101_env_event_split(&cursor);
    record->result        = p101_env_event_split(&cursor);
    record->file_name     = p101_env_event_split(&cursor);

    if(cursor != NULL || kind_text == NULL || line_text == NULL || record->function_name == NULL || record->call_name == NULL || record->arguments == NULL || record->result == NULL || record->file_name == NULL)
    {
        return P101_ENV_EVENT_PARSE_MALFORMED;
    }

    if(strcmp(kind_text, "ENTER") == 0)
    {
        record->call_kind = P101_ENV_EVENT_CALL_ENTER;
    }
    else if(strcmp(kind_text, "EXIT") == 0)
    {
        record->call_kind = P101_ENV_EVENT_CALL_EXIT;
    }
    else
    {
        return P101_ENV_EVENT_PARSE_MALFORMED;
    }

    if(!p101_env_event_parse_long_field(line_text, 0, INT_MAX, &line_number))
    {
        return P101_ENV_EVENT_PARSE_MALFORMED;
    }

    record->record_kind = P101_ENV_EVENT_RECORD_CALL;
    record->line_number = (int)line_number;
    p101_env_event_unescape_field(record->function_name);
    p101_env_event_unescape_field(record->call_name);
    p101_env_event_unescape_field(record->arguments);
    p101_env_event_unescape_field(record->result);
    p101_env_event_unescape_field(record->file_name);

    return P101_ENV_EVENT_PARSE_OK;
}

static void p101_env_fd_notify(const struct p101_env *env, p101_env_fd_event event, int fd, const char *file_name, const char *function_name, int line_number)
{
    if(env != NULL && env->fd_observer != NULL)
    {
        env->fd_observer(env, event, fd, file_name, function_name, line_number, env->fd_observer_data);
    }
}

static void p101_env_alloc_notify(const struct p101_env *env, p101_env_alloc_event event, const void *ptr, const void *new_ptr, size_t size, const char *file_name, const char *function_name, int line_number)
{
    if(env != NULL && env->alloc_observer != NULL)
    {
        env->alloc_observer(env, event, ptr, new_ptr, size, file_name, function_name, line_number, env->alloc_observer_data);
    }
}

void p101_env_track_alloc(const struct p101_env *env, const void *ptr, size_t size, const char *file_name, const char *function_name, int line_number)
{
    if(env == NULL || ptr == NULL)
    {
        return;
    }

    p101_env_alloc_notify(env, P101_ENV_ALLOC_ALLOC, ptr, NULL, size, file_name, function_name, line_number);
}

void p101_env_track_free(const struct p101_env *env, const void *ptr, const char *file_name, const char *function_name, int line_number)
{
    if(env == NULL || ptr == NULL)
    {
        return;
    }

    p101_env_alloc_notify(env, P101_ENV_ALLOC_FREE, ptr, NULL, 0, file_name, function_name, line_number);
}

void p101_env_track_realloc(const struct p101_env *env, const void *ptr, const void *new_ptr, size_t size, const char *file_name, const char *function_name, int line_number)
{
    if(env == NULL || new_ptr == NULL)
    {
        return;
    }

    p101_env_alloc_notify(env, P101_ENV_ALLOC_REALLOC, ptr, new_ptr, size, file_name, function_name, line_number);
}

void p101_env_track_open(const struct p101_env *env, int fd, const char *file_name, const char *function_name, int line_number)
{
    struct p101_fd_record *rec;

    if(env == NULL || fd < 0)
    {
        return;
    }

    /* The observer runs first and unconditionally: a log costs nothing extra
     * when the in-process ledger is switched off. */
    p101_env_fd_notify(env, P101_ENV_FD_OPEN, fd, file_name, function_name, line_number);

    if(env->fd_ledger == NULL)
    {
        return;
    }

    rec = (struct p101_fd_record *)malloc(sizeof(struct p101_fd_record));

    if(rec == NULL)
    {
        /* Best-effort bookkeeping: never fail the caller's real operation. */
        return;
    }

    rec->fd              = fd;
    rec->file_name       = file_name;
    rec->function_name   = function_name;
    rec->line_number     = line_number;
    rec->next            = env->fd_ledger->head;
    env->fd_ledger->head = rec;
}

void p101_env_track_close(const struct p101_env *env, int fd, const char *file_name, const char *function_name, int line_number)
{
    struct p101_fd_record **pp;

    if(env == NULL || fd < 0)
    {
        return;
    }

    /* The CLOSE site is recorded even though the ledger only needs the fd --
     * it is what lets an analyzer say which two closes raced for the same
     * descriptor, and where a stray close of a never-opened fd came from. */
    p101_env_fd_notify(env, P101_ENV_FD_CLOSE, fd, file_name, function_name, line_number);

    if(env->fd_ledger == NULL)
    {
        return;
    }

    pp = &env->fd_ledger->head;

    while(*pp != NULL)
    {
        if((*pp)->fd == fd)
        {
            struct p101_fd_record *dead = *pp;

            *pp = dead->next;
            free(dead);

            return;
        }

        pp = &(*pp)->next;
    }
}

void p101_env_track_fork(const struct p101_env *env, long parent_pid, long child_pid, const char *file_name, const char *function_name, int line_number)
{
    FILE *stream;

    if(env == NULL || parent_pid < 0 || child_pid < 0)
    {
        return;
    }

    /* A fork is not an OPEN or CLOSE, so it is emitted only by the standard
     * resource-log sink. Custom fd observers keep their simple two-event
     * contract. */
    if(env->fd_observer != p101_env_fd_log_observer)
    {
        return;
    }

    stream = (FILE *)env->fd_observer_data;
    p101_env_fork_log(env, stream, parent_pid, child_pid, file_name, function_name, line_number);
}

void p101_env_track_spawn(const struct p101_env *env, long parent_pid, long child_pid, const char *target, const char *file_name, const char *function_name, int line_number)
{
    FILE *stream;

    if(env == NULL || parent_pid < 0 || child_pid < 0 || env->fd_observer != p101_env_fd_log_observer)
    {
        return;
    }

    stream = (FILE *)env->fd_observer_data;
    p101_env_spawn_log(env, stream, parent_pid, child_pid, target, file_name, function_name, line_number);
}

void p101_env_track_exec(const struct p101_env *env, const char *target, const char *file_name, const char *function_name, int line_number)
{
    FILE *stream;
    long  limit;

    if(env == NULL)
    {
        return;
    }

    /*
     * Exec is a resource-log concept, not an OPEN/CLOSE observer event. Custom
     * fd observers keep their simple two-event contract.
     */
    if(env->fd_observer != p101_env_fd_log_observer)
    {
        return;
    }

    stream = (FILE *)env->fd_observer_data;
    limit  = p101_env_exec_scan_limit();

    for(long fd = 0; fd < limit; fd++)
    {
        int flags;

        flags = fcntl((int)fd, F_GETFD);

        if(flags == -1)
        {
            continue;
        }

        p101_env_exec_fd_log(env, stream, (int)fd, ((flags & FD_CLOEXEC) == FD_CLOEXEC) ? 1 : 0, target, file_name, function_name, line_number);
    }
}

void p101_env_track_exec_failure(const struct p101_env *env, const char *target, const char *file_name, const char *function_name, int line_number)
{
    FILE *stream;

    if(env == NULL || env->fd_observer != p101_env_fd_log_observer)
    {
        return;
    }

    stream = (FILE *)env->fd_observer_data;
    p101_env_exec_failure_log(env, stream, target, file_name, function_name, line_number);
}

size_t p101_env_report_leaks(const struct p101_env *env)
{
    size_t                       count;
    const struct p101_fd_record *cur;

    if(env == NULL || env->fd_ledger == NULL)
    {
        return 0;
    }

    count = 0;
    cur   = env->fd_ledger->head;

    while(cur != NULL)
    {
        fprintf(stderr,
                "LEAK (pid=%" PRIdMAX "): fd %d opened at %s : %s : @ %d never closed\n",
                (intmax_t)getpid(),
                cur->fd,
                (cur->file_name == NULL) ? "?" : cur->file_name,
                (cur->function_name == NULL) ? "?" : cur->function_name,
                cur->line_number);    // NOLINT(cert-err33-c)
        count++;
        cur = cur->next;
    }

    return count;
}
