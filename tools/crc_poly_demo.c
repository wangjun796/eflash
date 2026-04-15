/* CRC 多项式表示法演示程序
 * 
 * 说明为什么多项式可以用数字如 0xEDB88320 表示
 */

#include <stdio.h>
#include <stdint.h>

/* 32 位位反转函数 */
static uint32_t reverse_bits(uint32_t x)
{
    uint32_t result = 0;
    int i;
    
    for (i = 0; i < 32; i++) {
        result = (result << 1) | (x & 1);
        x >>= 1;
    }
    
    return result;
}

/* 打印二进制表示 */
static void print_binary(const char *label, uint32_t value)
{
    int i;
    printf("%s:\n", label);
    printf("  Hex: 0x%08X\n", value);
    printf("  Bin : ");
    
    for (i = 31; i >= 0; i--) {
        printf("%d", (value >> i) & 1);
        if (i % 4 == 0 && i > 0)
            printf(" ");
    }
    printf("\n");
}

/* 打印多项式展开式 */
static void print_polynomial_expansion(uint32_t value, int reversed)
{
    int i;
    int first = 1;
    
    printf("  Polynomial: ");
    
    /* Highest bit x^32 always exists (omitted in storage) */
    printf("x^32");
    first = 0;
    
    /* Print other terms based on bit pattern */
    for (i = 31; i >= 0; i--) {
        if ((value >> i) & 1) {
            if (!first)
                printf(" + ");
            
            if (reversed)
                printf("x^%d", 31 - i);
            else
                printf("x^%d", i);
            
            first = 0;
        }
    }
    
    printf("\n\n");
}

int main(void)
{
    /* Standard CRC-32/ISO-HDLC polynomial (normal order) */
    const uint32_t normal_poly = 0x04C11DB7;
    
    /* Reversed order (actually used in practice) */
    const uint32_t reversed_poly = 0xEDB88320;
    
    printf("=== CRC-32 Polynomial Representation ===\n\n");
    
    /* 1. Normal order representation */
    print_binary("[Normal] Polynomial coefficients (without x^32)", normal_poly);
    print_polynomial_expansion(normal_poly, 0);
    
    /* 2. Bit reversal operation */
    uint32_t computed_reversed = reverse_bits(normal_poly);
    print_binary("After bit reversal", computed_reversed);
    printf("  Verification: reverse_bits(0x%08X) = 0x%08X\n\n", 
           normal_poly, computed_reversed);
    
    /* 3. Reversed order representation */
    print_binary("[Reversed] Actually used polynomial", reversed_poly);
    print_polynomial_expansion(reversed_poly, 1);
    
    /* 4. Comparison */
    printf("=== Key Conclusions ===\n");
    printf("1. Normal order: 0x%08X -> Direct polynomial coefficients (MSB first)\n", normal_poly);
    printf("2. Reversed order: 0x%08X -> Bit-reversed coefficients (LSB first)\n", reversed_poly);
    printf("3. Relationship: reverse(0x%08X) = 0x%08X\n", 
           normal_poly, reverse_bits(normal_poly));
    printf("4. Practice: Use reversed order for better software performance!\n\n");
    
    /* 5. Why reversed order is more efficient */
    printf("=== Why Use Reversed Order? ===\n");
    printf("In table-lookup CRC calculation:\n");
    printf("  crc = (crc >> 8) ^ table[(crc ^ data[i]) & 0xFF];\n\n");
    
    printf("Advantages of reversed order:\n");
    printf("  - Data byte can be used directly as table index (no reversal needed)\n");
    printf("  - CRC right-shift naturally matches LSB-first processing order\n");
    printf("  - Fewer bit operations, faster execution\n\n");
    
    printf("If using normal order:\n");
    printf("  - Requires additional bit-reversal operations\n");
    printf("  - Left-shift less efficient than right-shift\n");
    printf("  - Increased code complexity\n\n");
    
    /* 6. Other common CRC polynomials */
    printf("=== Other Common CRC-32 Variants ===\n\n");
    
    uint32_t crc32c_normal = 0x82F63BD7;
    uint32_t crc32c_reversed = 0x82F63B78;  /* Note: this is already in reversed form */
    
    printf("CRC-32C (Castagnoli):\n");
    printf("  Reversed: 0x%08X (used in SCTP, iSCSI)\n", crc32c_reversed);
    print_polynomial_expansion(crc32c_reversed, 1);
    
    uint32_t crc32k_reversed = 0xEB31D82E;
    printf("CRC-32K (Koopman):\n");
    printf("  Reversed: 0x%08X (used in storage systems)\n", crc32k_reversed);
    print_polynomial_expansion(crc32k_reversed, 1);
    
    return 0;
}
