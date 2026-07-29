#include "p101_env/env.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures;

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

static void make_path(char path[], size_t path_size, const char *suffix)
{
    snprintf(path, path_size, "/tmp/p101-env-test-%ld-%s.log", (long)getpid(), suffix);
    remove(path);
}

static void test_event_parser_contract(void)
{
    struct p101_env_event_record record;
    char                         escaped[] = "P101CALL\t2\t42\t1\t100\t200\tENTER\t7\tfun\\tname\tcall\\\\name\targ\\ntext\t-\tfile\\tname.c\n";
    char                         exec_fail[] = "P101EXECFAIL\t2\t42\t2\t101\t201\t9\tp101_execv\tfile.c\t/bin/missing\n";
    char                         spawn[] = "P101SPAWN\t2\t42\t3\t102\t202\t43\t10\tp101_posix_spawn\tspawn.c\t/bin/true\n";
    char                         v1[]      = "P101FD\t1\t42\tOPEN\t3\t7\tmain\tfile.c\n";

    EXPECT(p101_env_parse_event_line(escaped, &record) == P101_ENV_EVENT_PARSE_OK);
    EXPECT(strcmp(record.function_name, "fun\tname") == 0);
    EXPECT(strcmp(record.call_name, "call\\name") == 0);
    EXPECT(strcmp(record.arguments, "arg\ntext") == 0);
    EXPECT(strcmp(record.file_name, "file\tname.c") == 0);
    EXPECT(p101_env_parse_event_line(exec_fail, &record) == P101_ENV_EVENT_PARSE_OK);
    EXPECT(record.record_kind == P101_ENV_EVENT_RECORD_EXEC_FAIL);
    EXPECT(strcmp(record.target, "/bin/missing") == 0);
    EXPECT(p101_env_parse_event_line(spawn, &record) == P101_ENV_EVENT_PARSE_OK);
    EXPECT(record.record_kind == P101_ENV_EVENT_RECORD_SPAWN);
    EXPECT(record.child_pid == 43);
    EXPECT(strcmp(record.target, "/bin/true") == 0);
    EXPECT(p101_env_parse_event_line(v1, &record) == P101_ENV_EVENT_PARSE_BAD_VERSION);
}

static void test_reader_terminates_on_error(void)
{
    struct p101_error *err;
    char               line[8] = "XXXXXXX";

    err = p101_error_create(false);
    EXPECT(err != NULL);
    EXPECT(p101_env_read_event_line(err, NULL, line, sizeof(line)) == P101_ENV_EVENT_LINE_ERROR);
    EXPECT(line[0] == '\0');
    EXPECT(p101_error_has_error(err));
    p101_error_destroy(err);
}

static void test_long_record_round_trip(void)
{
    struct p101_error           *err;
    struct p101_env             *env;
    struct p101_env_event_record record;
    FILE                        *stream;
    char                        *line;
    char                         long_name[1500];

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
    rewind(stream);

    EXPECT(p101_env_read_event_line(err, stream, line, 4096U) == P101_ENV_EVENT_LINE_OK);
    EXPECT(p101_env_parse_event_line(line, &record) == P101_ENV_EVENT_PARSE_OK);
    EXPECT(strcmp(record.function_name, long_name) == 0);

    free(line);
    fclose(stream);
    p101_env_destroy(env);
    p101_error_destroy(err);
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
        EXPECT(strncmp(line, "P101FD\t2\t", strlen("P101FD\t2\t")) == 0);
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
        EXPECT(strncmp(line, "P101ALLOC\t2\t", strlen("P101ALLOC\t2\t")) == 0);
        fclose(stream);
    }
    remove(path);
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
        EXPECT(strncmp(line, "P101FAULT\t1\t", strlen("P101FAULT\t1\t")) == 0);
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
    test_dup_keeps_owned_destination();
    test_observers_remain_independent();
    test_dup_keeps_fault_configuration();

    if(failures != 0)
    {
        fprintf(stderr, "%d lib_env test(s) failed\n", failures);
        return 1;
    }

    return 0;
}
