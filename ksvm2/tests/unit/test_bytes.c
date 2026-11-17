/*
 * test_bytes.c — load/store little-endian e extensão de sinal (bytes.h).
 */
#include "ksvm/bytes.h"
#include "test.h"

TEST(ld16le_is_little_endian)
{
    const uint8_t b[3] = { 0x34, 0x12, 0xFF };
    ASSERT_EQ_U32(ksvm_ld16le(b), 0x1234u);
    ASSERT_EQ_U32(ksvm_ld16le(b + 1), 0xFF12u);   /* endereço ímpar */
}

TEST(ld32le_is_little_endian_and_unaligned)
{
    const uint8_t b[6] = { 0x00, 0x78, 0x56, 0x34, 0x12, 0xFF };
    ASSERT_EQ_U32(ksvm_ld32le(b + 1), 0x12345678u);
    ASSERT_EQ_U32(ksvm_ld32le(b + 2), 0xFF123456u);
}

TEST(ld32le_extremes)
{
    const uint8_t ones[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
    const uint8_t top[4]  = { 0x00, 0x00, 0x00, 0x80 };
    const uint8_t zero[4] = { 0, 0, 0, 0 };
    ASSERT_EQ_U32(ksvm_ld32le(ones), 0xFFFFFFFFu);
    ASSERT_EQ_U32(ksvm_ld32le(top), 0x80000000u);
    ASSERT_EQ_U32(ksvm_ld32le(zero), 0u);
}

TEST(st16le_st32le_byte_order)
{
    uint8_t b[7] = { 0 };
    const uint8_t want[7] = { 0x00, 0xEF, 0xBE, 0xAD, 0xDE, 0x34, 0x12 };
    ksvm_st32le(b + 1, 0xDEADBEEFu);
    ksvm_st16le(b + 5, 0x1234u);
    ASSERT_MEM_EQ(b, want, sizeof want);
}

TEST(store_load_roundtrip)
{
    static const uint32_t vals[] = { 0u, 1u, 0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu, 0x01020304u };
    uint8_t b[5];
    size_t i;
    for (i = 0; i < sizeof vals / sizeof vals[0]; i++) {
        ksvm_st32le(b + 1, vals[i]);
        ASSERT_EQ_U32(ksvm_ld32le(b + 1), vals[i]);
        ksvm_st16le(b, (uint16_t)vals[i]);
        ASSERT_EQ_U32(ksvm_ld16le(b), vals[i] & 0xFFFFu);
    }
}

TEST(sext8_16)
{
    ASSERT_EQ_U32(ksvm_sext8(0x7Fu), 0x7Fu);
    ASSERT_EQ_U32(ksvm_sext8(0x80u), 0xFFFFFF80u);
    ASSERT_EQ_U32(ksvm_sext8(0xFFu), 0xFFFFFFFFu);
    ASSERT_EQ_U32(ksvm_sext8(0x12345600u), 0u);         /* só os 8 bits baixos */
    ASSERT_EQ_U32(ksvm_sext16(0x7FFFu), 0x7FFFu);
    ASSERT_EQ_U32(ksvm_sext16(0x8000u), 0xFFFF8000u);
    ASSERT_EQ_U32(ksvm_sext16(0xABCDFFFFu), 0xFFFFFFFFu);
}

TEST(sext14_20_24)
{
    ASSERT_EQ_U32(ksvm_sext14(0x1FFFu), 8191u);
    ASSERT_EQ_U32(ksvm_sext14(0x2000u), 0xFFFFE000u);   /* -8192 */
    ASSERT_EQ_U32(ksvm_sext14(0x3FFFu), 0xFFFFFFFFu);
    ASSERT_EQ_U32(ksvm_sext20(0x7FFFFu), 0x7FFFFu);
    ASSERT_EQ_U32(ksvm_sext20(0x80000u), 0xFFF80000u);
    ASSERT_EQ_U32(ksvm_sext24(0x7FFFFFu), 0x7FFFFFu);
    ASSERT_EQ_U32(ksvm_sext24(0x800000u), 0xFF800000u);
    ASSERT_EQ_U32(ksvm_sext24(0xFFFFFFFFu), 0xFFFFFFFFu);
}

TEST(sext_full_width_is_identity)
{
    ASSERT_EQ_U32(ksvm_sext(0x80000000u, 32), 0x80000000u);
    ASSERT_EQ_U32(ksvm_sext(0x12345678u, 32), 0x12345678u);
    ASSERT_EQ_U32(ksvm_sext(1u, 1), 0xFFFFFFFFu);
    ASSERT_EQ_U32(ksvm_sext(2u, 1), 0u);
}

TEST(u32_to_i32_extremes)
{
    ASSERT_EQ(ksvm_u32_to_i32(0u), 0);
    ASSERT_EQ(ksvm_u32_to_i32(0x7FFFFFFFu), INT32_MAX);
    ASSERT_EQ(ksvm_u32_to_i32(0x80000000u), INT32_MIN);
    ASSERT_EQ(ksvm_u32_to_i32(0xFFFFFFFFu), -1);
    ASSERT_EQ(ksvm_u32_to_i32(0xFFFFFFFEu), -2);
}

SUITE(bytes)
{
    RUN_TEST(ld16le_is_little_endian);
    RUN_TEST(ld32le_is_little_endian_and_unaligned);
    RUN_TEST(ld32le_extremes);
    RUN_TEST(st16le_st32le_byte_order);
    RUN_TEST(store_load_roundtrip);
    RUN_TEST(sext8_16);
    RUN_TEST(sext14_20_24);
    RUN_TEST(sext_full_width_is_identity);
    RUN_TEST(u32_to_i32_extremes);
}
