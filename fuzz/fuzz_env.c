#include "p101_env/env.h"
#include <p101_tool_event/event.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static void require(int condition)
{
    if(!condition)
    {
        abort();
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    struct p101_tool_event_record record;
    p101_tool_event_parse_status  status;
    char                        *line;

    if(size > 65536U)
    {
        return 0;
    }

    line = (char *)malloc(size + 1U);
    if(line == NULL)
    {
        return 0;
    }

    memcpy(line, data, size);
    line[size] = '\0';
    status     = p101_tool_event_parse_line(line, &record);

    if(status == P101_TOOL_EVENT_PARSE_OK)
    {
        require(record.pid >= 0);
        require(record.monotonic_ns_available == 0 || record.monotonic_ns_available == 1);
        require(record.wall_unix_ns_available == 0 || record.wall_unix_ns_available == 1);

        switch(record.record_kind)
        {
            case P101_TOOL_EVENT_RECORD_FD:
            case P101_TOOL_EVENT_RECORD_FORK:
            case P101_TOOL_EVENT_RECORD_SPAWN:
            case P101_TOOL_EVENT_RECORD_EXEC:
            case P101_TOOL_EVENT_RECORD_EXEC_FAIL:
            case P101_TOOL_EVENT_RECORD_ALLOC:
            case P101_TOOL_EVENT_RECORD_CALL:
            {
                require(record.file_name != NULL);
                require(record.function_name != NULL);
                break;
            }
            default:
            {
                abort();
            }
        }
    }

    free(line);
    return 0;
}
