#include "p101_env/env.h"
#include "p101_env/wrapper.h"
#include <p101_tool_event/event.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures;

enum
{
    CONCURRENT_THREAD_COUNT      = 4,
    CONCURRENT_EVENTS_PER_THREAD = 250
};

struct concurrent_writer
{
    struct p101_env *env;
    size_t           thread_number;
};

struct concurrent_fd_writer
{
    struct p101_env *env;
    size_t           thread_number;
};

#define EXPECT(condition)                                                                                                                                                                                                                                          \
    do                                                                                                                                                                                                                                                             \
    {                                                                                                                                                                                                                                                              \
        if(!(condition))                                                                                                                                                                                                                                           \
        {                                                                                                                                                                                                                                                          \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                                                                                                                                                                                   \
            failures++;                                                                                                                                                                                                                                            \
        }                                                                                                                                                                                                                                                          \
    } while(0)

static void ignore_fd_event(const struct p101_env *env, p101_env_fd_event event, int fd, const char *file_name, const char *function_name, int line_number, void *user_data)
{
    (void)env;
    (void)event;
    (void)fd;
    (void)file_name;
    (void)function_name;
    (void)line_number;
    (void)user_data;
}

static void ignore_call_event(const struct p101_env *env, p101_env_call_event event, const char *call_name, const char *arguments, const char *result, const char *file_name, const char *function_name, int line_number, void *user_data)
{
    (void)env;
    (void)event;
    (void)call_name;
    (void)arguments;
    (void)result;
    (void)file_name;
    (void)function_name;
    (void)line_number;
    (void)user_data;
}

static void *write_concurrent_fd_events(void *arg)
{
    struct concurrent_fd_writer *writer;

    writer = (struct concurrent_fd_writer *)arg;
    for(size_t index = 0U; index < CONCURRENT_EVENTS_PER_THREAD; index++)
    {
        int fd;

        fd = (int)(writer->thread_number * CONCURRENT_EVENTS_PER_THREAD + index + 100U);
        p101_env_track_open(writer->env, fd, "concurrent.c", "worker", (int)index);
        p101_env_track_close(writer->env, fd, "concurrent.c", "worker", (int)index);
    }
    return NULL;
}

static void test_concurrent_fd_ledger(void)
{
    struct p101_error          *err;
    struct p101_env            *env;
    struct concurrent_fd_writer writers[CONCURRENT_THREAD_COUNT];
    pthread_t                   threads[CONCURRENT_THREAD_COUNT];

    err = p101_error_create(false);
    env = p101_env_create(err, NULL);
    EXPECT(err != NULL);
    EXPECT(env != NULL);
    if(env == NULL)
    {
        p101_error_destroy(err);
        return;
    }
    p101_env_enable_fd_tracking(env, err);
    EXPECT(!p101_error_has_error(err));
    for(size_t index = 0U; index < CONCURRENT_THREAD_COUNT; index++)
    {
        writers[index].env           = env;
        writers[index].thread_number = index;
        EXPECT(pthread_create(&threads[index], NULL, write_concurrent_fd_events, &writers[index]) == 0);
    }
    for(size_t index = 0U; index < CONCURRENT_THREAD_COUNT; index++)
    {
        EXPECT(pthread_join(threads[index], NULL) == 0);
    }
    EXPECT(p101_env_report_leaks(env) == 0U);
    p101_env_destroy(env);
    p101_error_destroy(err);
}

static void make_path(char path[], size_t path_size, const char *suffix)
{
    snprintf(path, path_size, "/tmp/p101-env-test-%ld-%s.log", (long)getpid(), suffix);
    remove(path);
}

P101_ATTR_SEMANTIC_ROLE("p101:boundary-case:boundary:wrapper-observation:stale_version")

static void test_event_parser_contract(void)
{
    struct p101_tool_event_record record;
    char                          escaped[]   = "P101CALL\t5\ttest-run\t42\t1\t1\t100\t200\tENTER\t7\tfun\\tname\tcall\\\\name\targ\\ntext\t-\tfile\\tname.c\n";
    char                          exec_fail[] = "P101EXECFAIL\t5\ttest-run\t42\t1\t2\t101\t201\t9\tp101_execv\tfile.c\t/bin/missing\n";
    char                          spawn[]     = "P101SPAWN\t5\ttest-run\t42\t1\t3\t102\t202\t43\t10\tp101_posix_spawn\tspawn.c\t/bin/true\n";
    char                          v1[]        = "P101FD\t1\t42\tOPEN\t3\t7\tmain\tfile.c\n";

    EXPECT(p101_tool_event_parse_line(escaped, &record) == P101_TOOL_EVENT_PARSE_OK);
    EXPECT(strcmp(record.run_id, "test-run") == 0);
    EXPECT(strcmp(record.function_name, "fun\tname") == 0);
    EXPECT(strcmp(record.call_name, "call\\name") == 0);
    EXPECT(strcmp(record.arguments, "arg\ntext") == 0);
    EXPECT(strcmp(record.file_name, "file\tname.c") == 0);
    EXPECT(p101_tool_event_parse_line(exec_fail, &record) == P101_TOOL_EVENT_PARSE_OK);
    EXPECT(record.record_kind == P101_TOOL_EVENT_RECORD_EXEC_FAIL);
    EXPECT(strcmp(record.target, "/bin/missing") == 0);
    EXPECT(p101_tool_event_parse_line(spawn, &record) == P101_TOOL_EVENT_PARSE_OK);
    EXPECT(record.record_kind == P101_TOOL_EVENT_RECORD_SPAWN);
    EXPECT(record.child_pid == 43);
    EXPECT(strcmp(record.target, "/bin/true") == 0);
    EXPECT(p101_tool_event_parse_line(v1, &record) == P101_TOOL_EVENT_PARSE_BAD_VERSION);
}

static void test_reader_terminates_on_error(void)
{
    struct p101_error *err;
    char               line[8] = "XXXXXXX";

    err = p101_error_create(false);
    EXPECT(err != NULL);
    EXPECT(p101_tool_event_read_line(err, NULL, line, sizeof(line)) == P101_TOOL_EVENT_LINE_ERROR);
    EXPECT(line[0] == '\0');
    EXPECT(p101_error_has_error(err));
    p101_error_destroy(err);
}

P101_ATTR_SEMANTIC_ROLE("p101:boundary-case:boundary:wrapper-observation:resource_limit")

static void test_long_record_round_trip(void)
{
    struct p101_error            *err;
    struct p101_env              *env;
    struct p101_tool_event_record record;
    FILE                         *stream;
    char                         *line;
    char                          long_name[1500];

    err    = p101_error_create(false);
    env    = p101_env_create(err, NULL);
    stream = tmpfile();
    line   = (char *)malloc(4096U);
    EXPECT(err != NULL);
    EXPECT(env != NULL);
    EXPECT(stream != NULL);
    EXPECT(line != NULL);

    memset(long_name, 'x', sizeof(long_name) - 1U);
    long_name[sizeof(long_name) - 1U] = '\0';
    p101_env_set_fd_log(env, stream);
    p101_env_track_open(env, 9, "file.c", long_name, 8);
    p101_env_destroy(env);
    rewind(stream);

    EXPECT(p101_tool_event_read_line(err, stream, line, 4096U) == P101_TOOL_EVENT_LINE_OK);
    EXPECT(p101_tool_event_parse_line(line, &record) == P101_TOOL_EVENT_PARSE_OK);
    EXPECT(strcmp(record.function_name, long_name) == 0);

    free(line);
    fclose(stream);
    p101_error_destroy(err);
}

P101_ATTR_SEMANTIC_ROLE("p101:boundary-case:boundary:wrapper-observation:clean")

static void test_generic_resource_round_trip(void)
{
    struct p101_error            *err;
    struct p101_env              *env;
    struct p101_tool_event_record record;
    FILE                         *stream;
    char                          line[2048];

    err    = p101_error_create(false);
    env    = p101_env_create(err, NULL);
    stream = tmpfile();
    EXPECT(err != NULL);
    EXPECT(env != NULL);
    EXPECT(stream != NULL);

    p101_env_set_resource_log(env, stream);
    p101_env_track_resource(env, P101_ENV_RESOURCE_ACQUIRE, "mapping", "0x1000", NULL, 4096U, "private", "mapping.c", "map_file", 27);
    p101_env_destroy(env);
    rewind(stream);

    EXPECT(p101_tool_event_read_line(err, stream, line, sizeof(line)) == P101_TOOL_EVENT_LINE_OK);
    EXPECT(p101_tool_event_parse_line(line, &record) == P101_TOOL_EVENT_PARSE_OK);
    EXPECT(record.record_kind == P101_TOOL_EVENT_RECORD_RESOURCE);
    EXPECT(record.resource_kind == P101_TOOL_EVENT_RESOURCE_ACQUIRE);
    EXPECT(strcmp(record.resource_class, "mapping") == 0);
    EXPECT(strcmp(record.resource_id, "0x1000") == 0);
    EXPECT(record.related_id == NULL);
    EXPECT(record.size == 4096U);
    EXPECT(strcmp(record.metadata, "private") == 0);
    EXPECT(strcmp(record.function_name, "map_file") == 0);
    EXPECT(strcmp(record.file_name, "mapping.c") == 0);
    EXPECT(record.line_number == 27);

    fclose(stream);
    p101_error_destroy(err);
}

P101_ATTR_SEMANTIC_ROLE("p101:boundary-case:boundary:wrapper-observation:typed_refusal")

static void test_event_write_failure_is_sticky(void)
{
    struct p101_error *err;
    struct p101_env   *env;
    FILE              *stream;

    err    = p101_error_create(false);
    env    = p101_env_create(err, NULL);
    stream = fopen(__FILE__, "r");
    EXPECT(err != NULL);
    EXPECT(env != NULL);
    EXPECT(stream != NULL);

    if(env != NULL && stream != NULL)
    {
        p101_env_set_resource_log(env, stream);
        p101_env_track_resource(env, P101_ENV_RESOURCE_ACQUIRE, "test", "write-failure", NULL, 0U, NULL, __FILE__, __func__, __LINE__);
        EXPECT(p101_env_event_log_failed(env) != 0);
        EXPECT(p101_env_event_log_errno(env) != 0);
        p101_env_clear_event_log_error(env);
        EXPECT(p101_env_event_log_failed(env) == 0);
    }

    p101_env_destroy(env);
    if(stream != NULL)
    {
        fclose(stream);
    }
    p101_error_destroy(err);
}

static void traced_scope(const struct p101_env *env)
{
    P101_TRACE_SCOPE(env);
}

static void test_scope_trace_pairs_entry_and_exit(void)
{
    struct p101_tool_event_record record;
    struct p101_error            *err;
    struct p101_env              *env;
    FILE                         *stream;
    char                          line[2048];

    err    = p101_error_create(false);
    env    = p101_env_create(err, NULL);
    stream = tmpfile();
    EXPECT(err != NULL);
    EXPECT(env != NULL);
    EXPECT(stream != NULL);

    p101_env_set_call_log(env, stream, P101_ENV_CALL_LOG_DEFAULT);
    traced_scope(env);
    p101_env_destroy(env);
    rewind(stream);

    EXPECT(p101_tool_event_read_line(err, stream, line, sizeof(line)) == P101_TOOL_EVENT_LINE_OK);
    EXPECT(p101_tool_event_parse_line(line, &record) == P101_TOOL_EVENT_PARSE_OK);
    EXPECT(record.record_kind == P101_TOOL_EVENT_RECORD_CALL);
    EXPECT(record.call_kind == P101_TOOL_EVENT_CALL_ENTER);
    EXPECT(strcmp(record.call_name, "traced_scope") == 0);
    EXPECT(p101_tool_event_read_line(err, stream, line, sizeof(line)) == P101_TOOL_EVENT_LINE_OK);
    EXPECT(p101_tool_event_parse_line(line, &record) == P101_TOOL_EVENT_PARSE_OK);
    EXPECT(record.record_kind == P101_TOOL_EVENT_RECORD_CALL);
    EXPECT(record.call_kind == P101_TOOL_EVENT_CALL_EXIT);
    EXPECT(strcmp(record.call_name, "traced_scope") == 0);

    fclose(stream);
    p101_error_destroy(err);
}

static void *write_concurrent_events(void *data)
{
    struct concurrent_writer *writer;
    char                      resource_id[64];
    size_t                    event_number;

    writer = (struct concurrent_writer *)data;
    for(event_number = 0U; event_number < CONCURRENT_EVENTS_PER_THREAD; event_number++)
    {
        snprintf(resource_id, sizeof(resource_id), "%zu:%zu", writer->thread_number, event_number);
        p101_env_track_resource(writer->env, P101_ENV_RESOURCE_ACQUIRE, "concurrency-test", resource_id, NULL, 0U, NULL, __FILE__, __func__, __LINE__);
    }

    return NULL;
}

static void test_concurrent_event_sequences(void)
{
    struct concurrent_writer      writers[CONCURRENT_THREAD_COUNT];
    pthread_t                     threads[CONCURRENT_THREAD_COUNT];
    struct p101_tool_event_record record;
    struct p101_error            *err;
    struct p101_env              *env;
    FILE                         *stream;
    unsigned char                *seen;
    char                          line[2048];
    size_t                        count;
    size_t                        index;
    int                           created[CONCURRENT_THREAD_COUNT] = {0};

    err    = p101_error_create(false);
    env    = p101_env_create(err, NULL);
    stream = tmpfile();
    seen   = (unsigned char *)calloc((size_t)CONCURRENT_THREAD_COUNT * CONCURRENT_EVENTS_PER_THREAD, sizeof(*seen));
    EXPECT(err != NULL);
    EXPECT(env != NULL);
    EXPECT(stream != NULL);
    EXPECT(seen != NULL);

    if(err == NULL || env == NULL || stream == NULL || seen == NULL)
    {
        free(seen);
        if(stream != NULL)
        {
            fclose(stream);
        }
        p101_env_destroy(env);
        p101_error_destroy(err);
        return;
    }

    p101_env_set_resource_log(env, stream);
    for(index = 0U; index < CONCURRENT_THREAD_COUNT; index++)
    {
        writers[index].env           = env;
        writers[index].thread_number = index;
        created[index]               = pthread_create(&threads[index], NULL, write_concurrent_events, &writers[index]) == 0 ? 1 : 0;
        EXPECT(created[index] != 0);
    }

    for(index = 0U; index < CONCURRENT_THREAD_COUNT; index++)
    {
        if(created[index] != 0)
        {
            EXPECT(pthread_join(threads[index], NULL) == 0);
        }
    }

    EXPECT(p101_env_event_log_failed(env) == 0);
    p101_env_destroy(env);
    env = NULL;
    rewind(stream);
    count = 0U;
    while(p101_tool_event_read_line(err, stream, line, sizeof(line)) == P101_TOOL_EVENT_LINE_OK)
    {
        EXPECT(p101_tool_event_parse_line(line, &record) == P101_TOOL_EVENT_PARSE_OK);
        if(record.record_kind == P101_TOOL_EVENT_RECORD_RESOURCE)
        {
            EXPECT(record.sequence > 0U);
            EXPECT(record.sequence <= (size_t)CONCURRENT_THREAD_COUNT * CONCURRENT_EVENTS_PER_THREAD);
            if(record.sequence > 0U && record.sequence <= (size_t)CONCURRENT_THREAD_COUNT * CONCURRENT_EVENTS_PER_THREAD)
            {
                EXPECT(seen[record.sequence - 1U] == 0U);
                seen[record.sequence - 1U] = 1U;
            }
            count++;
        }
        else
        {
            EXPECT(record.record_kind == P101_TOOL_EVENT_RECORD_COMPLETE);
            EXPECT(record.events_attempted == (size_t)CONCURRENT_THREAD_COUNT * CONCURRENT_EVENTS_PER_THREAD);
            EXPECT(record.write_failed == 0);
        }
    }
    EXPECT(count == (size_t)CONCURRENT_THREAD_COUNT * CONCURRENT_EVENTS_PER_THREAD);
    EXPECT(p101_error_has_no_error(err));

    free(seen);
    fclose(stream);
    p101_error_destroy(err);
}

static void test_short_fault_action_repeats(void)
{
    struct p101_env_fault_action action;
    struct p101_error           *err;
    struct p101_env             *env;

    EXPECT(setenv("P101_FAULT_CALL", "2", 1) == 0);
    EXPECT(setenv("P101_FAULT_MODE", "short", 1) == 0);
    EXPECT(setenv("P101_FAULT_AMOUNT", "3", 1) == 0);
    EXPECT(setenv("P101_FAULT_REPEAT", "2", 1) == 0);
    EXPECT(setenv("P101_FAULT_NAME", "p101_read", 1) == 0);
    err = p101_error_create(false);
    env = p101_env_create(err, NULL);
    EXPECT(unsetenv("P101_FAULT_CALL") == 0);
    EXPECT(unsetenv("P101_FAULT_MODE") == 0);
    EXPECT(unsetenv("P101_FAULT_AMOUNT") == 0);
    EXPECT(unsetenv("P101_FAULT_REPEAT") == 0);
    EXPECT(unsetenv("P101_FAULT_NAME") == 0);
    EXPECT(err != NULL);
    EXPECT(env != NULL);

    EXPECT(p101_env_check_fault_action(env, "p101_write", &action) == 0);
    EXPECT(p101_env_check_fault_action(env, "p101_read", &action) == 0);
    EXPECT(p101_env_check_fault_action(env, "p101_read", &action) != 0);
    EXPECT(action.kind == P101_ENV_FAULT_SHORT);
    EXPECT(action.phase == P101_ENV_FAULT_AFTER_PARTIAL_PROGRESS);
    EXPECT(action.disposition == P101_ENV_FAULT_PROGRESS_KNOWN);
    EXPECT(action.amount == 3U);
    EXPECT(p101_env_check_fault_action(env, "p101_read", &action) != 0);
    EXPECT(action.kind == P101_ENV_FAULT_SHORT);
    EXPECT(p101_env_check_fault_action(env, "p101_read", &action) == 0);

    p101_env_destroy(env);
    p101_error_destroy(err);
}

static void test_uncertain_fault_action_describes_hidden_outcome(void)
{
    struct p101_env_fault_action action;
    struct p101_error           *err;
    struct p101_env             *env;
    FILE                        *stream;
    char                         line[256];
    char                         path[256];

    make_path(path, sizeof(path), "uncertain-fault");
    EXPECT(setenv("P101_FAULT_CALL", "1", 1) == 0);
    EXPECT(setenv("P101_FAULT_MODE", "uncertain", 1) == 0);
    EXPECT(setenv("P101_FAULT_NAME", "p101_write", 1) == 0);
    EXPECT(setenv("P101_FAULT_LOG", path, 1) == 0);
    err = p101_error_create(false);
    env = p101_env_create(err, NULL);
    EXPECT(unsetenv("P101_FAULT_CALL") == 0);
    EXPECT(unsetenv("P101_FAULT_MODE") == 0);
    EXPECT(unsetenv("P101_FAULT_NAME") == 0);
    EXPECT(unsetenv("P101_FAULT_LOG") == 0);
    EXPECT(err != NULL);
    EXPECT(env != NULL);

    EXPECT(p101_env_check_fault_action(env, "p101_write", &action) != 0);
    EXPECT(action.kind == P101_ENV_FAULT_UNCERTAIN);
    EXPECT(action.phase == P101_ENV_FAULT_AFTER_DISPATCH);
    EXPECT(action.disposition == P101_ENV_FAULT_OUTCOME_UNCERTAIN);
    EXPECT(action.errnum == ETIMEDOUT);
    stream = fopen(path, "r");
    EXPECT(stream != NULL);
    if(stream != NULL)
    {
        EXPECT(fgetc(stream) == EOF);
        fclose(stream);
    }
    p101_env_record_fault_action(env, "p101_write", &action);

    p101_env_destroy(env);
    p101_error_destroy(err);

    stream = fopen(path, "r");
    EXPECT(stream != NULL);
    if(stream != NULL)
    {
        EXPECT(fgets(line, sizeof(line), stream) != NULL);
        EXPECT(strstr(line, "\tuncertain\t1\tafter-dispatch\toutcome-uncertain\n") != NULL);
        fclose(stream);
    }
    remove(path);
}

static void test_dup_keeps_owned_destination(void)
{
    struct p101_error *err;
    struct p101_env   *dup;
    struct p101_env   *env;
    FILE              *stream;
    char               line[2048];
    char               path[256];

    make_path(path, sizeof(path), "dup");
    EXPECT(setenv("P101_RESOURCE_LOG", path, 1) == 0);
    err = p101_error_create(false);
    env = p101_env_create(err, NULL);
    EXPECT(unsetenv("P101_RESOURCE_LOG") == 0);
    dup = p101_env_dup(err, env);
    EXPECT(err != NULL);
    EXPECT(env != NULL);
    EXPECT(dup != NULL);

    p101_env_track_open(dup, 11, "file.c", "worker", 9);
    p101_env_destroy(dup);
    p101_env_destroy(env);
    p101_error_destroy(err);

    stream = fopen(path, "r");
    EXPECT(stream != NULL);
    if(stream != NULL)
    {
        EXPECT(fgets(line, sizeof(line), stream) != NULL);
        EXPECT(strncmp(line, "P101FD\t5\t", strlen("P101FD\t5\t")) == 0);
        fclose(stream);
    }
    remove(path);
}

static void test_observers_remain_independent(void)
{
    struct p101_error *err;
    struct p101_env   *env;
    FILE              *stream;
    int                allocation;
    char               line[2048];
    char               path[256];

    make_path(path, sizeof(path), "observers");
    EXPECT(setenv("P101_RESOURCE_LOG", path, 1) == 0);
    err = p101_error_create(false);
    env = p101_env_create(err, NULL);
    EXPECT(unsetenv("P101_RESOURCE_LOG") == 0);
    EXPECT(err != NULL);
    EXPECT(env != NULL);

    p101_env_set_fd_observer(env, ignore_fd_event, NULL);
    p101_env_track_alloc(env, &allocation, sizeof(allocation), "file.c", "allocate", 10);
    p101_env_destroy(env);
    p101_error_destroy(err);

    stream = fopen(path, "r");
    EXPECT(stream != NULL);
    if(stream != NULL)
    {
        EXPECT(fgets(line, sizeof(line), stream) != NULL);
        EXPECT(strncmp(line, "P101ALLOC\t5\t", strlen("P101ALLOC\t5\t")) == 0);
        fclose(stream);
    }
    remove(path);
}

static void test_configured_logs_survive_application_observers(void)
{
    struct p101_error *err;
    struct p101_env   *env;
    FILE              *stream;
    char               line[2048];
    char               call_path[256];
    char               resource_path[256];
    int                call_record;
    int                resource_record;

    make_path(call_path, sizeof(call_path), "observer-call-fanout");
    make_path(resource_path, sizeof(resource_path), "observer-resource-fanout");
    EXPECT(setenv("P101_CALL_LOG", call_path, 1) == 0);
    EXPECT(setenv("P101_RESOURCE_LOG", resource_path, 1) == 0);
    err = p101_error_create(false);
    env = p101_env_create(err, NULL);
    EXPECT(unsetenv("P101_CALL_LOG") == 0);
    EXPECT(unsetenv("P101_RESOURCE_LOG") == 0);
    EXPECT(err != NULL);
    EXPECT(env != NULL);

    p101_env_set_call_observer(env, ignore_call_event, NULL);
    p101_env_set_fd_observer(env, ignore_fd_event, NULL);
    p101_env_trace_call(env, "application_call", NULL, __FILE__, __func__, __LINE__);
    p101_env_trace_call_exit(env, "application_call", NULL, __FILE__, __func__, __LINE__);
    p101_env_track_open(env, 23, __FILE__, __func__, __LINE__);
    p101_env_destroy(env);
    p101_error_destroy(err);

    call_record = 0;
    stream      = fopen(call_path, "r");
    EXPECT(stream != NULL);
    if(stream != NULL)
    {
        while(fgets(line, sizeof(line), stream) != NULL)
        {
            if(strstr(line, "\tapplication_call\t") != NULL)
            {
                call_record++;
            }
        }
        fclose(stream);
    }
    EXPECT(call_record == 2);

    resource_record = 0;
    stream          = fopen(resource_path, "r");
    EXPECT(stream != NULL);
    if(stream != NULL)
    {
        while(fgets(line, sizeof(line), stream) != NULL)
        {
            if(strncmp(line, "P101FD\t5\t", strlen("P101FD\t5\t")) == 0)
            {
                resource_record++;
            }
        }
        fclose(stream);
    }
    EXPECT(resource_record == 1);
    remove(call_path);
    remove(resource_path);
}

P101_ATTR_SEMANTIC_ROLE("p101:boundary-case:boundary:wrapper-observation:identity_mismatch")

static void test_event_run_identity(void)
{
    struct p101_error            *err;
    struct p101_env              *env;
    struct p101_tool_event_record record;
    FILE                         *stream;
    char                          line[2048];
    char                          too_long[P101_TOOL_EVENT_RUN_ID_MAX_BYTES + 2U];

    EXPECT(setenv(P101_ENV_EVENT_RUN_ID_ENV, "env-test-run", 1) == 0);
    err    = p101_error_create(false);
    env    = p101_env_create(err, NULL);
    stream = tmpfile();
    EXPECT(unsetenv(P101_ENV_EVENT_RUN_ID_ENV) == 0);
    EXPECT(err != NULL);
    EXPECT(env != NULL);
    EXPECT(stream != NULL);
    if(env != NULL && stream != NULL)
    {
        p101_env_set_fd_log(env, stream);
        p101_env_track_open(env, 19, "identity.c", "open_identity", 7);
        p101_env_destroy(env);
        env = NULL;
        rewind(stream);
        EXPECT(fgets(line, sizeof(line), stream) != NULL);
        EXPECT(p101_tool_event_parse_line(line, &record) == P101_TOOL_EVENT_PARSE_OK);
        EXPECT(strcmp(record.run_id, "env-test-run") == 0);
    }
    p101_env_destroy(env);
    if(stream != NULL)
    {
        fclose(stream);
    }

    memset(too_long, 'x', sizeof(too_long) - 1U);
    too_long[sizeof(too_long) - 1U] = '\0';
    p101_error_reset(err);
    EXPECT(setenv(P101_ENV_EVENT_RUN_ID_ENV, too_long, 1) == 0);
    env = p101_env_create(err, NULL);
    EXPECT(unsetenv(P101_ENV_EVENT_RUN_ID_ENV) == 0);
    EXPECT(env == NULL);
    EXPECT(p101_error_is_errno(err, EINVAL));
    p101_env_destroy(env);
    p101_error_destroy(err);
}

P101_ATTR_SEMANTIC_ROLE("p101:boundary-case:boundary:wrapper-observation:binding_swap")

static void test_distinct_manual_streams_receive_completion(void)
{
    struct p101_error *err;
    struct p101_env   *env;
    FILE              *alloc_stream;
    FILE              *fd_stream;
    int                allocation;
    char               line[2048];
    int                alloc_complete;
    int                fd_complete;

    err          = p101_error_create(false);
    env          = p101_env_create(err, NULL);
    alloc_stream = tmpfile();
    fd_stream    = tmpfile();
    EXPECT(err != NULL);
    EXPECT(env != NULL);
    EXPECT(alloc_stream != NULL);
    EXPECT(fd_stream != NULL);
    if(env == NULL || alloc_stream == NULL || fd_stream == NULL)
    {
        if(env != NULL)
        {
            p101_env_destroy(env);
        }
        if(alloc_stream != NULL)
        {
            fclose(alloc_stream);
        }
        if(fd_stream != NULL)
        {
            fclose(fd_stream);
        }
        p101_error_destroy(err);
        return;
    }

    p101_env_set_alloc_log(env, alloc_stream);
    p101_env_set_fd_log(env, fd_stream);
    p101_env_track_alloc(env, &allocation, sizeof(allocation), "file.c", "allocate", 10);
    p101_env_track_open(env, 41, "file.c", "open_file", 11);
    p101_env_destroy(env);

    alloc_complete = 0;
    rewind(alloc_stream);
    while(fgets(line, sizeof(line), alloc_stream) != NULL)
    {
        if(strncmp(line, "P101COMPLETE\t5\t", strlen("P101COMPLETE\t5\t")) == 0)
        {
            alloc_complete++;
        }
    }
    fd_complete = 0;
    rewind(fd_stream);
    while(fgets(line, sizeof(line), fd_stream) != NULL)
    {
        if(strncmp(line, "P101COMPLETE\t5\t", strlen("P101COMPLETE\t5\t")) == 0)
        {
            fd_complete++;
        }
    }
    EXPECT(alloc_complete == 1);
    EXPECT(fd_complete == 1);

    fclose(alloc_stream);
    fclose(fd_stream);
    p101_error_destroy(err);
}

static void test_dup_keeps_fault_configuration(void)
{
    struct p101_error *err;
    struct p101_env   *dup;
    struct p101_env   *env;
    FILE              *stream;
    char               line[256];
    char               path[256];

    make_path(path, sizeof(path), "fault");
    EXPECT(setenv("P101_FAULT_CALL", "1", 1) == 0);
    EXPECT(setenv("P101_FAULT_ERRNO", "71", 1) == 0);
    EXPECT(setenv("P101_FAULT_NAME", "p101_test_call", 1) == 0);
    EXPECT(setenv("P101_FAULT_LOG", path, 1) == 0);
    err = p101_error_create(false);
    env = p101_env_create(err, NULL);
    EXPECT(unsetenv("P101_FAULT_CALL") == 0);
    EXPECT(unsetenv("P101_FAULT_ERRNO") == 0);
    EXPECT(unsetenv("P101_FAULT_NAME") == 0);
    EXPECT(unsetenv("P101_FAULT_LOG") == 0);
    dup = p101_env_dup(err, env);
    EXPECT(err != NULL);
    EXPECT(env != NULL);
    EXPECT(dup != NULL);

    if(dup != NULL)
    {
        EXPECT(p101_env_check_fault(dup, "other_call") == 0);
        EXPECT(p101_env_check_fault(dup, "p101_test_call") == 71);
    }

    p101_env_destroy(dup);
    p101_env_destroy(env);
    p101_error_destroy(err);

    stream = fopen(path, "r");
    EXPECT(stream != NULL);
    if(stream != NULL)
    {
        EXPECT(fgets(line, sizeof(line), stream) != NULL);
        EXPECT(strncmp(line, "P101FAULT\t3\t", strlen("P101FAULT\t3\t")) == 0);
        EXPECT(strstr(line, "\tbefore-call\tretry-safe\n") != NULL);
        fclose(stream);
    }
    remove(path);
}

int main(void)
{
    unsetenv("P101_RESOURCE_LOG");
    unsetenv("P101_CALL_LOG");
    unsetenv("P101_FAULT_CALL");

    test_event_parser_contract();
    test_reader_terminates_on_error();
    test_long_record_round_trip();
    test_generic_resource_round_trip();
    test_event_write_failure_is_sticky();
    test_scope_trace_pairs_entry_and_exit();
    test_concurrent_event_sequences();
    test_concurrent_fd_ledger();
    test_short_fault_action_repeats();
    test_uncertain_fault_action_describes_hidden_outcome();
    test_dup_keeps_owned_destination();
    test_observers_remain_independent();
    test_configured_logs_survive_application_observers();
    test_event_run_identity();
    test_distinct_manual_streams_receive_completion();
    test_dup_keeps_fault_configuration();

    if(failures != 0)
    {
        fprintf(stderr, "%d lib_env test(s) failed\n", failures);
        return 1;
    }

    return 0;
}
