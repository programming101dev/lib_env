#ifndef LIBP101_ENV_WRAPPER_H
#define LIBP101_ENV_WRAPPER_H

/*
 * Copyright 2026 D'Arcy Smith.
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

#include <p101_env/env.h>
#include <stddef.h>

/*
 * Shared mechanics for authors of p101-style wrappers. The wrapped function
 * name is derived from the required p101_ prefix.
 */
// clang-format off
#define P101_WRAPPER_FAULT_RETURN(env, err, failure_value)               \
    do                                                                  \
    {                                                                   \
        int p101_wrapper_fault_code_;                                   \
                                                                        \
        p101_wrapper_fault_code_ = p101_env_check_fault((env), __func__ + 5); \
        if(p101_wrapper_fault_code_ != 0)                               \
        {                                                               \
            P101_ERROR_RAISE_ERRNO((err), p101_wrapper_fault_code_);    \
            P101_TRACE_EXIT(env);                                       \
            return (failure_value);                                     \
        }                                                               \
    } while(0)

#define P101_WRAPPER_FAULT_RETURN_CODE(env, err)                         \
    do                                                                  \
    {                                                                   \
        int p101_wrapper_fault_code_;                                   \
                                                                        \
        p101_wrapper_fault_code_ = p101_env_check_fault((env), __func__ + 5); \
        if(p101_wrapper_fault_code_ != 0)                               \
        {                                                               \
            P101_ERROR_RAISE_ERRNO((err), p101_wrapper_fault_code_);    \
            P101_TRACE_EXIT(env);                                       \
            return p101_wrapper_fault_code_;                            \
        }                                                               \
    } while(0)
// clang-format on

static inline size_t p101_wrapper_short_count(size_t requested, size_t amount)
{
    if(requested == 0U)
    {
        return 0U;
    }
    if(amount < requested)
    {
        return amount;
    }
    return requested - 1U;
}

#endif    // LIBP101_ENV_WRAPPER_H
