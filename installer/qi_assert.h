#ifndef _QI_ASSERT_H_
#define _QI_ASSERT_H_

#include <stdio.h>

#include "anbui/anbui.h"

static inline void __qi__assert(const char * assertion, const char * file, unsigned int line, const char * func) {
    ad_deinit(); \
    system("clear"); \
    printf("GURU MEDITATION!\n\nArquivo '%s' | Linha %u | Função '%s'\n\nCondição '%s' falhou!\n\nSaindo para o shell.\n", file, line, func, assertion); \
    sync(); \
    abort(); \
}

#define QI_ASSERT(x) ((void)((x) || (__qi__assert(#x, __FILE__, __LINE__, __func__),0)))

#endif
