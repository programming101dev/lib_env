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
#include <stdatomic.h>
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
    unsigned long       target_call;
    atomic_ulong        calls_seen;
    unsigned long       repeat;
    int                 errnum;
    p101_env_fault_kind kind;
    const char         *mode_name;
    size_t              amount;
    char               *call_name;
    FILE               *log_stream;
    char               *log_path;
    int                 log_owned;
};

struct p101_env_event_state
{
    unsigned long long context_id;
    atomic_ullong      next_sequence;
    atomic_int         write_failed;
    atomic_int         write_errno;
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
    p101_env_resource_observer   resource_observer;
    void                        *resource_observer_data; /* non-owned; state for the observer */
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
    P101_POINTER_BUF_LEN         = 64,
    P101_NANOSECONDS_PER_SECOND  = 1000000000,
    P101_ENV_NUMBER_BASE         = 10,
    P101_EXEC_SCAN_FD_FALLBACK   = 65536,
    P101_TOOL_EVENT_PARSE_FD_MAX = 1048576,
    P101_DEFAULT_FAULT_ERRNO     = EIO
};

static void                         p101_env_init(struct p101_env *env, p101_env_tracer tracer);
static void                         p101_env_init_event_state(struct p101_env *env, struct p101_error *err);
static void                         p101_env_configure_from_environment(struct p101_env *env, struct p101_error *err);
static void                         p101_env_configure_event_log_version_from_environment(struct p101_env *env, struct p101_error *err);
static void                         p101_env_configure_fault_from_environment(struct p101_env *env, struct p101_error *err);
static void                         p101_env_configure_fd_log_from_environment(struct p101_env *env, struct p101_error *err);
static void                         p101_env_configure_call_log_from_environment(struct p101_env *env, struct p101_error *err);
static void                         p101_env_configure_resource_log_path(struct p101_env *env, struct p101_error *err, const char *path, int enable_fd, int enable_alloc, int enable_resource);
static void                         p101_env_configure_call_log_path(struct p101_env *env, struct p101_error *err, const char *path, unsigned options);
static FILE                        *p101_env_open_log_from_environment(struct p101_error *err, const char *path, int *owned);
static char                        *p101_env_copy_text(struct p101_error *err, const char *text);
static void                         p101_env_close_owned_resource_log(struct p101_env *env);
static void                         p101_env_close_owned_resource_log_if_unused(struct p101_env *env);
static void                         p101_env_close_owned_call_log(struct p101_env *env);
static struct p101_env_fault_state *p101_env_fault_state_dup(struct p101_error *err, const struct p101_env_fault_state *source);
static void                         p101_env_fault_state_destroy(struct p101_env_fault_state *state);
static void                         p101_env_log_fault_hit(const struct p101_env *env, const struct p101_env_fault_state *state, const char *call_name);
static int                          p101_env_environment_fault_injector(const struct p101_env *env, const char *call_name, void *user_data);
static int                          p101_env_environment_fault_action(const struct p101_env *env, const char *call_name, void *user_data, struct p101_env_fault_action *action);
static unsigned long                p101_env_parse_unsigned_environment(const char *text, unsigned long default_value, int *ok);
static int                          p101_env_parse_int_environment(const char *text, int default_value, int *ok);
static int                          p101_env_flag_on(const char *name, int default_value);
static int                          p101_env_supported_event_log_version(long version);
static int                          p101_env_effective_event_log_version(const struct p101_env *env);
static unsigned long long           p101_env_next_event_sequence(const struct p101_env *env);
static void                         p101_env_prepare_event_record(const struct p101_env *env, struct p101_tool_event_output *record, p101_tool_event_record_kind kind, long pid);
static void                         p101_env_record_event_write_failure(const struct p101_env *env);
static void                         p101_env_write_event(const struct p101_env *env, FILE *stream, const struct p101_tool_event_output *record);
static void                         p101_env_fd_notify(const struct p101_env *env, p101_env_fd_event event, int fd, const char *file_name, const char *function_name, int line_number);
static void                         p101_env_fd_log_observer(const struct p101_env *env, p101_env_fd_event event, int fd, const char *file_name, const char *function_name, int line_number, void *user_data);
static void                         p101_env_fork_log(const struct p101_env *env, FILE *stream, long parent_pid, long child_pid, const char *file_name, const char *function_name, int line_number);
static void                         p101_env_spawn_log(const struct p101_env *env, FILE *stream, long parent_pid, long child_pid, const char *target, const char *file_name, const char *function_name, int line_number);
static long                         p101_env_exec_scan_limit(void);
static void                         p101_env_exec_fd_log(const struct p101_env *env, FILE *stream, int fd, int cloexec, const char *target, const char *file_name, const char *function_name, int line_number);
static void                         p101_env_exec_failure_log(const struct p101_env *env, FILE *stream, const char *target, const char *file_name, const char *function_name, int line_number);
static void                         p101_env_alloc_notify(const struct p101_env *env, p101_env_alloc_event event, const void *ptr, const void *new_ptr, size_t size, const char *file_name, const char *function_name, int line_number);
static void                         p101_env_alloc_log_observer(const struct p101_env *env, p101_env_alloc_event event, const void *ptr, const void *new_ptr, size_t size, const char *file_name, const char *function_name, int line_number, void *user_data);
static void p101_env_resource_log_observer(const struct p101_env *env, p101_tool_event_resource_kind event, const char *resource_class, const char *resource_id, const char *related_id, size_t size, const char *metadata, const char *file_name,
                                           const char *function_name, int line_number, void *user_data);
static void p101_env_call_notify(const struct p101_env *env, p101_env_call_event event, const char *call_name, const char *arguments, const char *result, const char *file_name, const char *function_name, int line_number);
static void p101_env_call_log_observer(const struct p101_env *env, p101_env_call_event event, const char *call_name, const char *arguments, const char *result, const char *file_name, const char *function_name, int line_number, void *user_data);

static atomic_ullong p101_env_next_context_id = 0;    // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

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
        new_env->fd_observer            = env->fd_observer;
        new_env->fd_observer_data       = env->fd_observer_data;
        new_env->alloc_observer         = env->alloc_observer;
        new_env->alloc_observer_data    = env->alloc_observer_data;
        new_env->resource_observer      = env->resource_observer;
        new_env->resource_observer_data = env->resource_observer_data;

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
            int enable_resource;

            enable_fd       = (env->fd_observer == p101_env_fd_log_observer && env->fd_observer_data == env->owned_fd_log_stream) ? 1 : 0;
            enable_alloc    = (env->alloc_observer == p101_env_alloc_log_observer && env->alloc_observer_data == env->owned_fd_log_stream) ? 1 : 0;
            enable_resource = (env->resource_observer == p101_env_resource_log_observer && env->resource_observer_data == env->owned_fd_log_stream) ? 1 : 0;
            p101_env_configure_resource_log_path(new_env, err, env->owned_fd_log_path, enable_fd, enable_alloc, enable_resource);
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
    if(p101_env_event_log_failed(env))
    {
        int write_error;

        write_error = p101_env_event_log_errno(env);
        flockfile(stderr);
        fprintf(stderr, "p101 instrumentation log is incomplete: write failed with errno %d\n", write_error);    // NOLINT(cert-err33-c)
        fflush(stderr);                                                                                          // NOLINT(cert-err33-c)
        funlockfile(stderr);
    }

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
    env->tracer                 = tracer;
    env->exit_tracer            = NULL;
    env->tracer_data            = NULL;
    env->label                  = NULL;
    env->fault_injector         = NULL;
    env->fault_data             = NULL;
    env->fd_ledger              = NULL;
    env->fd_observer            = NULL;
    env->fd_observer_data       = NULL;
    env->alloc_observer         = NULL;
    env->alloc_observer_data    = NULL;
    env->resource_observer      = NULL;
    env->resource_observer_data = NULL;
    env->call_observer          = NULL;
    env->call_observer_data     = NULL;
    env->call_log_options       = P101_ENV_CALL_LOG_DEFAULT;
    env->event_log_version      = P101_TOOL_EVENT_LOG_VERSION_3;
    env->event_state            = NULL;
    env->owned_fd_log_stream    = NULL;
    env->owned_fd_log_path      = NULL;
    env->owned_call_log_stream  = NULL;
    env->owned_call_log_path    = NULL;
    env->owned_fault_state      = NULL;
}

static void p101_env_init_event_state(struct p101_env *env, struct p101_error *err)
{
    env->event_state = (struct p101_env_event_state *)malloc(sizeof(struct p101_env_event_state));

    if(env->event_state == NULL)
    {
        P101_ERROR_RAISE_ERRNO(err, errno);
        return;
    }

    env->event_state->context_id = atomic_fetch_add_explicit(&p101_env_next_context_id, 1ULL, memory_order_relaxed) + 1ULL;
    atomic_init(&env->event_state->next_sequence, 0ULL);
    atomic_init(&env->event_state->write_failed, 0);
    atomic_init(&env->event_state->write_errno, 0);
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

    version_text = getenv("P101_TOOL_EVENT_LOG_VERSION");

    if(version_text == NULL || version_text[0] == '\0')
    {
        return;
    }

    version = p101_env_parse_int_environment(version_text, P101_TOOL_EVENT_LOG_VERSION_3, &ok);

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
    const char                  *mode_text;
    const char                  *amount_text;
    const char                  *repeat_text;
    struct p101_env_fault_state *state;
    unsigned long                target_call;
    int                          fault_errno;
    int                          log_owned;
    int                          ok;
    unsigned long                amount;
    unsigned long                repeat;

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
    atomic_init(&state->calls_seen, 0UL);
    state->repeat     = 1UL;
    state->errnum     = fault_errno;
    state->kind       = P101_ENV_FAULT_ERROR;
    state->mode_name  = "error";
    state->amount     = 1U;
    state->call_name  = NULL;
    state->log_stream = NULL;
    state->log_path   = NULL;
    state->log_owned  = 0;

    mode_text = getenv("P101_FAULT_MODE");
    if(mode_text != NULL && mode_text[0] != '\0')
    {
        if(strcmp(mode_text, "error") == 0)
        {
            state->kind = P101_ENV_FAULT_ERROR;
        }
        else if(strcmp(mode_text, "eintr") == 0)
        {
            state->kind      = P101_ENV_FAULT_ERROR;
            state->errnum    = EINTR;
            state->mode_name = "eintr";
        }
        else if(strcmp(mode_text, "timeout") == 0)
        {
            state->kind      = P101_ENV_FAULT_ERROR;
            state->errnum    = ETIMEDOUT;
            state->mode_name = "timeout";
        }
        else if(strcmp(mode_text, "short") == 0)
        {
            state->kind      = P101_ENV_FAULT_SHORT;
            state->mode_name = "short";
        }
        else
        {
            P101_ERROR_RAISE_ERRNO(err, EINVAL);
            p101_env_fault_state_destroy(state);
            return;
        }
    }

    amount_text = getenv("P101_FAULT_AMOUNT");
    amount      = p101_env_parse_unsigned_environment(amount_text, 1UL, &ok);
    if(!ok)
    {
        P101_ERROR_RAISE_ERRNO(err, EINVAL);
        p101_env_fault_state_destroy(state);
        return;
    }
    state->amount = amount;

    repeat_text = getenv("P101_FAULT_REPEAT");
    repeat      = p101_env_parse_unsigned_environment(repeat_text, 1UL, &ok);
    if(!ok || repeat == 0UL)
    {
        P101_ERROR_RAISE_ERRNO(err, EINVAL);
        p101_env_fault_state_destroy(state);
        return;
    }
    state->repeat = repeat;

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

    p101_env_configure_resource_log_path(env, err, path, 1, 1, 1);
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

static void p101_env_configure_resource_log_path(struct p101_env *env, struct p101_error *err, const char *path, int enable_fd, int enable_alloc, int enable_resource)
{
    FILE *stream;
    char *path_copy;
    int   owned;

    if(path == NULL || (!enable_fd && !enable_alloc && !enable_resource))
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

    if(enable_resource)
    {
        env->resource_observer      = p101_env_resource_log_observer;
        env->resource_observer_data = stream;
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

    if(env->resource_observer_data == stream)
    {
        env->resource_observer      = NULL;
        env->resource_observer_data = NULL;
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
    if(env->fd_observer_data == stream || env->alloc_observer_data == stream || env->resource_observer_data == stream)
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
    atomic_init(&state->calls_seen, 0UL);
    state->repeat     = source->repeat;
    state->errnum     = source->errnum;
    state->kind       = source->kind;
    state->mode_name  = source->mode_name;
    state->amount     = source->amount;
    state->call_name  = NULL;
    state->log_stream = NULL;
    state->log_path   = NULL;
    state->log_owned  = 0;

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

static void p101_env_log_fault_hit(const struct p101_env *env, const struct p101_env_fault_state *state, const char *call_name)
{
    unsigned long calls_seen;
    int           write_result;

    if(state == NULL || state->log_stream == NULL)
    {
        return;
    }

    calls_seen = atomic_load_explicit(&state->calls_seen, memory_order_relaxed);
    flockfile(state->log_stream);
    write_result = fprintf(state->log_stream, "P101FAULT\t2\t%" PRIdMAX "\t%lu\t%s\t%d\t%s\t%zu\n", (intmax_t)getpid(), calls_seen, (call_name == NULL) ? "?" : call_name, state->errnum, state->mode_name,
                           state->amount);    // NOLINT(cert-err33-c)
    if(write_result < 0 || fflush(state->log_stream) == EOF)
    {
        p101_env_record_event_write_failure(env);
    }
    funlockfile(state->log_stream);
}

static int p101_env_environment_fault_action(const struct p101_env *env, const char *call_name, void *user_data, struct p101_env_fault_action *action)
{
    struct p101_env_fault_state *state;
    unsigned long                calls_seen;

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

    calls_seen = atomic_fetch_add_explicit(&state->calls_seen, 1UL, memory_order_relaxed) + 1UL;

    if(calls_seen >= state->target_call && calls_seen - state->target_call < state->repeat)
    {
        p101_env_log_fault_hit(env, state, call_name);
        action->kind   = state->kind;
        action->errnum = state->errnum;
        action->amount = state->amount;
        return 1;
    }

    return 0;
}

static int p101_env_environment_fault_injector(const struct p101_env *env, const char *call_name, void *user_data)
{
    struct p101_env_fault_action action;

    if(!p101_env_environment_fault_action(env, call_name, user_data, &action))
    {
        return 0;
    }
    return action.kind == P101_ENV_FAULT_ERROR ? action.errnum : ENOTSUP;
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

    version = P101_TOOL_EVENT_LOG_VERSION_3;

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

int p101_env_check_fault_action(const struct p101_env *env, const char *call_name, struct p101_env_fault_action *action)
{
    int errnum;

    if(action == NULL)
    {
        return 0;
    }
    action->kind   = P101_ENV_FAULT_NONE;
    action->errnum = 0;
    action->amount = 0U;

    if(env == NULL || env->fault_injector == NULL)
    {
        return 0;
    }
    if(env->owned_fault_state != NULL && env->fault_injector == p101_env_environment_fault_injector)
    {
        return p101_env_environment_fault_action(env, call_name, env->fault_data, action);
    }

    errnum = env->fault_injector(env, call_name, env->fault_data);
    if(errnum == 0)
    {
        return 0;
    }
    action->kind   = P101_ENV_FAULT_ERROR;
    action->errnum = errnum;
    return 1;
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

/* cppcheck-suppress funcArgNamesDifferentUnnamed */
void p101_env_set_resource_observer(struct p101_env *env, p101_env_resource_observer observer, void *user_data)
{
    p101_env_trace(env, __FILE__, __func__, __LINE__);

    if(env == NULL)
    {
        return;
    }

    env->resource_observer      = observer;
    env->resource_observer_data = user_data;
    p101_env_close_owned_resource_log_if_unused(env);
}

void p101_env_set_resource_log(struct p101_env *env, FILE *stream)
{
    p101_env_trace(env, __FILE__, __func__, __LINE__);

    if(env == NULL)
    {
        return;
    }

    if(stream == NULL)
    {
        env->resource_observer      = NULL;
        env->resource_observer_data = NULL;
        p101_env_close_owned_resource_log_if_unused(env);
        return;
    }

    env->resource_observer      = p101_env_resource_log_observer;
    env->resource_observer_data = stream;
    p101_env_close_owned_resource_log_if_unused(env);
}

static int p101_env_supported_event_log_version(long version)
{
    return (version == P101_TOOL_EVENT_LOG_VERSION_3) ? 1 : 0;
}

static int p101_env_effective_event_log_version(const struct p101_env *env)
{
    if(env == NULL)
    {
        return P101_TOOL_EVENT_LOG_VERSION_3;
    }

    return env->event_log_version;
}

static unsigned long long p101_env_next_event_sequence(const struct p101_env *env)
{
    if(env != NULL && env->event_state != NULL)
    {
        return atomic_fetch_add_explicit(&env->event_state->next_sequence, 1ULL, memory_order_relaxed) + 1ULL;
    }

    return 0ULL;
}

int p101_env_event_log_failed(const struct p101_env *env)
{
    return (env != NULL && env->event_state != NULL) ? atomic_load_explicit(&env->event_state->write_failed, memory_order_acquire) : 0;
}

int p101_env_event_log_errno(const struct p101_env *env)
{
    return (env != NULL && env->event_state != NULL) ? atomic_load_explicit(&env->event_state->write_errno, memory_order_relaxed) : 0;
}

void p101_env_clear_event_log_error(struct p101_env *env)
{
    if(env != NULL && env->event_state != NULL)
    {
        atomic_store_explicit(&env->event_state->write_errno, 0, memory_order_relaxed);
        atomic_store_explicit(&env->event_state->write_failed, 0, memory_order_release);
    }
}

static void p101_env_record_event_write_failure(const struct p101_env *env)
{
    int actual_error;

    if(env == NULL || env->event_state == NULL)
    {
        return;
    }

    actual_error = errno == 0 ? EIO : errno;
    atomic_store_explicit(&env->event_state->write_errno, actual_error, memory_order_relaxed);
    atomic_store_explicit(&env->event_state->write_failed, 1, memory_order_release);
}

static void p101_env_write_event(const struct p101_env *env, FILE *stream, const struct p101_tool_event_output *record)
{
    if(p101_tool_event_write(stream, record) != 0)
    {
        p101_env_record_event_write_failure(env);
    }
}

static void p101_env_prepare_event_record(const struct p101_env *env, struct p101_tool_event_output *record, p101_tool_event_record_kind kind, long pid)
{
    struct timespec monotonic;
    struct timespec wall;

    memset(record, 0, sizeof(*record));
    record->version     = p101_env_effective_event_log_version(env);
    record->record_kind = kind;
    record->pid         = pid;
    record->context_id  = (env == NULL || env->event_state == NULL) ? 0U : (size_t)env->event_state->context_id;
    record->sequence    = (size_t)p101_env_next_event_sequence(env);

    if(clock_gettime(CLOCK_MONOTONIC, &monotonic) == 0 && monotonic.tv_sec >= 0 && (unsigned long long)monotonic.tv_sec <= (SIZE_MAX / P101_NANOSECONDS_PER_SECOND))
    {
        record->monotonic_ns           = ((size_t)monotonic.tv_sec * P101_NANOSECONDS_PER_SECOND) + (size_t)monotonic.tv_nsec;
        record->monotonic_ns_available = 1;
    }
    if(clock_gettime(CLOCK_REALTIME, &wall) == 0 && wall.tv_sec >= 0 && (unsigned long long)wall.tv_sec <= (SIZE_MAX / P101_NANOSECONDS_PER_SECOND))
    {
        record->wall_unix_ns           = ((size_t)wall.tv_sec * P101_NANOSECONDS_PER_SECOND) + (size_t)wall.tv_nsec;
        record->wall_unix_ns_available = 1;
    }
}

static void p101_env_fd_log_observer(const struct p101_env *env, p101_env_fd_event event, int fd, const char *file_name, const char *function_name, int line_number, void *user_data)
{
    struct p101_tool_event_output record;
    FILE                         *stream;

    stream = (FILE *)user_data;

    if(stream == NULL)
    {
        return;
    }

    /* getpid() on every line rather than once at install time: after a fork
     * the child keeps writing to the same stream, and the analyzer has to be
     * able to tell the two processes' descriptors apart. */
    p101_env_prepare_event_record(env, &record, P101_TOOL_EVENT_RECORD_FD, (long)getpid());
    record.fd_kind       = event == P101_ENV_FD_OPEN ? P101_TOOL_EVENT_FD_OPEN : P101_TOOL_EVENT_FD_CLOSE;
    record.fd            = fd;
    record.line_number   = line_number;
    record.function_name = function_name;
    record.file_name     = file_name;
    p101_env_write_event(env, stream, &record);
}

static void p101_env_fork_log(const struct p101_env *env, FILE *stream, long parent_pid, long child_pid, const char *file_name, const char *function_name, int line_number)
{
    struct p101_tool_event_output record;

    if(stream == NULL)
    {
        return;
    }

    p101_env_prepare_event_record(env, &record, P101_TOOL_EVENT_RECORD_FORK, parent_pid);
    record.child_pid     = child_pid;
    record.line_number   = line_number;
    record.function_name = function_name;
    record.file_name     = file_name;
    p101_env_write_event(env, stream, &record);
}

static void p101_env_spawn_log(const struct p101_env *env, FILE *stream, long parent_pid, long child_pid, const char *target, const char *file_name, const char *function_name, int line_number)
{
    struct p101_tool_event_output record;

    if(stream == NULL)
    {
        return;
    }

    p101_env_prepare_event_record(env, &record, P101_TOOL_EVENT_RECORD_SPAWN, parent_pid);
    record.child_pid     = child_pid;
    record.target        = target;
    record.line_number   = line_number;
    record.function_name = function_name;
    record.file_name     = file_name;
    p101_env_write_event(env, stream, &record);
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
    struct p101_tool_event_output record;

    if(stream == NULL)
    {
        return;
    }

    p101_env_prepare_event_record(env, &record, P101_TOOL_EVENT_RECORD_EXEC, (long)getpid());
    record.fd            = fd;
    record.cloexec       = cloexec;
    record.target        = target;
    record.line_number   = line_number;
    record.function_name = function_name;
    record.file_name     = file_name;
    p101_env_write_event(env, stream, &record);
}

static void p101_env_exec_failure_log(const struct p101_env *env, FILE *stream, const char *target, const char *file_name, const char *function_name, int line_number)
{
    struct p101_tool_event_output record;

    if(stream == NULL)
    {
        return;
    }

    p101_env_prepare_event_record(env, &record, P101_TOOL_EVENT_RECORD_EXEC_FAIL, (long)getpid());
    record.target        = target;
    record.line_number   = line_number;
    record.function_name = function_name;
    record.file_name     = file_name;
    p101_env_write_event(env, stream, &record);
}

static void p101_env_alloc_log_observer(const struct p101_env *env, p101_env_alloc_event event, const void *ptr, const void *new_ptr, size_t size, const char *file_name, const char *function_name, int line_number, void *user_data)
{
    struct p101_tool_event_output record;
    FILE                         *stream;
    char                          pointer[P101_POINTER_BUF_LEN];
    char                          new_pointer[P101_POINTER_BUF_LEN];

    (void)env;
    stream = (FILE *)user_data;

    if(stream == NULL)
    {
        return;
    }

    snprintf(pointer, sizeof(pointer), "%p", ptr);    // NOLINT(cert-err33-c)
    if(new_ptr != NULL)
    {
        snprintf(new_pointer, sizeof(new_pointer), "%p", new_ptr);    // NOLINT(cert-err33-c)
    }

    p101_env_prepare_event_record(env, &record, P101_TOOL_EVENT_RECORD_ALLOC, (long)getpid());
    record.alloc_kind = P101_TOOL_EVENT_ALLOC_REALLOC;
    if(event == P101_ENV_ALLOC_ALLOC)
    {
        record.alloc_kind = P101_TOOL_EVENT_ALLOC_ALLOC;
    }
    else if(event == P101_ENV_ALLOC_FREE)
    {
        record.alloc_kind = P101_TOOL_EVENT_ALLOC_FREE;
    }
    record.ptr           = pointer;
    record.new_ptr       = new_ptr == NULL ? NULL : new_pointer;
    record.size          = size;
    record.line_number   = line_number;
    record.function_name = function_name;
    record.file_name     = file_name;
    p101_env_write_event(env, stream, &record);
}

static void p101_env_resource_log_observer(const struct p101_env *env, p101_tool_event_resource_kind event, const char *resource_class, const char *resource_id, const char *related_id, size_t size, const char *metadata, const char *file_name,
                                           const char *function_name, int line_number, void *user_data)
{
    struct p101_tool_event_output record;
    FILE                         *stream;

    stream = (FILE *)user_data;
    if(stream == NULL)
    {
        return;
    }

    p101_env_prepare_event_record(env, &record, P101_TOOL_EVENT_RECORD_RESOURCE, (long)getpid());
    record.resource_kind  = event;
    record.resource_class = resource_class;
    record.resource_id    = resource_id;
    record.related_id     = related_id;
    record.size           = size;
    record.metadata       = metadata;
    record.line_number    = line_number;
    record.function_name  = function_name;
    record.file_name      = file_name;
    p101_env_write_event(env, stream, &record);
}

static void p101_env_call_log_observer(const struct p101_env *env, p101_env_call_event event, const char *call_name, const char *arguments, const char *result, const char *file_name, const char *function_name, int line_number, void *user_data)
{
    struct p101_tool_event_output record;
    FILE                         *stream;
    const char                   *logged_arguments;
    const char                   *logged_result;

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
    p101_env_prepare_event_record(env, &record, P101_TOOL_EVENT_RECORD_CALL, (long)getpid());
    record.call_kind     = event == P101_ENV_CALL_ENTER ? P101_TOOL_EVENT_CALL_ENTER : P101_TOOL_EVENT_CALL_EXIT;
    record.line_number   = line_number;
    record.function_name = function_name;
    record.call_name     = call_name == NULL ? function_name : call_name;
    record.arguments     = logged_arguments;
    record.result        = logged_result;
    record.file_name     = file_name;
    p101_env_write_event(env, stream, &record);
}

static void p101_env_call_notify(const struct p101_env *env, p101_env_call_event event, const char *call_name, const char *arguments, const char *result, const char *file_name, const char *function_name, int line_number)
{
    if(env != NULL && env->call_observer != NULL)
    {
        env->call_observer(env, event, call_name, arguments, result, file_name, function_name, line_number, env->call_observer_data);
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

void p101_env_track_resource(const struct p101_env *env, p101_tool_event_resource_kind event, const char *resource_class, const char *resource_id, const char *related_id, size_t size, const char *metadata, const char *file_name, const char *function_name,
                             int line_number)
{
    if(env == NULL || env->resource_observer == NULL || resource_class == NULL || resource_class[0] == '\0' || resource_id == NULL || resource_id[0] == '\0')
    {
        return;
    }

    env->resource_observer(env, event, resource_class, resource_id, related_id, size, metadata, file_name, function_name, line_number, env->resource_observer_data);
}

void p101_env_pointer_resource_id(char *text, size_t text_size, const void *resource)
{
    if(text == NULL || text_size < P101_ENV_POINTER_RESOURCE_ID_SIZE)
    {
        return;
    }

    snprintf(text, P101_ENV_POINTER_RESOURCE_ID_SIZE, "%p", resource);    // NOLINT(cert-err33-c)
}

void p101_env_track_pointer_resource(const struct p101_env *env, p101_tool_event_resource_kind event, const char *resource_class, const void *resource, const void *related_resource, size_t size, const char *metadata, const char *file_name,
                                     const char *function_name, int line_number)
{
    char        resource_id[P101_POINTER_BUF_LEN];
    char        related_id[P101_POINTER_BUF_LEN];
    const char *related_text;

    if(resource == NULL)
    {
        return;
    }

    p101_env_pointer_resource_id(resource_id, sizeof(resource_id), resource);
    related_text = NULL;
    if(related_resource != NULL)
    {
        p101_env_pointer_resource_id(related_id, sizeof(related_id), related_resource);
        related_text = related_id;
    }
    p101_env_track_resource(env, event, resource_class, resource_id, related_text, size, metadata, file_name, function_name, line_number);
}

void p101_env_track_integer_resource(const struct p101_env *env, p101_tool_event_resource_kind event, const char *resource_class, intmax_t resource, intmax_t related_resource, size_t size, const char *metadata, const char *file_name, const char *function_name,
                                     int line_number)
{
    char        resource_id[P101_POINTER_BUF_LEN];
    char        related_id[P101_POINTER_BUF_LEN];
    const char *related_text;

    snprintf(resource_id, sizeof(resource_id), "%" PRIdMAX, resource);    // NOLINT(cert-err33-c)
    related_text = NULL;
    if(event == P101_TOOL_EVENT_RESOURCE_REPLACE || event == P101_TOOL_EVENT_RESOURCE_TRANSFER)
    {
        snprintf(related_id, sizeof(related_id), "%" PRIdMAX, related_resource);    // NOLINT(cert-err33-c)
        related_text = related_id;
    }
    p101_env_track_resource(env, event, resource_class, resource_id, related_text, size, metadata, file_name, function_name, line_number);
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
