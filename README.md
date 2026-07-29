# lib_env Repository Guide

Welcome to the `lib_env` repository — environment/tracing utilities, part of the Programming 101 C library collection. This guide will help you set up, build, and install the library.

## **Table of Contents**

1. [Cloning the Repository](#cloning-the-repository)
2. [Prerequisites](#prerequisites)
3. [Configuring the Build](#configuring-the-build)
4. [Building](#building)
5. [Testing](#testing)
6. [Installing](#installing)
7. [Adding or Removing Files](#adding-or-removing-files)

## **Cloning the Repository**

Clone the repository using the following command:

```bash
git clone https://github.com/programming101dev/lib_env.git
```

Navigate to the cloned directory:

```bash
cd lib_env
```

Ensure the scripts are executable:

```bash
chmod +x *.sh
```

## **Prerequisites**

To ensure you have all of the required tools installed, run:

```bash
./check-env.sh
```

If you are missing tools follow these [instructions](https://docs.google.com/document/d/1ZPqlPD1mie5iwJ2XAcNGz7WeA86dTLerFXs9sAuwCco/edit?usp=drive_link). If something still looks wrong, `./doctor.sh` reports what actually works on this machine for this project.

## **Configuring the Build**

Tell CMake which compiler you want to use:

```bash
./change-compiler.sh -c <compiler>
```

To see the list of possible compilers:

```bash
cat supported_c_compilers.txt
```

Run it again any time to switch compilers; each compiler configures into its own build directory (e.g. `build-clang`, `build-gcc-15`).

## **Building**

To build the library run:

```bash
./build.sh
```

This compiles through the strict analysis pipeline: the clang-format check, clang-tidy, cppcheck, the Clang static analyzer, and hundreds of warnings under `-Werror`. `./build.sh -f` applies the formatter and tidy fixes in place.

## **Testing**

`./check.sh` is the one command to run before you submit: the format check, the strict build, the tests, and a short fuzz smoke run, with a single PASS/FAIL at the end.
The event-library test tree covers v2 compatibility, v3 round trips, long records, reader
errors, duplicated log destinations, and independent resource observers. The
fuzz harness feeds arbitrary records through the shared event parser.

## **Tracing and structured call logs**

`P101_TRACE(env)` logs function entry through the installed tracer. `P101_TRACE_EXIT(env)` logs function exit through the installed exit tracer. These remain the lightweight call-tree hooks.

For strace/ltrace-style tooling, `lib_env` also provides a structured call observer. Existing `P101_TRACE` / `P101_TRACE_EXIT` calls emit `P101CALL` enter/exit records with no parameter text. Functions that want parameter or return-value text can use:

```c
P101_CALL_ENTER(env, "open", "path=\"config.txt\" flags=O_RDONLY");
P101_CALL_EXIT(env, "open", "ret=3 errno=0");
```

The built-in log sink is opt-in:

```c
p101_env_set_call_log(env,
                      stderr,
                      P101_ENV_CALL_LOG_ENTER |
                          P101_ENV_CALL_LOG_EXIT |
                          P101_ENV_CALL_LOG_ARGUMENTS |
                          P101_ENV_CALL_LOG_RESULT);
```

Omit `P101_ENV_CALL_LOG_ARGUMENTS` and/or `P101_ENV_CALL_LOG_RESULT` to suppress parameter or return-value text even when call sites provide it. User code can install its own observer with `p101_env_set_call_observer()`, but observers must not call p101 wrappers because that would recurse.

## **Environment-controlled diagnostics**

`p101_env_create()` also honors an opt-in environment contract. If these
variables are absent, the env behaves normally.

Fault injection:

```sh
P101_FAULT_CALL=3 ./program
P101_FAULT_CALL=2 P101_FAULT_ERRNO=5 P101_FAULT_NAME=open ./program
P101_FAULT_CALL=2 P101_FAULT_LOG=fault.log ./program
P101_FAULT_CALL=2 P101_FAULT_NAME=read P101_FAULT_MODE=short P101_FAULT_AMOUNT=4 ./program
P101_FAULT_CALL=2 P101_FAULT_MODE=eintr P101_FAULT_REPEAT=3 ./program
```

- `P101_FAULT_CALL=N` enables injection and fails the Nth matching
  fault-capable p101 wrapper call.
- `P101_FAULT_ERRNO=E` chooses the errno reported by the failed wrapper. The
  default is `EIO`.
- `P101_FAULT_NAME=name` optionally narrows counting to one wrapper name, such
  as `open`, `read`, or `socket`.
- `P101_FAULT_MODE=error|eintr|timeout|short` selects a hard failure, an
  `EINTR` failure, a timeout-style failure, or a successful short I/O. Short
  mode is currently implemented by `read`, `write`, `pread`, and `pwrite`.
- `P101_FAULT_AMOUNT=N` sets the maximum byte count passed to a wrapper during
  short-I/O injection. The default is one byte.
- `P101_FAULT_REPEAT=N` injects the selected behavior for `N` consecutive
  matching calls. This makes retry loops and repeated interruption testable.
- `P101_FAULT_LOG=path` records when a configured fault actually fires:
  `P101FAULT<TAB>2<TAB>pid<TAB>call-index<TAB>call-name<TAB>errno<TAB>mode<TAB>amount`. The
  `error-path-walk` tool uses this to stop automatically when it has walked
  past the last fault-capable call.

Resource and call logs:

```sh
P101_RESOURCE_LOG=resources.log ./program
P101_CALL_LOG=calls.log P101_CALL_LOG_ARGS=1 P101_CALL_LOG_RESULT=1 ./program
```

- `P101_RESOURCE_LOG=path` enables the resource event log read by
  `resource-tracker`. The stream includes descriptor records (`P101FD`) and
  allocation records (`P101ALLOC`) when code uses the p101 descriptor and heap
  wrappers, plus generic lifecycle records (`P101RESOURCE`) emitted through the
  `P101_TRACK_RESOURCE_*` macros.
- `P101_CALL_LOG=path` enables structured `P101CALL` records.
- `P101_CALL_LOG_ENTER=0` or `P101_CALL_LOG_EXIT=0` can suppress enter/exit
  records.
- `P101_CALL_LOG_ARGS=1` includes argument text supplied by `P101_CALL_ENTER`.
- `P101_CALL_LOG_RESULT=1` includes result text supplied by `P101_CALL_EXIT`.

Use `-` as the log path to write to `stderr`; otherwise the env opens the file
in append mode and closes it in `p101_env_destroy()`.

Event sequence allocation is atomic, and `lib_tool_event` publishes each complete
record as one locked stream write. If any event or fault-log write fails,
`p101_env_event_log_failed()` remains true until
`p101_env_clear_event_log_error()` is called;
`p101_env_event_log_errno()` returns the first recorded error. Destruction also
prints a warning to `stderr`, so a partial instrumentation log cannot look
silently complete. The environment's other mutable configuration remains
caller-synchronized. One env per thread remains the general rule; the built-in
event-log observers are the narrow exception and may emit concurrently after
configuration.

All event consumers use `P101_TOOL_EVENT_LINE_MAX_BYTES` from `lib_tool_event` as the shared record
ceiling. A record that does not fit, contains an embedded NUL, or has an
unsupported version is malformed input rather than a partially parsed event.

## **Installing**

To install the library run:

```bash
./install.sh
```

You may need to run it via sudo, or give the user account access to the install directories. `./uninstall.sh` removes it again.

## **Adding or Removing Files**

The `CMakeLists.txt` is fixed and shared across every repository — do not edit it. When you add or remove a source or header, edit the lists in `config.cmake` (`p101_env_SOURCES`, `p101_env_HEADERS`, and `p101_env_LINK_LIBRARIES`), then re-configure and build:

```bash
./change-compiler.sh -c <compiler>
./build.sh
```
