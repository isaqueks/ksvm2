# mk/ports.mk — portas embarcadas (fase 4): ATmega328P (simavr) e
# Cortex-M (QEMU mps2-an385). Incluído pelo Makefile principal.
#
#   make ports-avr    build/ports/avr/avr-{hello,blink,selftest,fselftest,fault}.elf
#   make test-avr     roda cada firmware no simavr e confere a UART
#   make size-avr     flash/RAM por módulo (mapa do linker) e meta RNF-04
#   make ports-arm    build/ports/arm/arm-{hello,blink,selftest,fselftest,fault}.elf
#   make test-arm     roda cada firmware no qemu-system-arm e confere a UART
#   make test-ports   test-avr + test-arm
#   make avr-custom KSX=prog.ksx / arm-custom KSX=prog.ksx   firmware com um
#                     programa próprio (fim deste arquivo)
#
# Atalhos: avr-hello.elf, avr-blink.elf, avr-selftest.elf, arm-hello.elf...
# Todo alvo de topo pula com aviso se o toolchain faltar.
#
# Os programas do guest são gerados por ports/common/genprogs.c (compilado
# para o host com os codificadores do núcleo) e convertidos em
# program_image.h por ports/avr/mkimage.py.

# Este fragmento é lido antes de "all:" no Makefile: sem isto o primeiro
# alvo daqui viraria o padrão.
.DEFAULT_GOAL := all

PORTS_DIR  = $(BUILD)/ports
PORTS_GEN  = $(PORTS_DIR)/genprogs
PORTS_KSX  = $(PORTS_DIR)/ksx
PORTS_PY  ?= python3
PORTS_RUN  = sh ports/common/expect_run.sh
PORTS_HDRS = $(wildcard include/ksvm/*.h) include/ksvm/isa.def

.PHONY: ports-avr test-avr size-avr ports-arm test-arm test-ports \
        avr-elfs arm-elfs avr-size-report avr-config-check \
        avr-hello.elf avr-blink.elf avr-selftest.elf avr-fselftest.elf avr-fault.elf \
        arm-hello.elf arm-blink.elf arm-selftest.elf arm-fselftest.elf arm-fault.elf

# ---- Programas do guest (host) ----------------------------------------

$(PORTS_GEN): ports/common/genprogs.c $(CORE_SRC) $(PORTS_HDRS)
	@mkdir -p $(dir $@)
	$(CC) -Iinclude $(CSTD) -O1 $(WARN) ports/common/genprogs.c $(CORE_SRC) -o $@

$(PORTS_KSX)/hello-avr.ksx: $(PORTS_GEN)
	@mkdir -p $(dir $@)
	$(PORTS_GEN) hello $@ "KSVM on AVR"
$(PORTS_KSX)/hello-arm.ksx: $(PORTS_GEN)
	@mkdir -p $(dir $@)
	$(PORTS_GEN) hello $@ "KSVM on ARM"
$(PORTS_KSX)/blink.ksx: $(PORTS_GEN)
	@mkdir -p $(dir $@)
	$(PORTS_GEN) blink $@ 6
$(PORTS_KSX)/selftest.ksx $(PORTS_KSX)/fselftest.ksx $(PORTS_KSX)/fault.ksx: $(PORTS_KSX)/%.ksx: $(PORTS_GEN)
	@mkdir -p $(dir $@)
	$(PORTS_GEN) $* $@

# Tabela compacta da ISA (KSVM_ISA_COMPACT, usada no AVR) contra isa.def.
$(PORTS_DIR)/isa_compact_check: ports/common/isa_compact_check.c src/core/isa.c $(PORTS_HDRS)
	@mkdir -p $(dir $@)
	$(CC) -Iinclude $(CSTD) -O1 $(WARN) -DKSVM_ISA_COMPACT=1 \
	    ports/common/isa_compact_check.c src/core/isa.c -o $@

# ---- AVR (ATmega328P) --------------------------------------------------

AVR_DIR     = $(PORTS_DIR)/avr
AVR_MCU     = atmega328p
AVR_F_CPU   = 16000000UL
AVR_PROGS   = hello blink selftest fselftest fault
AVR_DEFS    =
# Medido (firmware hello, antes das reduções no código): -Os puro 16174 B;
# + -mcall-prologues 15084; + -mrelax 14840; + -mstrict-X
# -fno-inline-small-functions 14584. -flto piora com avr-gcc 7.3 (15148).
#
# -fno-jump-tables é OBRIGATÓRIO junto com -mrelax no avr-ld 2.26 (binutils
# do Ubuntu 24.04): ao encurtar um "jmp __tablejump2__" para rjmp logo antes
# de uma tabela de salto, o linker não ajusta o rótulo da tabela que fica
# exatamente no fim dos bytes removidos; a base calculada aponta 2 bytes
# antes e cada "case" salta para o código do anterior (visto no switch de
# exec_move: MOVI executava como MOV). Dependia do layout; o test-avr pegou.
# Sem tabelas de salto o custo é +48 bytes.
AVR_OPT     = -Os -ffunction-sections -fdata-sections -mcall-prologues -mrelax \
              -mstrict-X -fno-inline-small-functions -fno-jump-tables
AVR_FLAGS   = -mmcu=$(AVR_MCU) -DF_CPU=$(AVR_F_CPU) $(CSTD) $(WARN) $(AVR_OPT) \
              -Iinclude -Iports/avr -DKSVM_HAVE_USER_CONFIG $(AVR_DEFS)
AVR_LDFLAGS = -mmcu=$(AVR_MCU) -mrelax -Wl,--gc-sections
AVR_OBJ     = $(CORE_SRC:src/core/%.c=$(AVR_DIR)/obj/%.o)
AVR_LIB     = $(AVR_DIR)/libksvm_avr.a
AVR_HDRS    = $(PORTS_HDRS) ports/avr/ksvm_config_user.h
AVR_SIM     = simavr -m $(AVR_MCU) -f $(patsubst %UL,%,$(AVR_F_CPU))

$(AVR_DIR)/obj/%.o: src/core/%.c $(AVR_HDRS)
	@mkdir -p $(dir $@)
	avr-gcc $(AVR_FLAGS) -c $< -o $@

$(AVR_DIR)/obj/hal_avr.o: src/hal/hal_avr.c $(AVR_HDRS)
	@mkdir -p $(dir $@)
	avr-gcc $(AVR_FLAGS) -c $< -o $@

# Biblioteca: o linker só puxa os módulos referenciados (ksx_write, encode
# e disasm ficam de fora sem precisar de chave).
$(AVR_LIB): $(AVR_OBJ)
	@rm -f $@
	avr-ar rcs $@ $^

# $(1) = programa, $(2) = .ksx de origem
define AVR_PROG_RULES
$(AVR_DIR)/img/$(1)/program_image.h: $(PORTS_KSX)/$(2).ksx ports/avr/mkimage.py
	@mkdir -p $$(dir $$@)
	$(PORTS_PY) ports/avr/mkimage.py $$< $$@

$(AVR_DIR)/obj/main-$(1).o: ports/avr/main.c $(AVR_DIR)/img/$(1)/program_image.h $(AVR_HDRS)
	@mkdir -p $$(dir $$@)
	avr-gcc $$(AVR_FLAGS) -I$(AVR_DIR)/img/$(1) -c $$< -o $$@

$(AVR_DIR)/avr-$(1).elf: $(AVR_DIR)/obj/main-$(1).o $(AVR_DIR)/obj/hal_avr.o $(AVR_LIB)
	avr-gcc $$(AVR_LDFLAGS) -Wl,-Map,$(AVR_DIR)/avr-$(1).map $$^ -o $$@
endef

$(eval $(call AVR_PROG_RULES,hello,hello-avr))
$(eval $(call AVR_PROG_RULES,blink,blink))
$(eval $(call AVR_PROG_RULES,selftest,selftest))
$(eval $(call AVR_PROG_RULES,fselftest,fselftest))
$(eval $(call AVR_PROG_RULES,fault,fault))

avr-elfs: $(AVR_PROGS:%=$(AVR_DIR)/avr-%.elf)

avr-hello.elf avr-blink.elf avr-selftest.elf avr-fselftest.elf avr-fault.elf:
	@$(MAKE) --no-print-directory $(AVR_DIR)/$@

AVR_HAVE = command -v avr-gcc >/dev/null 2>&1 && command -v $(PORTS_PY) >/dev/null 2>&1

# O firmware não chama ksvm_op_name/ksvm_fault_name: compila o núcleo inteiro
# também nas combinações que ele não exercita (nomes na flash, tabela
# completa, tabela na RAM), para que esses caminhos não apodreçam.
AVR_CFG_COMBOS = -DKSVM_ISA_NAMES=1~-DKSVM_FAULT_NAMES=1 \
                 -DKSVM_ISA_NAMES=1~-DKSVM_FAULT_NAMES=1~-DKSVM_ISA_COMPACT=0 \
                 -DKSVM_TABLE_PROGMEM=0~-DKSVM_ISA_NAMES=1

avr-config-check:
	@for c in $(AVR_CFG_COMBOS); do \
	    defs=$$(echo "$$c" | tr '~' ' '); \
	    for f in $(CORE_SRC); do \
	        avr-gcc $(AVR_FLAGS) $$defs -c $$f -o /dev/null || exit 1; \
	    done; \
	    echo "avr-config-check: ok ($$defs)"; \
	done

ports-avr:
	@if $(AVR_HAVE); then \
	    $(MAKE) --no-print-directory avr-elfs && avr-size $(AVR_DIR)/avr-*.elf; \
	else echo "ports-avr: avr-gcc/$(PORTS_PY) nao encontrados, pulando"; fi

# Cada firmware termina com "KSVM halt <codigo>" ou "KSVM FAULT ..." e
# dorme com as interrupções desligadas, o que encerra o simavr.
test-avr:
	@if $(AVR_HAVE) && command -v simavr >/dev/null 2>&1; then \
	    $(MAKE) --no-print-directory avr-elfs $(PORTS_DIR)/isa_compact_check || exit 1; \
	    ./$(PORTS_DIR)/isa_compact_check || exit 1; \
	    $(MAKE) --no-print-directory -s avr-config-check || exit 1; \
	    echo "test-avr: simavr $(AVR_MCU) @ $(AVR_F_CPU)"; \
	    fail=0; \
	    $(PORTS_RUN) avr-hello $(AVR_DIR)/avr-hello.log 30 \
	        "KSVM on AVR" "KSVM halt 00000000" -- $(AVR_SIM) $(AVR_DIR)/avr-hello.elf || fail=1; \
	    $(PORTS_RUN) avr-blink $(AVR_DIR)/avr-blink.log 60 \
	        "101010 time ok" "KSVM halt 00000000" -- $(AVR_SIM) $(AVR_DIR)/avr-blink.elf || fail=1; \
	    $(PORTS_RUN) avr-selftest $(AVR_DIR)/avr-selftest.log 60 \
	        "selftest ok" "KSVM halt 00000000" -- $(AVR_SIM) $(AVR_DIR)/avr-selftest.elf || fail=1; \
	    $(PORTS_RUN) avr-fselftest $(AVR_DIR)/avr-fselftest.log 30 \
	        "fselftest: sem extensao F" "KSVM halt 00000000" -- $(AVR_SIM) $(AVR_DIR)/avr-fselftest.elf || fail=1; \
	    $(PORTS_RUN) avr-fault $(AVR_DIR)/avr-fault.log 30 \
	        "KSVM FAULT 04 pc=00000008 addr=7FFFFFF0" -- $(AVR_SIM) $(AVR_DIR)/avr-fault.elf || fail=1; \
	    if [ $$fail -ne 0 ]; then echo "test-avr: FALHOU"; exit 1; fi; \
	    echo "test-avr: ok"; \
	else echo "test-avr: avr-gcc/simavr/$(PORTS_PY) nao encontrados, pulando"; fi

# Tamanho por módulo depois do --gc-sections (lido do mapa do linker), na
# configuração da porta e em variantes para comparação (nome:defs, "~"
# separa defs). A meta RNF-04 (12 KB) vale para núcleo + hal_avr + libgcc.
#   tabela-ram   como antes da fase 4: ksvm_isa[] completa (768 B) na SRAM
#   tabela-768   tabela completa, mas na flash
#   no-int64     MULH/MULHU por produtos de 16 bits
#   sem-pwm      HAL sem PWM_WRITE
#   com-float    extensão F ligada (a porta desliga: não cabe na meta)
# (KSVM_ISA_NAMES/FAULT_NAMES não mudam nada aqui: o firmware não chama
# ksvm_op_name/ksvm_fault_name e o --gc-sections descarta os nomes.)
AVR_SIZE_VARIANTS = tabela-ram:-DKSVM_TABLE_PROGMEM=0~-DKSVM_ISA_COMPACT=0 \
                    tabela-768:-DKSVM_ISA_COMPACT=0 \
                    no-int64:-DKSVM_NO_INT64=1 \
                    sem-pwm:-DKSVM_HAL_AVR_PWM=0 \
                    com-float:-DKSVM_ENABLE_FLOAT=1

avr-size-report: $(AVR_DIR)/avr-hello.elf
	@echo "== size-avr: porta (config de ports/avr/ksvm_config_user.h) =="
	@$(PORTS_PY) ports/avr/size_report.py --budget 12288 $(AVR_DIR)/avr-hello.map
	@sz=$$(avr-nm -S $(AVR_DIR)/avr-hello.elf | awk '$$4 == "vm" { print $$2 }'); \
	    echo "sizeof(struct ksvm) no AVR = $$((0x$$sz)) bytes (avr-nm -S, simbolo vm)"
	@for v in $(AVR_SIZE_VARIANTS); do \
	    name=$${v%%:*}; defs=$$(echo "$${v#*:}" | tr '~' ' '); \
	    $(MAKE) --no-print-directory -s $(AVR_DIR)-$$name/avr-hello.elf \
	        AVR_DIR=$(AVR_DIR)-$$name AVR_DEFS="$$defs" >/dev/null || exit 1; \
	    echo "== variante $$name ($$defs) =="; \
	    $(PORTS_PY) ports/avr/size_report.py --summary $(AVR_DIR)-$$name/avr-hello.map; \
	done

size-avr:
	@if $(AVR_HAVE); then \
	    $(MAKE) --no-print-directory avr-size-report; \
	else echo "size-avr: avr-gcc/$(PORTS_PY) nao encontrados, pulando"; fi

# ---- Cortex-M (QEMU mps2-an385) ----------------------------------------
#
# Código gerado para Cortex-M0 (ARMv6-M, o alvo mais restrito: sem divisão
# em hardware, sem acesso desalinhado) e executado no Cortex-M3 do
# mps2-an385 (o QEMU 8.2 não falha em acesso desalinhado nem com
# CCR.UNALIGN_TRP: essa garantia vem do UBSan em test-asan). Saída pela UART0
# CMSDK (serial do -nographic) e término por semihosting SYS_EXIT. Sem
# CMSIS e sem crt0: startup.c + linker.ld; memcpy/memset da newlib-nano.

ARM_DIR     = $(PORTS_DIR)/arm
ARM_CPU     = cortex-m0
ARM_PROGS   = hello blink selftest fselftest fault
# Config de MCU (sem ksvm_config_user.h: a flash é endereçável e não
# precisa de KSVM_TABLE_PROGMEM). Com os padrões de desktop a tabela com
# ponteiros de nome mantém as strings vivas: isa.o vai de ~2,6 KB para ~0,5 KB.
ARM_DEFS    = -DKSVM_ISA_COMPACT=1 -DKSVM_ISA_NAMES=0 -DKSVM_FAULT_NAMES=0 \
              -DKSVM_MAX_REGIONS=3
ARM_FLAGS   = -mcpu=$(ARM_CPU) -mthumb -Os -ffunction-sections -fdata-sections \
              -ffreestanding $(CSTD) $(WARN) -Iinclude -DKSVM_ARM_CM_SEMIHOSTING=1 $(ARM_DEFS)
ARM_LDFLAGS = -mcpu=$(ARM_CPU) -mthumb -nostartfiles --specs=nano.specs \
              -T ports/arm_cm/linker.ld -Wl,--gc-sections
ARM_OBJ     = $(CORE_SRC:src/core/%.c=$(ARM_DIR)/obj/%.o)
ARM_LIB     = $(ARM_DIR)/libksvm_arm.a
ARM_QEMU    = qemu-system-arm -M mps2-an385 -nographic -semihosting -kernel

$(ARM_DIR)/obj/%.o: src/core/%.c $(PORTS_HDRS)
	@mkdir -p $(dir $@)
	arm-none-eabi-gcc $(ARM_FLAGS) -c $< -o $@

$(ARM_DIR)/obj/hal_arm_cm.o: src/hal/hal_arm_cm.c $(PORTS_HDRS)
	@mkdir -p $(dir $@)
	arm-none-eabi-gcc $(ARM_FLAGS) -c $< -o $@

$(ARM_DIR)/obj/startup.o: ports/arm_cm/startup.c $(PORTS_HDRS)
	@mkdir -p $(dir $@)
	arm-none-eabi-gcc $(ARM_FLAGS) -c $< -o $@

$(ARM_LIB): $(ARM_OBJ)
	@rm -f $@
	arm-none-eabi-ar rcs $@ $^

# $(1) = programa, $(2) = .ksx de origem
define ARM_PROG_RULES
$(ARM_DIR)/img/$(1)/program_image.h: $(PORTS_KSX)/$(2).ksx ports/avr/mkimage.py
	@mkdir -p $$(dir $$@)
	$(PORTS_PY) ports/avr/mkimage.py $$< $$@

$(ARM_DIR)/obj/main-$(1).o: ports/arm_cm/main.c $(ARM_DIR)/img/$(1)/program_image.h $(PORTS_HDRS)
	@mkdir -p $$(dir $$@)
	arm-none-eabi-gcc $$(ARM_FLAGS) -I$(ARM_DIR)/img/$(1) -c $$< -o $$@

$(ARM_DIR)/arm-$(1).elf: $(ARM_DIR)/obj/startup.o $(ARM_DIR)/obj/main-$(1).o \
                         $(ARM_DIR)/obj/hal_arm_cm.o $(ARM_LIB) ports/arm_cm/linker.ld
	arm-none-eabi-gcc $$(ARM_LDFLAGS) -Wl,-Map,$(ARM_DIR)/arm-$(1).map \
	    $(ARM_DIR)/obj/startup.o $(ARM_DIR)/obj/main-$(1).o \
	    $(ARM_DIR)/obj/hal_arm_cm.o $(ARM_LIB) -lgcc -o $$@
endef

$(eval $(call ARM_PROG_RULES,hello,hello-arm))
$(eval $(call ARM_PROG_RULES,blink,blink))
$(eval $(call ARM_PROG_RULES,selftest,selftest))
$(eval $(call ARM_PROG_RULES,fselftest,fselftest))
$(eval $(call ARM_PROG_RULES,fault,fault))

arm-elfs: $(ARM_PROGS:%=$(ARM_DIR)/arm-%.elf)

arm-hello.elf arm-blink.elf arm-selftest.elf arm-fselftest.elf arm-fault.elf:
	@$(MAKE) --no-print-directory $(ARM_DIR)/$@

ARM_HAVE = command -v arm-none-eabi-gcc >/dev/null 2>&1 && command -v $(PORTS_PY) >/dev/null 2>&1

ports-arm:
	@if $(ARM_HAVE); then \
	    $(MAKE) --no-print-directory arm-elfs && arm-none-eabi-size $(ARM_DIR)/arm-*.elf; \
	else echo "ports-arm: arm-none-eabi-gcc/$(PORTS_PY) nao encontrados, pulando"; fi

# O GPIO do AN385 não é modelado pelo QEMU (leitura devolve 0), por isso o
# blink espera "000000": o teste confere as syscalls de GPIO sem erro, o
# SysTick e o sleep_ms (tempo medido >= 600 ms), não o nível do pino.
test-arm:
	@if ! { $(ARM_HAVE); }; then \
	    echo "test-arm: arm-none-eabi-gcc/$(PORTS_PY) nao encontrados, pulando"; \
	elif ! command -v qemu-system-arm >/dev/null 2>&1; then \
	    $(MAKE) --no-print-directory arm-elfs || exit 1; \
	    echo "test-arm: qemu-system-arm nao encontrado; firmwares linkados, execucao pulada"; \
	else \
	    $(MAKE) --no-print-directory arm-elfs || exit 1; \
	    echo "test-arm: qemu-system-arm mps2-an385, codigo $(ARM_CPU)"; \
	    fail=0; \
	    $(PORTS_RUN) arm-hello $(ARM_DIR)/arm-hello.log 30 \
	        "KSVM on ARM" "KSVM halt 00000000" -- $(ARM_QEMU) $(ARM_DIR)/arm-hello.elf || fail=1; \
	    $(PORTS_RUN) arm-blink $(ARM_DIR)/arm-blink.log 30 \
	        "000000 time ok" "KSVM halt 00000000" -- $(ARM_QEMU) $(ARM_DIR)/arm-blink.elf || fail=1; \
	    $(PORTS_RUN) arm-selftest $(ARM_DIR)/arm-selftest.log 30 \
	        "selftest ok" "KSVM halt 00000000" -- $(ARM_QEMU) $(ARM_DIR)/arm-selftest.elf || fail=1; \
	    $(PORTS_RUN) arm-fselftest $(ARM_DIR)/arm-fselftest.log 30 \
	        "fselftest ok" "KSVM halt 00000000" -- $(ARM_QEMU) $(ARM_DIR)/arm-fselftest.elf || fail=1; \
	    $(PORTS_RUN) arm-fault $(ARM_DIR)/arm-fault.log 30 \
	        "KSVM FAULT 04 pc=00000008 addr=7FFFFFF0" -- $(ARM_QEMU) $(ARM_DIR)/arm-fault.elf || fail=1; \
	    if [ $$fail -ne 0 ]; then echo "test-arm: FALHOU"; exit 1; fi; \
	    echo "test-arm: ok"; \
	fi

test-ports: test-avr test-arm

# ---- Firmware com um programa próprio ----------------------------------
#
#   make avr-custom KSX=build/prog.ksx   build/ports/avr/avr-custom.elf
#   make arm-custom KSX=build/prog.ksx   build/ports/arm/arm-custom.elf
#
# O .ksx vem do ksas (ex.: build/ksas examples/hello.ksasm -o build/prog.ksx)
# e é copiado a cada chamada para build/ports/ksx/custom.ksx; mkimage.py
# valida magic/versão/checksum e gera o program_image.h embutido.

.PHONY: avr-custom arm-custom ports-custom-force

ports-custom-force:

$(PORTS_KSX)/custom.ksx: ports-custom-force
	@if [ -z "$(KSX)" ]; then echo "informe KSX=prog.ksx"; exit 2; fi
	@mkdir -p $(dir $@)
	cp $(KSX) $@

$(eval $(call AVR_PROG_RULES,custom,custom))
$(eval $(call ARM_PROG_RULES,custom,custom))

avr-custom:
	@if [ -z "$(KSX)" ]; then echo "uso: make avr-custom KSX=prog.ksx"; exit 2; fi
	@$(MAKE) --no-print-directory $(AVR_DIR)/avr-custom.elf KSX="$(KSX)" && \
	    avr-size $(AVR_DIR)/avr-custom.elf

arm-custom:
	@if [ -z "$(KSX)" ]; then echo "uso: make arm-custom KSX=prog.ksx"; exit 2; fi
	@$(MAKE) --no-print-directory $(ARM_DIR)/arm-custom.elf KSX="$(KSX)" && \
	    arm-none-eabi-size $(ARM_DIR)/arm-custom.elf
