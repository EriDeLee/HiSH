/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "tracee/tracee.h"
#include "arch.h"
int hish_prepare_exec(Tracee *tracee);
int hish_platform_bindings(Tracee *tracee);
int hish_platform_path(const char *path,char *result);
word_t hish_script_end(Tracee *tracee,word_t kernel_stack);
int hish_publish_script(Tracee *tracee,word_t kernel_stack,word_t address,const void *data,size_t size);
