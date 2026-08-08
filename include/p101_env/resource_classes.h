#ifndef LIBP101_ENV_RESOURCE_CLASSES_H
#define LIBP101_ENV_RESOURCE_CLASSES_H

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
 * Resource-class names used by the p101 resource-tracking macros
 * (P101_TRACK_RESOURCE_*, P101_TRACK_POINTER_RESOURCE_*,
 * P101_TRACK_INTEGER_RESOURCE_*) and by the analyzers that read the records
 * they emit.
 *
 * The vocabulary is OPEN: a resource class is just a string, and one-off or
 * application-specific classes may still be written as free string literals.
 * These macros exist so that an acquire in one library and the matching
 * release in another spell the same class exactly once, letting the lifecycle
 * model pair them.
 */

#ifdef __cplusplus
extern "C"
{
#endif

#define P101_RESOURCE_CLASS_ADDRESS_INFO "address-info"
#define P101_RESOURCE_CLASS_ALLOCATION "allocation"
#define P101_RESOURCE_CLASS_COMPILED_REGEX "compiled-regex"
#define P101_RESOURCE_CLASS_DIRECTORY_STREAM "directory-stream"
#define P101_RESOURCE_CLASS_DYNAMIC_LIBRARY "dynamic-library"
#define P101_RESOURCE_CLASS_FD "fd"
#define P101_RESOURCE_CLASS_GLOB_RESULT "glob-result"
#define P101_RESOURCE_CLASS_ICONV_DESCRIPTOR "iconv-descriptor"
#define P101_RESOURCE_CLASS_INTERFACE_NAME_INDEX "interface-name-index"
#define P101_RESOURCE_CLASS_LOCALE "locale"
#define P101_RESOURCE_CLASS_MAPPING "mapping"
#define P101_RESOURCE_CLASS_NAMED_SEMAPHORE "named-semaphore"
#define P101_RESOURCE_CLASS_NDBM_DATABASE "ndbm-database"
#define P101_RESOURCE_CLASS_PROCESS_HASH_TABLE "process-hash-table"
#define P101_RESOURCE_CLASS_PTHREAD_ATTRIBUTES "pthread-attributes"
#define P101_RESOURCE_CLASS_PTHREAD_CONDITION "pthread-condition"
#define P101_RESOURCE_CLASS_PTHREAD_CONDITION_ATTRIBUTES "pthread-condition-attributes"
#define P101_RESOURCE_CLASS_PTHREAD_CONDITION_WAIT "pthread-condition-wait"
#define P101_RESOURCE_CLASS_PTHREAD_JOINABLE_THREAD "pthread-joinable-thread"
#define P101_RESOURCE_CLASS_PTHREAD_JOIN_WAIT "pthread-join-wait"
#define P101_RESOURCE_CLASS_PTHREAD_MUTEX "pthread-mutex"
#define P101_RESOURCE_CLASS_PTHREAD_MUTEX_ATTRIBUTES "pthread-mutex-attributes"
#define P101_RESOURCE_CLASS_PTHREAD_MUTEX_HELD "pthread-mutex-held"
#define P101_RESOURCE_CLASS_PTHREAD_MUTEX_WAIT "pthread-mutex-wait"
#define P101_RESOURCE_CLASS_PTHREAD_RWLOCK "pthread-rwlock"
#define P101_RESOURCE_CLASS_PTHREAD_RWLOCK_ATTRIBUTES "pthread-rwlock-attributes"
#define P101_RESOURCE_CLASS_PTHREAD_RWLOCK_HELD "pthread-rwlock-held"
#define P101_RESOURCE_CLASS_PTHREAD_RWLOCK_READ_WAIT "pthread-rwlock-read-wait"
#define P101_RESOURCE_CLASS_PTHREAD_RWLOCK_WRITE_WAIT "pthread-rwlock-write-wait"
#define P101_RESOURCE_CLASS_SPAWN_ATTRIBUTES "spawn-attributes"
#define P101_RESOURCE_CLASS_SPAWN_FILE_ACTIONS "spawn-file-actions"
#define P101_RESOURCE_CLASS_STDIO_STREAM "stdio-stream"
#define P101_RESOURCE_CLASS_SYSLOG_SESSION "syslog-session"
#define P101_RESOURCE_CLASS_SYSV_MESSAGE_QUEUE "sysv-message-queue"
#define P101_RESOURCE_CLASS_SYSV_SEMAPHORE_SET "sysv-semaphore-set"
#define P101_RESOURCE_CLASS_SYSV_SHARED_MEMORY "sysv-shared-memory"
#define P101_RESOURCE_CLASS_SYSV_SHARED_MEMORY_ATTACHMENT "sysv-shared-memory-attachment"
#define P101_RESOURCE_CLASS_WORDEXP_RESULT "wordexp-result"

#ifdef __cplusplus
}
#endif

#endif    // LIBP101_ENV_RESOURCE_CLASSES_H
