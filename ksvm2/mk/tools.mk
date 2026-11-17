# mk/tools.mk — ferramentas de host (ksdis, ksimg, ksvm), HALs de desktop
# (hal_posix, hal_win32) e seus testes. Incluído pelo Makefile principal.
#
#   make tools        build/tools/{ksdis,ksimg,ksvm}
#   make test-tools   test_disasm (disassembler e política de caminho do
#                     hal_fs) + tests/tools/test_runner.sh, com o round-trip
#                     ksas -> ksdis --raw -> ksas (doc 09) usando build/ksas
#   make tools-win    build/win/*.exe com MinGW (pulado se ausente); sob WSL
#                     com interop, roda test_runner.sh contra os .exe
#
# As ferramentas usam um núcleo próprio compilado com trace e grupo 3 de
# syscalls ligados (a config muda o layout de struct ksvm, então núcleo e
# ferramentas precisam ver as mesmas chaves): BUILD=build/tools numa chamada
# recursiva, como o Makefile principal faz para a config "alt".

# Este fragmento é incluído antes de "all": sem isto o primeiro alvo daqui
# viraria o padrão de um "make" sem argumentos.
.DEFAULT_GOAL := all

TOOLS_DIR    := $(BUILD)/tools
WIN_DIR      := $(BUILD)/win
TOOLS_DEFS   := -DKSVM_ENABLE_TRACE=1 -DKSVM_ENABLE_SYSCALL_FS=1
MINGW_PREFIX ?= x86_64-w64-mingw32-

# Variáveis da chamada recursiva (sobrescritas por tools-win).
EXE      ?=
HOST_HAL ?= src/hal/hal_posix.c
TOOLS_LDFLAGS ?=
KSAS_RT ?=

TOOLS_COMMON_SRC = src/tools/toolio.c
KSDIS_SRC = src/tools/ksdis/main.c src/tools/ksdis/symtab.c \
            src/tools/ksdis/listing.c src/tools/ksdis/raw.c
KSIMG_SRC = src/tools/ksimg/main.c
KSVM_SRC  = src/tools/ksvm_main.c $(HOST_HAL) src/hal/hal_fs.c
TT_SRC    = tests/tools/test_disasm.c tests/tools/test_hal_fs.c tests/tools/test_tools_main.c
MKIMG_SRC = tests/tools/mkimg.c

tools_obj = $(patsubst %.c,$(BUILD)/%.o,$(1))
TOOLS_OBJ = $(call tools_obj,$(TOOLS_COMMON_SRC) $(KSDIS_SRC) $(KSIMG_SRC) $(KSVM_SRC) \
                             $(TT_SRC) $(MKIMG_SRC))

.PHONY: tools tools-bins test-tools test-tools-run tools-win tools-win-bins

tools:
	$(MAKE) --no-print-directory tools-bins BUILD=$(TOOLS_DIR) DEFS="$(TOOLS_DEFS)"

tools-bins: $(BUILD)/ksdis$(EXE) $(BUILD)/ksimg$(EXE) $(BUILD)/ksvm$(EXE)

$(BUILD)/ksdis$(EXE): $(call tools_obj,$(KSDIS_SRC) $(TOOLS_COMMON_SRC)) $(LIB)
	$(CC) $(SAN) $^ $(TOOLS_LDFLAGS) -o $@

$(BUILD)/ksimg$(EXE): $(call tools_obj,$(KSIMG_SRC) $(TOOLS_COMMON_SRC)) $(LIB)
	$(CC) $(SAN) $^ $(TOOLS_LDFLAGS) -o $@

$(BUILD)/ksvm$(EXE): $(call tools_obj,$(KSVM_SRC) $(TOOLS_COMMON_SRC)) $(LIB)
	$(CC) $(SAN) $^ $(TOOLS_LDFLAGS) -o $@

# Testes das ferramentas: mesmas flags de aviso do núcleo (inclusive
# -Wconversion), não as TEST_WARN dos testes unitários.
$(BUILD)/tests/tools/%.o: tests/tools/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DEPFLAGS) -Itests/unit -Isrc/hal -c $< -o $@

$(BUILD)/test_disasm$(EXE): $(call tools_obj,$(TT_SRC) src/hal/hal_fs.c) $(LIB)
	$(CC) $(SAN) $^ $(TOOLS_LDFLAGS) -o $@

$(BUILD)/mkimg$(EXE): $(call tools_obj,$(MKIMG_SRC)) $(LIB)
	$(CC) $(SAN) $^ $(TOOLS_LDFLAGS) -o $@

# O round-trip usa o ksas da config padrão (build/ksas): caminho absoluto,
# porque a chamada recursiva troca BUILD.
test-tools: tools ksas
	$(MAKE) --no-print-directory test-tools-run BUILD=$(TOOLS_DIR) DEFS="$(TOOLS_DEFS)" \
	    KSAS_RT="$(CURDIR)/$(KSAS_BIN)"

# Além dos testes: as HALs compilam com o grupo 3 desligado, e hal_win32.c
# compila (vazio) fora do Windows.
test-tools-run: tools-bins $(BUILD)/test_disasm $(BUILD)/mkimg
	./$(BUILD)/test_disasm
	KSAS="$(KSAS_RT)" sh tests/tools/test_runner.sh $(BUILD) $(BUILD)/mkimg
	@for f in src/hal/hal_posix.c src/hal/hal_win32.c src/hal/hal_fs.c; do \
	    $(CC) $(CPPFLAGS) -UKSVM_ENABLE_SYSCALL_FS -DKSVM_ENABLE_SYSCALL_FS=0 $(CFLAGS) \
	        -c $$f -o /dev/null || exit 1; \
	done; echo "test-tools: HALs com KSVM_ENABLE_SYSCALL_FS=0: ok"

# Windows (MinGW): ferramentas + test_disasm.exe + mkimg.exe. Sob WSL com
# interop os testes rodam aqui mesmo; sem interop, rodar no Git Bash do
# Windows: build/win/test_disasm.exe && sh tests/tools/test_runner.sh
# build/win build/win/mkimg.exe .exe
tools-win:
	@if command -v $(MINGW_PREFIX)gcc >/dev/null 2>&1; then \
	    $(MAKE) --no-print-directory tools-win-bins BUILD=$(WIN_DIR) DEFS="$(TOOLS_DEFS)" \
	        CC=$(MINGW_PREFIX)gcc AR=$(MINGW_PREFIX)ar EXE=.exe \
	        HOST_HAL=src/hal/hal_win32.c TOOLS_LDFLAGS=-static || exit 1; \
	    if ls /proc/sys/fs/binfmt_misc/ 2>/dev/null | grep -q '^WSLInterop'; then \
	        $(WIN_DIR)/test_disasm.exe && \
	        sh tests/tools/test_runner.sh $(WIN_DIR) $(WIN_DIR)/mkimg.exe .exe || exit 1; \
	    else \
	        echo "tools-win: sem interop WSL para executar .exe; apenas compilado"; \
	    fi; \
	else \
	    echo "tools-win: $(MINGW_PREFIX)gcc nao encontrado, pulando"; \
	fi

tools-win-bins: tools-bins $(BUILD)/test_disasm$(EXE) $(BUILD)/mkimg$(EXE)

-include $(TOOLS_OBJ:.o=.d)
