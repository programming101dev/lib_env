#ifndef LIBP101_ENV_ENV_H
#define LIBP101_ENV_ENV_H

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

#include <p101_error/error.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

    struct p101_env;

    typedef void (*p101_env_tracer)(const struct p101_env *env, const char *file_name, const char *function_name, size_t line_number);

    /**
     *
     * @param err
     * @param zero_free
     * @param tracer
     */
    struct p101_env *p101_env_create(struct p101_error *err, bool zero_free, p101_env_tracer tracer);

    /**
     *
     * @param err
     * @param env
     * @return
     */
    struct p101_env *p101_env_dup(struct p101_error *err, const struct p101_env *env);

    /**
     *
     * @param env
     * @return
     */
    bool p101_env_is_zero_free(const struct p101_env *env);

    /**
     *
     * @param env
     * @return
     */
    p101_env_tracer p101_env_get_tracer(const struct p101_env *env);

    /**
     *
     * @param env
     * @param on
     */
    void p101_env_set_zero_free(struct p101_env *env, bool on);

    /**
     *
     * @param env
     * @param tracer
     */
    void p101_env_set_tracer(struct p101_env *env, p101_env_tracer tracer);

    /**
     *
     * @param env
     * @param file_name
     * @param function_name
     * @param line_number
     */
    void p101_env_default_tracer(const struct p101_env *env, const char *file_name, const char *function_name, size_t line_number);

    /**
     *
     * @param env
     * @param file_name
     * @param function_name
     * @param line_number
     */
    void p101_env_trace(const struct p101_env *env, const char *file_name, const char *function_name, size_t line_number);

#define P101_TRACE(env) p101_env_trace((env), __FILE__, __func__, __LINE__)

#ifdef __cplusplus
}
#endif

#endif    // LIBP101_ENV_ENV_H
