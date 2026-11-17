/*
 * test_hal_fs.c — política de caminho do OPEN (src/hal/hal_fs.h).
 */
#include "hal_fs.h"
#include "test_tools.h"

static const char *const accepted[] = {
    "out.txt", "dir/file.bin", "a.b.c", "dir\\sub\\x", ".hidden", "x..y",
    "console", "con1", "com10", "lpt", "nul_x", "a/conx.txt", "auxiliar.txt",
    "prn2.log", "dir/com.txt", "CONIN", "conout",
};

static const char *const rejected[] = {
    "", "/etc/passwd", "\\x", "c:x", "a/b:c", "..", "a/../b", "a\\..\\b", ". .", "...",
    /* dispositivos do Windows, com ou sem extensão, qualquer caixa */
    "CON", "con", "Con.txt", "nul", "NUL.tar.gz", "prn", "aux", "AUX ", "aux .log",
    "con.", "COM1", "com9.log", "Com0", "LPT1", "lpt9.txt", "LPT0",
    "CONIN$", "conout$", "CONOUT$.x", "dir/nul", "a/b/Com3.dat", "nul/x",
};

TEST(accepts_ordinary_paths)
{
    size_t i;
    for (i = 0; i < sizeof accepted / sizeof accepted[0]; i++) {
        if (!ksvm_fs_path_ok(accepted[i])) {
            test_note(t_, "recusado: \"%s\"", accepted[i]);
        }
        ASSERT_TRUE(ksvm_fs_path_ok(accepted[i]));
    }
}

TEST(rejects_escapes_and_device_names)
{
    size_t i;
    for (i = 0; i < sizeof rejected / sizeof rejected[0]; i++) {
        if (ksvm_fs_path_ok(rejected[i])) {
            test_note(t_, "aceito: \"%s\"", rejected[i]);
        }
        ASSERT_FALSE(ksvm_fs_path_ok(rejected[i]));
    }
}

SUITE(hal_fs)
{
    RUN_TEST(accepts_ordinary_paths);
    RUN_TEST(rejects_escapes_and_device_names);
}
