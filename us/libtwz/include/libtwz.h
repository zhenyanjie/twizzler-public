#pragma once

#define EXTERNAL __attribute__((visibility("default")))

__attribute__((noreturn)) void libtwz_panic(const char *s, ...);

void libtwz_do_backtrace(void);
