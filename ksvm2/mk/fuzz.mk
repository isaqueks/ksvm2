# mk/fuzz.mk — fuzzer de execução (tests/fuzz/fuzz_exec.c).
#
#   make fuzz        FUZZ_SEEDS (1000) sementes, -O2
#   make fuzz-asan   FUZZ_ASAN_SEEDS (200) sementes, ASan + UBSan
#   make fuzz-quick  FUZZ_QUICK_SEEDS (100) sementes, -O2 (parte do check-all)
#
# Cada alvo roda quatro variantes: bytes aleatórios e instruções válidas
# (-DKSVM_FUZZ_STRUCTURED=1), cada uma na config padrão e na "alt" (trace,
# MULH sem uint64_t, 3 regiões). Núcleo, hal_null e fuzzer são compilados
# juntos com os mesmos avisos do núcleo (-Wconversion incluso, -Werror).

# Este fragmento é incluído antes de "all": não roubar o alvo padrão.
ifeq ($(.DEFAULT_GOAL),)
.DEFAULT_GOAL := all
endif

FUZZ_SRC        = tests/fuzz/fuzz_exec.c
FUZZ_SEEDS      ?= 1000
FUZZ_ASAN_SEEDS ?= 200
FUZZ_FIRST      ?= 1
FUZZ_STEPS      ?= 100000
FUZZ_DIR        = $(BUILD)/fuzz

.PHONY: fuzz fuzz-asan

# $(1) = binário, $(2) = flags extras, $(3) = sementes
define fuzz_build_run
	$(CC) $(CPPFLAGS) $(CSTD) $(WARN) $(2) $(CORE_SRC) $(HAL_SRC) $(FUZZ_SRC) -o $(FUZZ_DIR)/$(1)
	./$(FUZZ_DIR)/$(1) $(3) $(FUZZ_FIRST) $(FUZZ_STEPS)
endef

fuzz:
	@mkdir -p $(FUZZ_DIR)
	$(call fuzz_build_run,fuzz_raw,-O2,$(FUZZ_SEEDS))
	$(call fuzz_build_run,fuzz_struct,-O2 -DKSVM_FUZZ_STRUCTURED=1,$(FUZZ_SEEDS))
	$(call fuzz_build_run,fuzz_raw_alt,-O2 $(ALT_DEFS),$(FUZZ_SEEDS))
	$(call fuzz_build_run,fuzz_struct_alt,-O2 $(ALT_DEFS) -DKSVM_FUZZ_STRUCTURED=1,$(FUZZ_SEEDS))

fuzz-asan:
	@mkdir -p $(FUZZ_DIR)
	$(call fuzz_build_run,fuzz_raw_asan,-O1 -g $(ASAN),$(FUZZ_ASAN_SEEDS))
	$(call fuzz_build_run,fuzz_struct_asan,-O1 -g $(ASAN) -DKSVM_FUZZ_STRUCTURED=1,$(FUZZ_ASAN_SEEDS))
	$(call fuzz_build_run,fuzz_raw_alt_asan,-O1 -g $(ASAN) $(ALT_DEFS),$(FUZZ_ASAN_SEEDS))
	$(call fuzz_build_run,fuzz_struct_alt_asan,-O1 -g $(ASAN) $(ALT_DEFS) -DKSVM_FUZZ_STRUCTURED=1,$(FUZZ_ASAN_SEEDS))

# Versão curta para o check-all: as mesmas quatro variantes de "fuzz" com
# FUZZ_QUICK_SEEDS (100) sementes.
FUZZ_QUICK_SEEDS ?= 100

.PHONY: fuzz-quick

fuzz-quick:
	$(MAKE) --no-print-directory fuzz FUZZ_SEEDS=$(FUZZ_QUICK_SEEDS)
