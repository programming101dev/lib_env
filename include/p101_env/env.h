#ifndef LIBP101_ENV_ENV_H
#define LIBP101_ENV_ENV_H

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

/*
 * Thread-safety: mutable configuration, custom observers/tracers, and the
 * in-process descriptor ledger are caller-synchronized. The convention is one
 * env object per thread (p101_env_dup() exists to make that easy), matching the
 * one-error-per-thread convention in p101_error. After configuration, the
 * built-in event-log observers may be called concurrently: sequence allocation
 * is atomic and lib_tool_event serializes each complete record.
 */

#include <p101_error/error.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifndef P101_ATTR_MALLOC
    #if defined(__GNUC__) || defined(__clang__)
        #define P101_ATTR_MALLOC __attribute__((malloc))
    #else
        #define P101_ATTR_MALLOC
    #endif
#endif

#ifndef P101_ATTR_WARN_UNUSED_RESULT
    #if defined(__GNUC__) || defined(__clang__)
        #define P101_ATTR_WARN_UNUSED_RESULT __attribute__((warn_unused_result))
    #else
        #define P101_ATTR_WARN_UNUSED_RESULT
    #endif
#endif

#ifdef __cplusplus
extern "C"
{
#endif

/*
 * The environment variables this library reads. Every p101-owned name lives
 * here so launchers, wrappers, and analyzers spell it exactly once.
 */
#define P101_ENV_EVENT_RUN_ID_ENV "P101_EVENT_RUN_ID"
#define P101_ENV_FAULT_CALL_ENV "P101_FAULT_CALL"
#define P101_ENV_FAULT_ERRNO_ENV "P101_FAULT_ERRNO"
#define P101_ENV_FAULT_MODE_ENV "P101_FAULT_MODE"
#define P101_ENV_FAULT_AMOUNT_ENV "P101_FAULT_AMOUNT"
#define P101_ENV_FAULT_REPEAT_ENV "P101_FAULT_REPEAT"
#define P101_ENV_FAULT_NAME_ENV "P101_FAULT_NAME"
#define P101_ENV_FAULT_LOG_ENV "P101_FAULT_LOG"
#define P101_ENV_RESOURCE_LOG_ENV "P101_RESOURCE_LOG"
#define P101_ENV_CALL_LOG_ENV "P101_CALL_LOG"
#define P101_ENV_CALL_LOG_ENTER_ENV "P101_CALL_LOG_ENTER"
#define P101_ENV_CALL_LOG_EXIT_ENV "P101_CALL_LOG_EXIT"
#define P101_ENV_CALL_LOG_ARGS_ENV "P101_CALL_LOG_ARGS"
#define P101_ENV_CALL_LOG_RESULT_ENV "P101_CALL_LOG_RESULT"

    struct p101_env;

    /* line_number is int to match __LINE__ and p101_error's line numbers. */
    typedef void (*p101_env_tracer)(const struct p101_env *env, const char *file_name, const char *function_name, int line_number);

    typedef struct p101_env_trace_scope
    {
        const struct p101_env *env;
        const char            *file_name;
        const char            *function_name;
        int                    line_number;
    } p101_env_trace_scope P101_ATTR_SEMANTIC_ROLE("p101:trace-scope");

    struct p101_env *p101_env_create(struct p101_error *err, p101_env_tracer tracer) P101_ATTR_MALLOC P101_ATTR_WARN_UNUSED_RESULT P101_ATTR_SEMANTIC_ROLE("p101:ownership:env:acquire");
    struct p101_env *p101_env_dup(struct p101_error *err, const struct p101_env *env) P101_ATTR_MALLOC P101_ATTR_WARN_UNUSED_RESULT;
    void             p101_env_destroy(struct p101_env *env) P101_ATTR_SEMANTIC_ROLE("p101:ownership:env:release");
    p101_env_tracer  p101_env_get_tracer(const struct p101_env *env);
    void             p101_env_set_tracer(struct p101_env *env, p101_env_tracer tracer);
    void             p101_env_default_tracer(const struct p101_env *env, const char *file_name, const char *function_name, int line_number);
    void             p101_env_trace(const struct p101_env *env, const char *file_name, const char *function_name, int line_number) P101_ATTR_SEMANTIC_ROLE("p101:instrumentation:trace-entry");
    void             p101_env_trace_scope_cleanup(p101_env_trace_scope *scope) P101_ATTR_SEMANTIC_ROLE("p101:instrumentation:trace-exit");

    /* State for the INSTALLED TRACER only (call depth, an output stream, a
     * filter...) -- not a general-purpose stash for application data. The
     * getter deliberately does not trace, so it is safe to call from inside
     * a tracer without recursing. */
    void  p101_env_set_tracer_data(struct p101_env *env, void *data);
    void *p101_env_get_tracer_data(const struct p101_env *env);

    /* Exit tracing. P101_TRACE fires on function ENTRY; call P101_TRACE_EXIT at
     * every return point, or use P101_TRACE_SCOPE for an automatic exit on
     * ordinary scope departure. Cleanup handlers do not run after longjmp,
     * _Exit, abort, or process termination. */
    void            p101_env_set_exit_tracer(struct p101_env *env, p101_env_tracer tracer);
    p101_env_tracer p101_env_get_exit_tracer(const struct p101_env *env);
    void            p101_env_trace_exit(const struct p101_env *env, const char *file_name, const char *function_name, int line_number) P101_ATTR_SEMANTIC_ROLE("p101:instrumentation:trace-exit");

    /*
     * Structured call observation. This is the strace/ltrace-ish layer for p101
     * wrappers AND for user code that follows the same rules:
     *
     *   - call P101_CALL_ENTER() at entry and P101_CALL_EXIT() at every return;
     *   - pass small, already-formatted strings for arguments/results;
     *   - never call p101 wrappers from an observer, or it will recurse;
     *   - treat every string pointer as borrowed for the duration of the call.
     *
     * Plain P101_TRACE/P101_TRACE_EXIT also emits structured ENTER/EXIT records
     * with NULL arguments/results, so existing instrumented p101 wrappers and
     * user functions participate immediately. Use the P101_CALL_* macros when a
     * function wants to provide parameter or return-value text.
     */
    typedef enum
    {
        P101_ENV_CALL_ENTER = 0,
        P101_ENV_CALL_EXIT  = 1
    } p101_env_call_event;

    typedef enum
    {
        P101_ENV_CALL_LOG_ENTER     = 1U << 0U,
        P101_ENV_CALL_LOG_EXIT      = 1U << 1U,
        P101_ENV_CALL_LOG_ARGUMENTS = 1U << 2U,
        P101_ENV_CALL_LOG_RESULT    = 1U << 3U,
        P101_ENV_CALL_LOG_DEFAULT   = P101_ENV_CALL_LOG_ENTER | P101_ENV_CALL_LOG_EXIT
    } p101_env_call_log_options;

    typedef void (*p101_env_call_observer)(const struct p101_env *env, p101_env_call_event event, const char *call_name, const char *arguments, const char *result, const char *file_name, const char *function_name, int line_number, void *user_data);

    void p101_env_set_call_observer(struct p101_env *env, p101_env_call_observer observer, void *user_data);
    void p101_env_set_call_log(struct p101_env *env, FILE *stream, unsigned options);
    void p101_env_trace_call(const struct p101_env *env, const char *call_name, const char *arguments, const char *file_name, const char *function_name, int line_number);
    void p101_env_trace_call_exit(const struct p101_env *env, const char *call_name, const char *result, const char *file_name, const char *function_name, int line_number);

    /*
     * Event observers cannot safely raise into an application error object.
     * Instead, write failure is sticky and queryable. A clean analysis is only
     * trustworthy when p101_env_event_log_failed() remains false.
     */
    int  p101_env_event_log_failed(const struct p101_env *env);
    int  p101_env_event_log_errno(const struct p101_env *env);
    void p101_env_clear_event_log_error(struct p101_env *env);

    /* A short label for this env (typically a thread name). When set, the
     * default tracer prints it, so interleaved traces from one-env-per-thread
     * programs stay legible. The string is NOT copied; keep it alive. */
    void        p101_env_set_label(struct p101_env *env, const char *label);
    const char *p101_env_get_label(const struct p101_env *env);

    /*
     * Fault injection (TEST ONLY). Every p101 wrapper that can fail calls
     * p101_env_check_fault() before doing the real work; an installed injector
     * returns 0 to proceed or a non-zero errno to make the call fail as if the
     * OS had returned that error. This drives error paths that are otherwise
     * almost impossible to reach -- run a test in a loop failing call #1, then
     * #2, and walk every "did you check the error?" branch. Like the error
     * reporter, this changes what the CALLEE returns; it never intercepts the
     * caller's handling. There is no injector by default, so check_fault() is a
     * single predictable branch in production builds.
     *
     * The environment bridge also understands P101_FAULT_LOG=path. When a
     * configured fault actually fires it writes:
     *
     *   P101FAULT<TAB>3<TAB>pid<TAB>call-index<TAB>call-name<TAB>errno<TAB>mode<TAB>amount<TAB>phase<TAB>disposition
     *
     * This is intentionally separate from P101_RESOURCE_LOG; launchers use it
     * as a control signal to stop after the last fault-capable call.
     * P101_FAULT_MODE may be error, eintr, timeout, short, or uncertain;
     * P101_FAULT_AMOUNT bounds supported short-I/O calls and
     * P101_FAULT_REPEAT exercises repeated retry paths. Before-call failures
     * are recorded when selected. A wrapper must call
     * p101_env_record_fault_action() only after an after-dispatch or
     * partial-progress action has actually occurred.
     */
    typedef int (*p101_env_fault_injector)(const struct p101_env *env, const char *call_name, void *user_data);

    typedef enum
    {
        P101_ENV_FAULT_NONE = 0,
        P101_ENV_FAULT_ERROR,
        P101_ENV_FAULT_SHORT,
        P101_ENV_FAULT_UNCERTAIN
    } p101_env_fault_kind;

    typedef enum
    {
        P101_ENV_FAULT_BEFORE_CALL = 0,
        P101_ENV_FAULT_AFTER_DISPATCH,
        P101_ENV_FAULT_AFTER_PARTIAL_PROGRESS
    } p101_env_fault_phase;

    typedef enum
    {
        P101_ENV_FAULT_RETRY_SAFE = 0,
        P101_ENV_FAULT_PROGRESS_KNOWN,
        P101_ENV_FAULT_OUTCOME_UNCERTAIN
    } p101_env_fault_disposition;

    struct p101_env_fault_action
    {
        p101_env_fault_kind        kind;
        p101_env_fault_phase       phase;
        p101_env_fault_disposition disposition;
        int                        errnum;
        size_t                     amount;
        unsigned long              call_index;
    };

/* The fault-log record prefix and the emitted format version. */
#define P101_ENV_FAULT_LOG_TAG "P101FAULT"
#define P101_ENV_FAULT_LOG_VERSION "3"

    /*
     * The closed vocabulary of P101_FAULT_MODE values. The mode is the
     * user-facing name; the kind/phase/disposition triple below is what the
     * mode means, and it is the same triple the fault log reports.
     */
    typedef enum
    {
        P101_ENV_FAULT_MODE_ERROR = 0,
        P101_ENV_FAULT_MODE_EINTR,
        P101_ENV_FAULT_MODE_TIMEOUT,
        P101_ENV_FAULT_MODE_SHORT,
        P101_ENV_FAULT_MODE_UNCERTAIN
    } p101_env_fault_mode;

    /*
     * The semantics a mode implies. errnum is the errno the mode dictates, or
     * 0 when the mode keeps whatever P101_ENV_FAULT_ERRNO_ENV supplied. This
     * struct is the single source of truth for the mode invariant; an analyzer
     * that reads a fault record can check the record's phase and disposition
     * against it instead of restating the table.
     */
    struct p101_env_fault_defaults
    {
        p101_env_fault_kind        kind;
        p101_env_fault_phase       phase;
        p101_env_fault_disposition disposition;
        int                        errnum;
    };

    /* The wire names. Every one of these returns a static string, never NULL;
     * an unrecognized value maps to the name of the zero enumerator. */
    const char *p101_env_fault_mode_name(p101_env_fault_mode mode);
    const char *p101_env_fault_phase_name(p101_env_fault_phase phase);
    const char *p101_env_fault_disposition_name(p101_env_fault_disposition disposition);

    /* Parse a mode name. Returns false (leaving *mode untouched) for NULL or
     * for a name outside the vocabulary. */
    bool p101_env_fault_mode_from_name(const char *name, p101_env_fault_mode *mode);

    /* Look up the semantics of a mode. Returns false for a mode outside the
     * vocabulary or a NULL destination. */
    bool p101_env_fault_mode_defaults(p101_env_fault_mode mode, struct p101_env_fault_defaults *out);

    void p101_env_set_fault_injector(struct p101_env *env, p101_env_fault_injector injector, void *user_data);
    int  p101_env_check_fault(const struct p101_env *env, const char *call_name) P101_ATTR_SEMANTIC_ROLE("p101:instrumentation:fault");
    int  p101_env_check_fault_action(const struct p101_env *env, const char *call_name, struct p101_env_fault_action *action) P101_ATTR_SEMANTIC_ROLE("p101:instrumentation:fault");
    void p101_env_record_fault_action(const struct p101_env *env, const char *call_name, const struct p101_env_fault_action *action);

    /*
     * File-descriptor ledger (opt-in). When enabled, wrappers that open or
     * close descriptors record them here with the file/function/line that
     * created them; p101_env_report_leaks() prints any still open. This is the
     * portable, attributed equivalent of what ASan does for memory -- aimed at
     * the descriptor leaks that plague long-running servers. Tracking is
     * PER-ENV and is NOT inherited by p101_env_dup(); re-enable it on the dup if
     * you need it.
     *
     * The wrappers that feed the ledger are every call that hands back a
     * descriptor -- open, openat, creat, fcntl(F_DUPFD), dup, dup2, pipe,
     * socket, socketpair, accept, mkstemp, shm_open, posix_openpt -- plus
     * close and dup2, which retire one. pipe and socketpair record BOTH
     * descriptors; dup2 retires the descriptor it silently closes before
     * recording the replacement.
     */
    void   p101_env_enable_fd_tracking(struct p101_env *env, struct p101_error *err);
    void   p101_env_track_open(const struct p101_env *env, int fd, const char *file_name, const char *function_name, int line_number) P101_ATTR_SEMANTIC_ROLE("p101:instrumentation:fd");
    void   p101_env_track_close(const struct p101_env *env, int fd, const char *file_name, const char *function_name, int line_number) P101_ATTR_SEMANTIC_ROLE("p101:instrumentation:fd");
    size_t p101_env_report_leaks(const struct p101_env *env);

    /*
     * The fd EVENT OBSERVER is the layer underneath the ledger. It fires for
     * every descriptor the wrappers create or retire, whether or not the
     * in-process ledger is enabled -- so a program can emit a log and pay
     * nothing for the linked list, keep the ledger and emit no log, or do both.
     * The observer sees the CLOSE site as well as the OPEN site, which is what
     * lets an offline analyzer name both halves of a double close.
     *
     * Unlike the ledger, the observer IS inherited by p101_env_dup(), because a
     * log is a destination rather than per-env state, and a per-thread env
     * should keep writing to the same place.
     *
     * Bookkeeping never fails the caller's real operation: an observer must not
     * raise errors, and must not call back into the wrappers it is observing.
     */
    typedef enum
    {
        P101_ENV_FD_OPEN  = 0,
        P101_ENV_FD_CLOSE = 1
    } p101_env_fd_event;

    typedef void (*p101_env_fd_observer)(const struct p101_env *env, p101_env_fd_event event, int fd, const char *file_name, const char *function_name, int line_number, void *user_data);

    void p101_env_set_fd_observer(struct p101_env *env, p101_env_fd_observer observer, void *user_data);

    /*
     * Emit the final completeness record for every configured event stream.
     * Normal owners get this automatically from p101_env_destroy(); wrappers
     * that terminate without ordinary cleanup call it immediately before
     * handing control to a process-termination function. Repeated calls in
     * one process are harmless.
     */
    void p101_env_complete_event_streams(const struct p101_env *env);
    void p101_env_after_fork_child(const struct p101_env *env);

    /*
     * The convenience sink built on that observer: one line per event, in the
     * format the resource-tracker analyzer reads.
     *
     *     P101FD<TAB>4<TAB>pid<TAB>context<TAB>seq<TAB>mono_ns<TAB>wall_ns<TAB>OPEN|CLOSE<TAB>fd<TAB>line<TAB>function<TAB>file
     *     P101FORK<TAB>4<TAB>parent-pid<TAB>context<TAB>seq<TAB>mono_ns<TAB>wall_ns<TAB>child-pid<TAB>line<TAB>function<TAB>file
     *     P101SPAWN<TAB>4<TAB>parent-pid<TAB>context<TAB>seq<TAB>mono_ns<TAB>wall_ns<TAB>child-pid<TAB>line<TAB>function<TAB>file<TAB>target
     *     P101EXEC<TAB>4<TAB>pid<TAB>context<TAB>seq<TAB>mono_ns<TAB>wall_ns<TAB>fd<TAB>cloexec<TAB>line<TAB>function<TAB>file<TAB>target
     *     P101EXECFAIL<TAB>4<TAB>pid<TAB>context<TAB>seq<TAB>mono_ns<TAB>wall_ns<TAB>line<TAB>function<TAB>file<TAB>target
     *
     * The P101 record prefixes let the log share a stream with ordinary output
     * and still be grepped back out; 4 is the emitted format version.
     * Free-form fields are escaped so embedded tabs and newlines cannot change
     * the record shape.
     * Each bounded record is published under the environment's emission lock.
     * The stream is not synchronously flushed after every record. Because the pid is
     * on every line, the analyzer can tell the child's descriptors from the
     * parent's. The fork record lets an analyzer seed the child's descriptor
     * table from the descriptors live in the parent at fork time. A spawn
     * record preserves the parent, child, target, and source boundary without
     * claiming to reconstruct opaque spawn file actions. The exec record scans
     * the process descriptor range at an exec boundary and records every open
     * descriptor it finds with its FD_CLOEXEC state, so an offline analyzer can
     * explain which tracked descriptors would cross into the new program
     * image. Passing NULL removes the sink. The stream is NOT closed by
     * p101_env_destroy(); the caller owns it. Keep a configured stream open
     * until destruction writes its completion receipt, or clear the sink
     * before closing the stream yourself.
     */
    void p101_env_set_fd_log(struct p101_env *env, FILE *stream);
    void p101_env_track_fork(const struct p101_env *env, long parent_pid, long child_pid, const char *file_name, const char *function_name, int line_number) P101_ATTR_SEMANTIC_ROLE("p101:instrumentation:fd");
    void p101_env_track_spawn(const struct p101_env *env, long parent_pid, long child_pid, const char *target, const char *file_name, const char *function_name, int line_number) P101_ATTR_SEMANTIC_ROLE("p101:instrumentation:fd");
    void p101_env_track_exec(const struct p101_env *env, const char *target, const char *file_name, const char *function_name, int line_number) P101_ATTR_SEMANTIC_ROLE("p101:instrumentation:fd");
    void p101_env_track_exec_failure(const struct p101_env *env, const char *target, const char *file_name, const char *function_name, int line_number) P101_ATTR_SEMANTIC_ROLE("p101:instrumentation:fd");

    /*
     * Allocation event observer. Like the fd observer, this is best-effort
     * bookkeeping and must never call p101 wrappers. It powers heap-resource
     * analysis without requiring platform-specific tools such as Valgrind.
     */
    typedef enum
    {
        P101_ENV_ALLOC_ALLOC = 0,
        P101_ENV_ALLOC_FREE,
        P101_ENV_ALLOC_REALLOC
    } p101_env_alloc_event;

    typedef void (*p101_env_alloc_observer)(const struct p101_env *env, p101_env_alloc_event event, const void *ptr, const void *new_ptr, size_t size, const char *file_name, const char *function_name, int line_number, void *user_data);

    void p101_env_set_alloc_observer(struct p101_env *env, p101_env_alloc_observer observer, void *user_data);
    void p101_env_set_alloc_log(struct p101_env *env, FILE *stream);
    void p101_env_track_alloc(const struct p101_env *env, const void *ptr, size_t size, const char *file_name, const char *function_name, int line_number) P101_ATTR_SEMANTIC_ROLE("p101:instrumentation:allocation");
    void p101_env_track_free(const struct p101_env *env, const void *ptr, const char *file_name, const char *function_name, int line_number) P101_ATTR_SEMANTIC_ROLE("p101:instrumentation:allocation");
    void p101_env_track_realloc(const struct p101_env *env, const void *ptr, const void *new_ptr, size_t size, const char *file_name, const char *function_name, int line_number) P101_ATTR_SEMANTIC_ROLE("p101:instrumentation:allocation");

    /*
     * Generic non-FD/non-heap resource lifecycle events. Resource classes and
     * ids are stable text chosen by the wrapper (for example "mmap" plus the
     * mapped address). The observer is deliberately generic so new wrappers do
     * not require another event schema.
     */
    typedef enum
    {
        P101_ENV_RESOURCE_ACQUIRE = 0,
        P101_ENV_RESOURCE_RELEASE,
        P101_ENV_RESOURCE_REPLACE,
        P101_ENV_RESOURCE_TRANSFER,
        P101_ENV_RESOURCE_USE
    } p101_env_resource_kind;

    typedef void (*p101_env_resource_observer)(const struct p101_env *env, p101_env_resource_kind event, const char *resource_class, const char *resource_id, const char *related_id, size_t size, const char *metadata, const char *file_name,
                                               const char *function_name, int line_number, void *user_data);

    void p101_env_set_resource_observer(struct p101_env *env, p101_env_resource_observer observer, void *user_data);
    void p101_env_set_resource_log(struct p101_env *env, FILE *stream);
    void p101_env_track_resource(const struct p101_env *env, p101_env_resource_kind event, const char *resource_class, const char *resource_id, const char *related_id, size_t size, const char *metadata, const char *file_name, const char *function_name,
                                 int line_number) P101_ATTR_SEMANTIC_ROLE("p101:instrumentation:resource");
    void p101_env_pointer_resource_id(char *text, size_t text_size, const void *resource);
    void p101_env_track_pointer_resource(const struct p101_env *env, p101_env_resource_kind event, const char *resource_class, const void *resource, const void *related_resource, size_t size, const char *metadata, const char *file_name,
                                         const char *function_name, int line_number) P101_ATTR_SEMANTIC_ROLE("p101:instrumentation:resource");
    void p101_env_track_integer_resource(const struct p101_env *env, p101_env_resource_kind event, const char *resource_class, intmax_t resource, intmax_t related_resource, size_t size, const char *metadata, const char *file_name, const char *function_name,
                                         int line_number) P101_ATTR_SEMANTIC_ROLE("p101:instrumentation:resource");

#define P101_TRACE(env) p101_env_trace((env), __FILE__, __func__, __LINE__)
#define P101_TRACE_EXIT(env) p101_env_trace_exit((env), __FILE__, __func__, __LINE__)
#if defined(__GNUC__) || defined(__clang__)
    #define P101_TRACE_SCOPE(env)                                                                                                                                                                                                                                  \
        p101_env_trace_scope p101_trace_scope_guard __attribute__((cleanup(p101_env_trace_scope_cleanup))) = {(env), __FILE__, __func__, __LINE__};                                                                                                                \
        p101_env_trace((env), __FILE__, __func__, __LINE__)
#else
    #error "P101_TRACE_SCOPE requires a compiler with cleanup attribute support"
#endif
#define P101_CALL_ENTER(env, call_name, arguments) p101_env_trace_call((env), (call_name), (arguments), __FILE__, __func__, __LINE__)
#define P101_CALL_EXIT(env, call_name, result) p101_env_trace_call_exit((env), (call_name), (result), __FILE__, __func__, __LINE__)
#define P101_TRACK_OPEN(env, fd) p101_env_track_open((env), (fd), __FILE__, __func__, __LINE__)
#define P101_TRACK_CLOSE(env, fd) p101_env_track_close((env), (fd), __FILE__, __func__, __LINE__)
#define P101_TRACK_FORK(env, parent_pid, child_pid) p101_env_track_fork((env), (parent_pid), (child_pid), __FILE__, __func__, __LINE__)
#define P101_TRACK_SPAWN(env, parent_pid, child_pid, target) p101_env_track_spawn((env), (parent_pid), (child_pid), (target), __FILE__, __func__, __LINE__)
#define P101_TRACK_EXEC(env, target) p101_env_track_exec((env), (target), __FILE__, __func__, __LINE__)
#define P101_TRACK_EXEC_FAILURE(env, target) p101_env_track_exec_failure((env), (target), __FILE__, __func__, __LINE__)
#define P101_TRACK_ALLOC(env, ptr, size) p101_env_track_alloc((env), (ptr), (size), __FILE__, __func__, __LINE__)
#define P101_TRACK_FREE(env, ptr) p101_env_track_free((env), (ptr), __FILE__, __func__, __LINE__)
#define P101_TRACK_REALLOC(env, ptr, new_ptr, size) p101_env_track_realloc((env), (ptr), (new_ptr), (size), __FILE__, __func__, __LINE__)
#define P101_TRACK_RESOURCE_ACQUIRE(env, resource_class, resource_id, size, metadata) p101_env_track_resource((env), P101_ENV_RESOURCE_ACQUIRE, (resource_class), (resource_id), NULL, (size), (metadata), __FILE__, __func__, __LINE__)
#define P101_TRACK_RESOURCE_RELEASE(env, resource_class, resource_id, metadata) p101_env_track_resource((env), P101_ENV_RESOURCE_RELEASE, (resource_class), (resource_id), NULL, 0U, (metadata), __FILE__, __func__, __LINE__)
#define P101_TRACK_RESOURCE_REPLACE(env, resource_class, resource_id, related_id, size, metadata) p101_env_track_resource((env), P101_ENV_RESOURCE_REPLACE, (resource_class), (resource_id), (related_id), (size), (metadata), __FILE__, __func__, __LINE__)
#define P101_TRACK_RESOURCE_TRANSFER(env, resource_class, resource_id, related_id, metadata) p101_env_track_resource((env), P101_ENV_RESOURCE_TRANSFER, (resource_class), (resource_id), (related_id), 0U, (metadata), __FILE__, __func__, __LINE__)
#define P101_TRACK_RESOURCE_USE(env, resource_class, resource_id, metadata) p101_env_track_resource((env), P101_ENV_RESOURCE_USE, (resource_class), (resource_id), NULL, 0U, (metadata), __FILE__, __func__, __LINE__)
#define P101_ENV_POINTER_RESOURCE_ID_SIZE (2U + (sizeof(uintptr_t) * 2U) + 1U)
#define P101_TRACK_POINTER_RESOURCE_ACQUIRE(env, resource_class, resource, size, metadata) p101_env_track_pointer_resource((env), P101_ENV_RESOURCE_ACQUIRE, (resource_class), (resource), NULL, (size), (metadata), __FILE__, __func__, __LINE__)
#define P101_TRACK_POINTER_RESOURCE_RELEASE(env, resource_class, resource, metadata) p101_env_track_pointer_resource((env), P101_ENV_RESOURCE_RELEASE, (resource_class), (resource), NULL, 0U, (metadata), __FILE__, __func__, __LINE__)
#define P101_TRACK_POINTER_RESOURCE_REPLACE(env, resource_class, resource, related_resource, size, metadata)                                                                                                                                                       \
    p101_env_track_pointer_resource((env), P101_ENV_RESOURCE_REPLACE, (resource_class), (resource), (related_resource), (size), (metadata), __FILE__, __func__, __LINE__)
#define P101_TRACK_POINTER_RESOURCE_TRANSFER(env, resource_class, resource, related_resource, metadata)                                                                                                                                                            \
    p101_env_track_pointer_resource((env), P101_ENV_RESOURCE_TRANSFER, (resource_class), (resource), (related_resource), 0U, (metadata), __FILE__, __func__, __LINE__)
#define P101_TRACK_POINTER_RESOURCE_USE(env, resource_class, resource, metadata) p101_env_track_pointer_resource((env), P101_ENV_RESOURCE_USE, (resource_class), (resource), NULL, 0U, (metadata), __FILE__, __func__, __LINE__)
#define P101_TRACK_INTEGER_RESOURCE_ACQUIRE(env, resource_class, resource, size, metadata) p101_env_track_integer_resource((env), P101_ENV_RESOURCE_ACQUIRE, (resource_class), (intmax_t)(resource), 0, (size), (metadata), __FILE__, __func__, __LINE__)
#define P101_TRACK_INTEGER_RESOURCE_RELEASE(env, resource_class, resource, metadata) p101_env_track_integer_resource((env), P101_ENV_RESOURCE_RELEASE, (resource_class), (intmax_t)(resource), 0, 0U, (metadata), __FILE__, __func__, __LINE__)
#define P101_TRACK_INTEGER_RESOURCE_REPLACE(env, resource_class, resource, related_resource, size, metadata)                                                                                                                                                       \
    p101_env_track_integer_resource((env), P101_ENV_RESOURCE_REPLACE, (resource_class), (intmax_t)(resource), (intmax_t)(related_resource), (size), (metadata), __FILE__, __func__, __LINE__)
#define P101_TRACK_INTEGER_RESOURCE_TRANSFER(env, resource_class, resource, related_resource, metadata)                                                                                                                                                            \
    p101_env_track_integer_resource((env), P101_ENV_RESOURCE_TRANSFER, (resource_class), (intmax_t)(resource), (intmax_t)(related_resource), 0U, (metadata), __FILE__, __func__, __LINE__)
#define P101_TRACK_INTEGER_RESOURCE_USE(env, resource_class, resource, metadata) p101_env_track_integer_resource((env), P101_ENV_RESOURCE_USE, (resource_class), (intmax_t)(resource), 0, 0U, (metadata), __FILE__, __func__, __LINE__)

#ifdef __cplusplus
}
#endif

#endif    // LIBP101_ENV_ENV_H
