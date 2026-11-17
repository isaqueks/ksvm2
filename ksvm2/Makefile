# KSVM v2 — build do núcleo, HAL de teste e testes unitários (GNU make).
#
#   make                 libksvm.a (núcleo)
#   make test            testes nas configs padrão, "alt" e "min"
#   make test-asan       idem com AddressSanitizer + UBSan
#   make check-freestanding / check-avr / check-arm
#   make check-all       definição de "verde": tudo, inclusive fuzz-quick,
#                        test-ksas, test-tools e test-ports (mk/*.mk)
#   make clean
#
# A config "alt" liga caminhos que a padrão não exercita: hook de trace,
# MULH sem uint64_t e só 3 regiões no barramento. A config "min" é a de MCU
# pequeno: sem MUL/DIV nem CLZ/CTZ/POPCNT (viram BAD_OPCODE), sem nomes de
# opcode/fault e com a tabela compacta da ISA (1 byte por opcode).

CC       = gcc
AR       = ar
BUILD    = build
OPT      = -O2
SAN      =
DEFS     =

CSTD     = -std=c99
WARN     = -Wall -Wextra -Wpedantic -Wconversion -Wshadow \
           -Wstrict-prototypes -Wmissing-prototypes -Werror
# Testes: mesmos avisos, exceto -Wconversion (o código de teste mistura
# literais e tipos de propósito; o núcleo é que precisa ser à prova disso).
TEST_WARN = -Wall -Wextra -Wpedantic -Wshadow \
            -Wstrict-prototypes -Wmissing-prototypes -Werror

CPPFLAGS = -Iinclude $(DEFS)
CFLAGS   = $(CSTD) $(OPT) $(WARN) $(SAN)
DEPFLAGS = -MMD -MP

ALT_DEFS = -DKSVM_ENABLE_TRACE=1 -DKSVM_NO_INT64=1 -DKSVM_MAX_REGIONS=3
MIN_DEFS = -DKSVM_ENABLE_MULDIV=0 -DKSVM_ENABLE_BITOPS=0 -DKSVM_ISA_NAMES=0 \
           -DKSVM_FAULT_NAMES=0 -DKSVM_ISA_COMPACT=1
ASAN     = -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer

CORE_SRC = $(wildcard src/core/*.c)
HAL_SRC  = src/hal/hal_null.c
TEST_SRC = $(wildcard tests/unit/*.c)

CORE_OBJ = $(CORE_SRC:%.c=$(BUILD)/%.o)
HAL_OBJ  = $(HAL_SRC:%.c=$(BUILD)/%.o)
TEST_OBJ = $(TEST_SRC:%.c=$(BUILD)/%.o)

LIB      = $(BUILD)/libksvm.a
TEST_BIN = $(BUILD)/ksvm_tests

.PHONY: all test test-asan run-tests clean check-freestanding check-avr check-arm         check-clang check-m32 test-bigendian check-all

# Fragmentos opcionais (ferramentas, fuzz, portas). Cada subprojeto tem o seu
# em mk/<nome>.mk para que possam evoluir sem editar este arquivo.
-include mk/*.mk

all: $(LIB)

$(LIB): $(CORE_OBJ)
	$(AR) rcs $@ $^

$(BUILD)/src/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

$(BUILD)/tests/%.o: tests/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CSTD) $(OPT) $(TEST_WARN) $(SAN) $(DEPFLAGS) -c $< -o $@

$(TEST_BIN): $(TEST_OBJ) $(HAL_OBJ) $(LIB)
	$(CC) $(SAN) $(TEST_OBJ) $(HAL_OBJ) $(LIB) -o $@

# Compila e roda os testes com as variáveis atuais (BUILD, DEFS, SAN...).
run-tests: $(TEST_BIN)
	./$(TEST_BIN)

test:
	$(MAKE) --no-print-directory run-tests
	$(MAKE) --no-print-directory run-tests BUILD=$(BUILD)/alt DEFS="$(ALT_DEFS)"
	$(MAKE) --no-print-directory run-tests BUILD=$(BUILD)/min DEFS="$(MIN_DEFS)"

test-asan:
	$(MAKE) --no-print-directory run-tests BUILD=$(BUILD)/asan OPT="-O1 -g" SAN="$(ASAN)"
	$(MAKE) --no-print-directory run-tests BUILD=$(BUILD)/asan-alt OPT="-O1 -g" SAN="$(ASAN)" DEFS="$(ALT_DEFS)"
	$(MAKE) --no-print-directory run-tests BUILD=$(BUILD)/asan-min OPT="-O1 -g" SAN="$(ASAN)" DEFS="$(MIN_DEFS)"

# O núcleo não pode depender de libc além de <string.h> (memcpy/memset).
check-freestanding:
	@mkdir -p $(BUILD)/freestanding
	@for f in $(CORE_SRC); do \
	    echo "  CC (freestanding) $$f"; \
	    $(CC) $(CPPFLAGS) $(CFLAGS) -ffreestanding -nostdlib -c $$f \
	        -o $(BUILD)/freestanding/$$(basename $$f .c).o || exit 1; \
	done
	@echo "check-freestanding: ok"

# Compiladores cruzados: pulados com aviso se ausentes.
check-avr:
	@if command -v avr-gcc >/dev/null 2>&1; then \
	    mkdir -p $(BUILD)/avr; \
	    for f in $(CORE_SRC); do \
	        avr-gcc -mmcu=atmega328p -Os $(CSTD) $(WARN) $(CPPFLAGS) -c $$f \
	            -o $(BUILD)/avr/$$(basename $$f .c).o || exit 1; \
	    done; \
	    avr-size -t $(BUILD)/avr/*.o; \
	else \
	    echo "check-avr: avr-gcc nao encontrado, pulando"; \
	fi

check-arm:
	@if command -v arm-none-eabi-gcc >/dev/null 2>&1; then \
	    mkdir -p $(BUILD)/arm; \
	    for f in $(CORE_SRC); do \
	        arm-none-eabi-gcc -mcpu=cortex-m0 -mthumb -Os -ffreestanding $(CSTD) $(WARN) \
	            $(CPPFLAGS) -c $$f -o $(BUILD)/arm/$$(basename $$f .c).o || exit 1; \
	    done; \
	    arm-none-eabi-size -t $(BUILD)/arm/*.o; \
	else \
	    echo "check-arm: arm-none-eabi-gcc nao encontrado, pulando"; \
	fi

clean:
	rm -rf $(BUILD)

-include $(CORE_OBJ:.o=.d) $(HAL_OBJ:.o=.d) $(TEST_OBJ:.o=.d)

# --- Verificações cruzadas adicionais -------------------------------------

# Mesmo núcleo, outro compilador: pega extensões GNU acidentais.
check-clang:
	@if command -v clang >/dev/null 2>&1; then 	    for f in $(CORE_SRC) $(HAL_SRC); do 	        clang $(CPPFLAGS) $(CFLAGS) -c $$f -o /dev/null || exit 1; 	    done; echo "check-clang: ok"; 	else echo "check-clang: clang nao encontrado, pulando"; fi

# size_t/long de 32 bits.
check-m32:
	@for f in $(CORE_SRC); do 	    $(CC) -m32 $(CPPFLAGS) $(CFLAGS) -c $$f -o /dev/null || exit 1; 	done; echo "check-m32: ok"

# Roda a suite inteira em PowerPC big-endian (qemu-user): prova que nenhum
# acesso depende do endianness do host.
test-bigendian:
	@if command -v powerpc-linux-gnu-gcc >/dev/null 2>&1 && command -v qemu-ppc >/dev/null 2>&1; then 	    mkdir -p $(BUILD)/ppc; 	    powerpc-linux-gnu-gcc -static $(CPPFLAGS) $(CSTD) $(OPT) $(TEST_WARN) 	        $(CORE_SRC) $(HAL_SRC) $(TEST_SRC) -o $(BUILD)/ppc/ksvm_tests && 	    qemu-ppc $(BUILD)/ppc/ksvm_tests; 	else echo "test-bigendian: powerpc-linux-gnu-gcc/qemu-ppc nao encontrados, pulando"; fi

# Definição de "verde" do projeto: um comando verifica tudo. Os alvos de
# mk/*.mk (fuzz-quick, test-ksas, test-tools, test-ports, size-avr — que falha
# se o AVR passar da meta RNF-04 de 12 KB) pulam com aviso
# o que depender de toolchain ausente.
check-all: test test-asan check-freestanding check-clang check-m32 check-avr check-arm \
           test-bigendian fuzz-quick test-ksas test-tools test-ports size-avr

