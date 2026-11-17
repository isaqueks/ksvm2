# mk/ksas.mk — assembler ksas (doc 07) e seus testes.
#
#   make ksas            build/ksas
#   make test-ksas       testes do assembler (lexer, expressões, codificação,
#                        diretivas, erros e execução de programas na VM)
#   make test-ksas-asan  idem com AddressSanitizer + UBSan
#   make ksas-examples   monta examples/*.ksasm e examples/v1_ported/*.ksasm
#                        com listing em build/examples/
#
# Incluído pelo Makefile principal antes do alvo "all": o bloco abaixo
# garante que "make" sem argumentos continue construindo "all".
ifeq ($(.DEFAULT_GOAL),)
.DEFAULT_GOAL := all
endif

KSAS_DIR      = src/tools/ksas
KSAS_SRC      = $(filter-out $(KSAS_DIR)/main.c,$(wildcard $(KSAS_DIR)/*.c))
KSAS_OBJ      = $(KSAS_SRC:%.c=$(BUILD)/%.o)
KSAS_MAIN_OBJ = $(BUILD)/$(KSAS_DIR)/main.o
KSAS_BIN      = $(BUILD)/ksas
KSAS_TEST_SRC = $(wildcard tests/ksas/*.c)
KSAS_TEST_OBJ = $(KSAS_TEST_SRC:%.c=$(BUILD)/%.o)
KSAS_TEST_BIN = $(BUILD)/ksas_tests
KSAS_EXAMPLES = $(wildcard examples/*.ksasm examples/v1_ported/*.ksasm)

.PHONY: ksas test-ksas run-ksas-tests test-ksas-asan ksas-examples

ksas: $(KSAS_BIN)

$(KSAS_BIN): $(KSAS_MAIN_OBJ) $(KSAS_OBJ) $(LIB)
	$(CC) $(SAN) $^ -o $@

# Os testes usam os headers internos do assembler e o harness de tests/unit.
$(KSAS_TEST_OBJ): CPPFLAGS += -I$(KSAS_DIR) -Itests/unit

$(KSAS_TEST_BIN): $(KSAS_TEST_OBJ) $(KSAS_OBJ) $(HAL_OBJ) $(LIB)
	$(CC) $(SAN) $^ -o $@

# Roda a partir da raiz do projeto: os testes leem examples/.
run-ksas-tests: $(KSAS_TEST_BIN)
	./$(KSAS_TEST_BIN)

test-ksas:
	$(MAKE) --no-print-directory run-ksas-tests

test-ksas-asan:
	$(MAKE) --no-print-directory run-ksas-tests BUILD=$(BUILD)/asan OPT="-O1 -g" SAN="$(ASAN)"

ksas-examples: $(KSAS_BIN)
	@mkdir -p $(BUILD)/examples
	@for f in $(KSAS_EXAMPLES); do \
	    out=$(BUILD)/examples/$$(basename $$f .ksasm); \
	    ./$(KSAS_BIN) $$f -o $$out.ksx -l $$out.lst || exit 1; \
	    echo "  ksas $$f -> $$out.ksx ($$out.lst)"; \
	done

-include $(KSAS_OBJ:.o=.d) $(KSAS_MAIN_OBJ:.o=.d) $(KSAS_TEST_OBJ:.o=.d)
