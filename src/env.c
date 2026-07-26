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
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
    const char   *call_name; /* borrowed from the process environment */
    FILE         *log_stream;
    int           log_owned;
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
    FILE                        *owned_fd_log_stream;
    FILE                        *owned_call_log_stream;
    struct p101_env_fault_state *owned_fault_state;
};

/* Big enough for a deep path; anything longer is truncated, never split. */
enum
{
    P101_FD_LOG_LINE_MAX     = 1024,
    P101_CALL_LOG_LINE_MAX   = 2048,
    P101_ALLOC_LOG_LINE_MAX  = 2048,
    P101_NUMBER_BUF_LEN      = 64,
    P101_POINTER_BUF_LEN     = 64,
    P101_ASCII_DELETE        = 0x7F,
    P101_ENV_NUMBER_BASE     = 10,
    P101_DEFAULT_FAULT_ERRNO = EIO
};

static void          p101_env_init(struct p101_env *env, p101_env_tracer tracer);
static void          p101_env_configure_from_environment(struct p101_env *env, struct p101_error *err);
static void          p101_env_configure_fault_from_environment(struct p101_env *env, struct p101_error *err);
static void          p101_env_configure_fd_log_from_environment(struct p101_env *env, struct p101_error *err);
static void          p101_env_configure_call_log_from_environment(struct p101_env *env, struct p101_error *err);
static FILE         *p101_env_open_log_from_environment(struct p101_error *err, const char *path, int *owned);
static void          p101_env_close_owned_resource_log(struct p101_env *env);
static void          p101_env_fault_state_destroy(struct p101_env_fault_state *state);
static void          p101_env_log_fault_hit(const struct p101_env_fault_state *state, const char *call_name);
static int           p101_env_environment_fault_injector(const struct p101_env *env, const char *call_name, void *user_data);
static unsigned long p101_env_parse_unsigned_environment(const char *text, unsigned long default_value, int *ok);
static int           p101_env_parse_int_environment(const char *text, int default_value, int *ok);
static int           p101_env_flag_on(const char *name, int default_value);
static void          p101_env_fd_notify(const struct p101_env *env, p101_env_fd_event event, int fd, const char *file_name, const char *function_name, int line_number);
static void          p101_env_fd_log_observer(const struct p101_env *env, p101_env_fd_event event, int fd, const char *file_name, const char *function_name, int line_number, void *user_data);
static void          p101_env_alloc_notify(const struct p101_env *env, p101_env_alloc_event event, const void *ptr, const void *new_ptr, size_t size, const char *file_name, const char *function_name, int line_number);
static void          p101_env_alloc_log_observer(const struct p101_env *env, p101_env_alloc_event event, const void *ptr, const void *new_ptr, size_t size, const char *file_name, const char *function_name, int line_number, void *user_data);
static const char   *p101_env_alloc_event_name(p101_env_alloc_event event);
static void          p101_env_call_notify(const struct p101_env *env, p101_env_call_event event, const char *call_name, const char *arguments, const char *result, const char *file_name, const char *function_name, int line_number);
static void          p101_env_call_log_observer(const struct p101_env *env, p101_env_call_event event, const char *call_name, const char *arguments, const char *result, const char *file_name, const char *function_name, int line_number, void *user_data);
static void          p101_env_log_append_char(char line[], size_t line_size, size_t *offset, char ch);
static void          p101_env_log_append_text(char line[], size_t line_size, size_t *offset, const char *text);
static void          p101_env_log_append_field(char line[], size_t line_size, size_t *offset, const char *text);

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
        p101_env_configure_from_environment(env, err);
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
        new_env->owned_fd_log_stream   = NULL;
        new_env->owned_call_log_stream = NULL;
        new_env->owned_fault_state     = NULL;

        if(env->owned_fault_state != NULL)
        {
            new_env->fault_injector = NULL;
            new_env->fault_data     = NULL;
            p101_env_configure_fault_from_environment(new_env, err);
        }

        if(env->owned_fd_log_stream != NULL)
        {
            new_env->fd_observer         = NULL;
            new_env->fd_observer_data    = NULL;
            new_env->alloc_observer      = NULL;
            new_env->alloc_observer_data = NULL;
            p101_env_configure_fd_log_from_environment(new_env, err);
        }

        if(env->owned_call_log_stream != NULL)
        {
            new_env->call_observer      = NULL;
            new_env->call_observer_data = NULL;
            new_env->call_log_options   = P101_ENV_CALL_LOG_DEFAULT;
            p101_env_configure_call_log_from_environment(new_env, err);
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
        fclose(env->owned_call_log_stream);    // NOLINT(cert-err33-c)
    }

    if(env != NULL)
    {
        p101_env_fault_state_destroy(env->owned_fault_state);
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
    env->owned_fd_log_stream   = NULL;
    env->owned_call_log_stream = NULL;
    env->owned_fault_state     = NULL;
}

static void p101_env_configure_from_environment(struct p101_env *env, struct p101_error *err)
{
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
    state->call_name   = getenv("P101_FAULT_NAME");
    state->log_stream  = NULL;
    state->log_owned   = 0;

    if(state->call_name != NULL && state->call_name[0] == '\0')
    {
        state->call_name = NULL;
    }

    log_text = getenv("P101_FAULT_LOG");

    if(log_text != NULL && log_text[0] != '\0')
    {
        state->log_stream = p101_env_open_log_from_environment(err, log_text, &log_owned);

        if(state->log_stream == NULL)
        {
            free(state);
            return;
        }

        state->log_owned = log_owned;
    }

    env->fault_injector    = p101_env_environment_fault_injector;
    env->fault_data        = state;
    env->owned_fault_state = state;
}

static void p101_env_configure_fd_log_from_environment(struct p101_env *env, struct p101_error *err)
{
    const char *path;
    FILE       *stream;
    int         owned;

    path = getenv("P101_RESOURCE_LOG");

    if(path == NULL || path[0] == '\0')
    {
        return;
    }

    stream = p101_env_open_log_from_environment(err, path, &owned);

    if(stream == NULL)
    {
        return;
    }

    setvbuf(stream, NULL, _IOLBF, 0);    // NOLINT(cert-err33-c)
    env->fd_observer         = p101_env_fd_log_observer;
    env->fd_observer_data    = stream;
    env->alloc_observer      = p101_env_alloc_log_observer;
    env->alloc_observer_data = stream;

    if(owned)
    {
        env->owned_fd_log_stream = stream;
    }
}

static void p101_env_configure_call_log_from_environment(struct p101_env *env, struct p101_error *err)
{
    const char *path;
    FILE       *stream;
    unsigned    options;
    int         owned;

    path = getenv("P101_CALL_LOG");

    if(path == NULL || path[0] == '\0')
    {
        return;
    }

    stream = p101_env_open_log_from_environment(err, path, &owned);

    if(stream == NULL)
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

    setvbuf(stream, NULL, _IOLBF, 0);    // NOLINT(cert-err33-c)
    env->call_observer      = p101_env_call_log_observer;
    env->call_observer_data = stream;
    env->call_log_options   = options;

    if(owned)
    {
        env->owned_call_log_stream = stream;
    }
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
    const char *label = p101_env_get_label(env);

    if(label != NULL)
    {
        fprintf(stdout, "TRACE (pid=%" PRIdMAX ", %s): %s : %s : @ %d\n", (intmax_t)getpid(), label, file_name, function_name, line_number);    // NOLINT(cert-err33-c)
    }
    else
    {
        fprintf(stdout, "TRACE (pid=%" PRIdMAX "): %s : %s : @ %d\n", (intmax_t)getpid(), file_name, function_name, line_number);    // NOLINT(cert-err33-c)
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
        fclose(env->owned_call_log_stream);    // NOLINT(cert-err33-c)
        env->owned_call_log_stream = NULL;
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
        fclose(env->owned_call_log_stream);    // NOLINT(cert-err33-c)
        env->owned_call_log_stream = NULL;
    }

    if(stream == NULL)
    {
        env->call_observer      = NULL;
        env->call_observer_data = NULL;
        env->call_log_options   = P101_ENV_CALL_LOG_DEFAULT;

        return;
    }

    /* Line buffering keeps a trace stream grep-friendly when mixed with
     * ordinary output. */
    setvbuf(stream, NULL, _IOLBF, 0);    // NOLINT(cert-err33-c)
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

    p101_env_close_owned_resource_log(env);

    env->fd_observer      = observer;
    env->fd_observer_data = user_data;
}

void p101_env_set_fd_log(struct p101_env *env, FILE *stream)
{
    p101_env_trace(env, __FILE__, __func__, __LINE__);

    if(env == NULL)
    {
        return;
    }

    p101_env_close_owned_resource_log(env);

    if(stream == NULL)
    {
        env->fd_observer      = NULL;
        env->fd_observer_data = NULL;

        return;
    }

    /* Line buffering so a log sharing a stream with ordinary output never
     * interleaves in the middle of a record. */
    setvbuf(stream, NULL, _IOLBF, 0);    // NOLINT(cert-err33-c)
    env->fd_observer      = p101_env_fd_log_observer;
    env->fd_observer_data = stream;
}

/* cppcheck-suppress funcArgNamesDifferentUnnamed */
void p101_env_set_alloc_observer(struct p101_env *env, p101_env_alloc_observer observer, void *user_data)
{
    p101_env_trace(env, __FILE__, __func__, __LINE__);

    if(env == NULL)
    {
        return;
    }

    p101_env_close_owned_resource_log(env);

    env->alloc_observer      = observer;
    env->alloc_observer_data = user_data;
}

void p101_env_set_alloc_log(struct p101_env *env, FILE *stream)
{
    p101_env_trace(env, __FILE__, __func__, __LINE__);

    if(env == NULL)
    {
        return;
    }

    p101_env_close_owned_resource_log(env);

    if(stream == NULL)
    {
        env->alloc_observer      = NULL;
        env->alloc_observer_data = NULL;

        return;
    }

    setvbuf(stream, NULL, _IOLBF, 0);    // NOLINT(cert-err33-c)
    env->alloc_observer      = p101_env_alloc_log_observer;
    env->alloc_observer_data = stream;
}

static void p101_env_fd_log_observer(const struct p101_env *env, p101_env_fd_event event, int fd, const char *file_name, const char *function_name, int line_number, void *user_data)
{
    FILE  *stream;
    char   line[P101_FD_LOG_LINE_MAX];
    int    written;
    size_t length;

    (void)env;
    stream = (FILE *)user_data;

    if(stream == NULL)
    {
        return;
    }

    /* getpid() on every line rather than once at install time: after a fork
     * the child keeps writing to the same stream, and the analyzer has to be
     * able to tell the two processes' descriptors apart. */
    written =
        snprintf(line, sizeof(line), "P101FD\t1\t%" PRIdMAX "\t%s\t%d\t%d\t%s\t%s\n", (intmax_t)getpid(), (event == P101_ENV_FD_OPEN) ? "OPEN" : "CLOSE", fd, line_number, (function_name == NULL) ? "?" : function_name, (file_name == NULL) ? "?" : file_name);

    if(written < 0)
    {
        return;
    }

    if((size_t)written >= sizeof(line))
    {
        /* Truncated. Force the newline back on, or this record would swallow
         * the next one and the analyzer would lose two events instead of one. */
        length           = sizeof(line) - 1;
        line[length - 1] = '\n';
    }
    else
    {
        length = (size_t)written;
    }

    fwrite(line, 1, length, stream);    // NOLINT(cert-err33-c)
    fflush(stream);                     // NOLINT(cert-err33-c)
}

static void p101_env_alloc_log_observer(const struct p101_env *env, p101_env_alloc_event event, const void *ptr, const void *new_ptr, size_t size, const char *file_name, const char *function_name, int line_number, void *user_data)
{
    FILE  *stream;
    char   line[P101_ALLOC_LOG_LINE_MAX];
    char   number[P101_NUMBER_BUF_LEN];
    char   pointer[P101_POINTER_BUF_LEN];
    size_t offset;

    (void)env;
    stream = (FILE *)user_data;

    if(stream == NULL)
    {
        return;
    }

    offset  = 0;
    line[0] = '\0';

    p101_env_log_append_text(line, sizeof(line), &offset, "P101ALLOC\t1\t");
    snprintf(number, sizeof(number), "%" PRIdMAX, (intmax_t)getpid());    // NOLINT(cert-err33-c)
    p101_env_log_append_text(line, sizeof(line), &offset, number);
    p101_env_log_append_char(line, sizeof(line), &offset, '\t');
    p101_env_log_append_text(line, sizeof(line), &offset, p101_env_alloc_event_name(event));
    p101_env_log_append_char(line, sizeof(line), &offset, '\t');
    snprintf(pointer, sizeof(pointer), "%p", ptr);    // NOLINT(cert-err33-c)
    p101_env_log_append_text(line, sizeof(line), &offset, pointer);
    p101_env_log_append_char(line, sizeof(line), &offset, '\t');

    if(new_ptr == NULL)
    {
        p101_env_log_append_char(line, sizeof(line), &offset, '-');
    }
    else
    {
        snprintf(pointer, sizeof(pointer), "%p", new_ptr);    // NOLINT(cert-err33-c)
        p101_env_log_append_text(line, sizeof(line), &offset, pointer);
    }

    p101_env_log_append_char(line, sizeof(line), &offset, '\t');
    snprintf(number, sizeof(number), "%zu", size);    // NOLINT(cert-err33-c)
    p101_env_log_append_text(line, sizeof(line), &offset, number);
    p101_env_log_append_char(line, sizeof(line), &offset, '\t');
    snprintf(number, sizeof(number), "%d", line_number);    // NOLINT(cert-err33-c)
    p101_env_log_append_text(line, sizeof(line), &offset, number);
    p101_env_log_append_char(line, sizeof(line), &offset, '\t');
    p101_env_log_append_field(line, sizeof(line), &offset, function_name);
    p101_env_log_append_char(line, sizeof(line), &offset, '\t');
    p101_env_log_append_field(line, sizeof(line), &offset, file_name);
    p101_env_log_append_char(line, sizeof(line), &offset, '\n');

    fwrite(line, 1, offset, stream);    // NOLINT(cert-err33-c)
    fflush(stream);                     // NOLINT(cert-err33-c)
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
    FILE       *stream;
    char        line[P101_CALL_LOG_LINE_MAX];
    char        number[P101_NUMBER_BUF_LEN];
    const char *logged_arguments;
    const char *logged_result;
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
    offset           = 0;
    line[0]          = '\0';

    p101_env_log_append_text(line, sizeof(line), &offset, "P101CALL\t1\t");
    snprintf(number, sizeof(number), "%" PRIdMAX, (intmax_t)getpid());    // NOLINT(cert-err33-c)
    p101_env_log_append_text(line, sizeof(line), &offset, number);
    p101_env_log_append_char(line, sizeof(line), &offset, '\t');
    p101_env_log_append_text(line, sizeof(line), &offset, (event == P101_ENV_CALL_ENTER) ? "ENTER" : "EXIT");
    p101_env_log_append_char(line, sizeof(line), &offset, '\t');
    snprintf(number, sizeof(number), "%d", line_number);    // NOLINT(cert-err33-c)
    p101_env_log_append_text(line, sizeof(line), &offset, number);
    p101_env_log_append_char(line, sizeof(line), &offset, '\t');
    p101_env_log_append_field(line, sizeof(line), &offset, function_name);
    p101_env_log_append_char(line, sizeof(line), &offset, '\t');
    p101_env_log_append_field(line, sizeof(line), &offset, (call_name == NULL) ? function_name : call_name);
    p101_env_log_append_char(line, sizeof(line), &offset, '\t');
    p101_env_log_append_field(line, sizeof(line), &offset, logged_arguments);
    p101_env_log_append_char(line, sizeof(line), &offset, '\t');
    p101_env_log_append_field(line, sizeof(line), &offset, logged_result);
    p101_env_log_append_char(line, sizeof(line), &offset, '\t');
    p101_env_log_append_field(line, sizeof(line), &offset, file_name);
    p101_env_log_append_char(line, sizeof(line), &offset, '\n');

    fwrite(line, 1, offset, stream);    // NOLINT(cert-err33-c)
    fflush(stream);                     // NOLINT(cert-err33-c)
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
