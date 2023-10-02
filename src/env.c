/*
 * Copyright 2021-2022 D'Arcy Smith.
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
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>


static void p101_env_init(struct p101_env *env, bool zero_free, p101_env_tracer tracer);


struct p101_env
{
    bool zero_free;
    p101_env_tracer tracer;
};


struct p101_env *p101_env_create(struct p101_error *err, bool zero_free, p101_env_tracer tracer)
{
    struct p101_env *env;

    env = (struct p101_env *)malloc(sizeof(struct p101_env));  // NOLINT(clang-analyzer-unix.Malloc)

    if(env == NULL)
    {
        P101_ERROR_RAISE_ERRNO(err, errno);
    }
    else
    {
        p101_env_init(env, zero_free, tracer);
    }

    return env;
}

struct p101_env *p101_env_dup(struct p101_error *err, const struct p101_env *env)
{
    struct p101_env *new_env;

    new_env = (struct p101_env *)malloc(sizeof(struct p101_env));  // NOLINT(clang-analyzer-unix.Malloc)

    if(new_env == NULL)
    {
        P101_ERROR_RAISE_ERRNO(err, errno);
    }
    else
    {
        p101_env_init(new_env, env->zero_free, env->tracer);
    }

    return new_env;
}

static void p101_env_init(struct p101_env *env, bool zero_free, p101_env_tracer tracer)
{
    env->zero_free = zero_free;
    env->tracer = tracer;
}

bool p101_env_is_zero_free(const struct p101_env *env)
{
    P101_TRACE(env);

    return env->zero_free;
}

p101_env_tracer p101_env_get_tracer(const struct p101_env *env)
{
    P101_TRACE(env);

    return env->tracer;
}

void p101_env_set_zero_free(struct p101_env *env, bool on)
{
    P101_TRACE(env);

    env->zero_free = on;
}

void p101_env_set_tracer(struct p101_env *env, p101_env_tracer tracer)
{
    P101_TRACE(env);

    env->tracer = tracer;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
void p101_env_default_tracer(const struct p101_env *env,
                             const char *file_name,
                             const char *function_name,
                             size_t line_number)
{
    fprintf(stdout, "TRACE (pid=%d): %s : %s : @ %zu\n", getpid(), file_name, function_name, line_number); // NOLINT(cert-err33-c)
}
#pragma GCC diagnostic pop

void p101_env_trace(const struct p101_env *env, const char *file_name, const char *function_name, size_t line_number)
{
    if(env->tracer != NULL)
    {
        env->tracer(env, file_name, function_name, line_number);
    }
}
