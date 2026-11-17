/*
 * types.h — tipos e códigos de retorno comuns a todo o núcleo.
 *
 * Regra de portabilidade: todo valor do guest é uint32_t/int32_t. "int" só
 * aparece como código de erro do host (KSVM_OK / KSVM_E_*), cujos valores
 * cabem em 16 bits (AVR).
 */
#ifndef KSVM_TYPES_H
#define KSVM_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "config.h"

/* Versão da implementação (syscall VERSION) e da ISA (SR ISA_VERSION). */
#define KSVM_VERSION_MAJOR 2u
#define KSVM_VERSION_MINOR 0u
#define KSVM_VERSION_PATCH 0u
#define KSVM_ISA_VERSION   UINT32_C(0x0200)

/* Estado de execução da instância. */
typedef enum ksvm_status {
    KSVM_RUNNING = 0,
    KSVM_HALTED,
    KSVM_FAULT,
    KSVM_YIELD   /* devolveu controle ao host; ksvm_run/ksvm_step retomam */
} ksvm_status;

/* Códigos de retorno das funções de API do host (0 = ok, negativo = erro). */
enum ksvm_error {
    KSVM_OK               = 0,
    KSVM_E_ARG            = -1,  /* argumento inválido */
    KSVM_E_FULL           = -2,  /* sem slot livre no barramento */
    KSVM_E_OVERLAP        = -3,  /* regiões se sobrepõem */
    KSVM_E_RANGE          = -4,  /* faixa de memória do guest inválida */
    KSVM_E_IMAGE_MAGIC    = -5,
    KSVM_E_IMAGE_VERSION  = -6,
    KSVM_E_IMAGE_CHECKSUM = -7,
    KSVM_E_IMAGE_RANGE    = -8,  /* seção fora do arquivo / arquivo curto */
    KSVM_E_IMAGE_ENTRY    = -9,
    KSVM_E_NO_RAM         = -10
};

#endif /* KSVM_TYPES_H */
