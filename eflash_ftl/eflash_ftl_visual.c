#include "eflash_ftl.h"
#include "eflash_sim.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdbool.h>

// Only compile visualization functions when FTL_DEBUG_ENABLE is defined
#ifdef FTL_DEBUG_ENABLE

// Metadata offset in page (same as in eflash_ftl.c)
#define META_OFFSET USER_DATA_SIZE

// BCH configuration (use bch_3bit directly)
extern const struct bch_def bch_3bit;

// Forward declaration
static int verify_and_correct_page(uint8_t *page_buf);

/**
 * uint16_to_binary: Convert 16-bit number to binary string
 * @value: The value to convert
 * @buffer: Output buffer (must be at least 17 bytes for 16 bits + null terminator)
 */
static void uint16_to_binary(uint16_t value, char *buffer) {
    for (int i = 15; i >= 0; i--) {
        buffer[15 - i] = (value & (1 << i)) ? '1' : '0';
    }
    buffer[16] = '\0';
}

/**
 * Helper function to safely print Mermaid node labels
 * Avoids issues with newline escaping in printf
 * Format: PPN in hex, SID in hex with binary in parentheses
 */
static void mermaid_print_node(uint16_t ppn, uint16_t sector_id, uint16_t epoch, uint32_t count, bool is_root) {
    const char* style = is_root ? "rootNode" : "normalNode";
    char binary_str[17];
    
    // Convert sector_id to binary string
    uint16_to_binary(sector_id, binary_str);
    
    // Print node definition with hex and binary format
    printf("    N%d[\"", ppn);
    printf("PPN:0x%04X", ppn);
    printf("<br/>SID:0x%04X(%s)", sector_id, binary_str);
    printf("<br/>Epoch:%d", epoch);
    printf("<br/>Cnt:%u", count);
    printf("\"]\n");
    printf("    class N%d %s;\n", ppn, style);
}

/**
 * Helper function to print to both stdout and file
 */
static void print_to_both(FILE *fp, const char *format, ...) {
    va_list args;
    
    // Print to stdout
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    
    // Print to file if valid
    if (fp) {
        va_start(args, format);
        vfprintf(fp, format, args);
        va_end(args);
    }
}

/**
 * eflash_ftl_print_radix_tree_mermaid: Visualize radix tree in Mermaid format
 * @ftl: FTL instance
 * @root_page: Root physical page to start from
 * 
 * Output format:
 *   - Each node shows: PPN (physical page), SID (sector_id), Epoch, Count
 *   - Edges labeled with alt:0 through alt:15
 *   - Root node highlighted in yellow
 *   - Can be rendered in any Mermaid viewer
 *   - Also saves to file: root{root_page}_whole_tree.txt
 */
void eflash_ftl_print_radix_tree_mermaid(eflash_ftl_t *ftl, uint16_t root_page) {
    // Open output file
    char filename[128];
    snprintf(filename, sizeof(filename), "root%u_whole_tree.txt", (unsigned int)root_page);
    FILE *fp = fopen(filename, "w");
    if (!fp) {
        printf("  [ERROR] Failed to create file: %s\n", filename);
        fp = NULL;  // Continue with stdout only
    }

    if (!ftl || root_page == PAGE_NONE) {
        print_to_both(fp, "graph TD\n");
        print_to_both(fp, "    Empty[\"Empty Tree - No valid root page\"]\n");
        if (fp) fclose(fp);
        return;
    }

    // Mermaid header with styling
    print_to_both(fp, "graph TD\n");
    print_to_both(fp, "    %%%% Radix Tree Visualization\n");
    print_to_both(fp, "    %%%% Root: PPN 0x%04X\n", root_page);
    print_to_both(fp, "    classDef rootNode fill:#ff9,stroke:#333,stroke-width:3px;\n");
    print_to_both(fp, "    classDef normalNode fill:#fff,stroke:#333,stroke-width:1px;\n");
    print_to_both(fp, "    classDef errorNode fill:#f99,stroke:#900,stroke-width:2px;\n\n");

    // BFS traversal setup
    bool visited[EFLASH_TOTAL_PAGES];
    memset(visited, false, sizeof(visited));

    uint16_t queue[EFLASH_TOTAL_PAGES];
    int front = 0, rear = 0;
    int node_count = 0;
    int error_count = 0;

    // Start from root
    queue[rear++] = root_page;
    visited[root_page] = true;

    print_to_both(fp, "    %%%% === Tree Nodes ===\n");

    while (front < rear) {
        uint16_t current_ppn = queue[front++];
        node_count++;

        // Read page metadata
        uint8_t meta_buf[EFLASH_PAGE_SIZE];
        ftl_meta_t meta;

        if (eflash_hw_read(current_ppn, meta_buf) != 0) {
            print_to_both(fp, "    N%d[\"PPN:0x%04X<br/>ERROR: Read failed\"]\n", current_ppn, current_ppn);
            print_to_both(fp, "    class N%d errorNode;\n", current_ppn);
            error_count++;
            continue;
        }

        if (verify_and_correct_page(meta_buf) != 0) {
            print_to_both(fp, "    N%d[\"PPN:0x%04X<br/>ERROR: ECC fail\"]\n", current_ppn, current_ppn);
            print_to_both(fp, "    class N%d errorNode;\n", current_ppn);
            error_count++;
            continue;
        }

        memcpy(&meta, meta_buf + META_OFFSET, META_SIZE);

        // Print node info
        bool is_root = (current_ppn == root_page);
        mermaid_print_node(current_ppn, meta.sector_id, meta.epoch, meta.global_count, is_root);
        // Also write to file
        if (fp) {
            const char* style = is_root ? "rootNode" : "normalNode";
            char binary_str[17];
            uint16_to_binary(meta.sector_id, binary_str);
            
            fprintf(fp, "    N%d[\"", current_ppn);
            fprintf(fp, "PPN:0x%04X", current_ppn);
            fprintf(fp, "<br/>SID:0x%04X(%s)", meta.sector_id, binary_str);
            fprintf(fp, "<br/>Epoch:%d", meta.epoch);
            fprintf(fp, "<br/>Cnt:%u", meta.global_count);
            fprintf(fp, "\"]\n");
            fprintf(fp, "    class N%d %s;\n", current_ppn, style);
        }

        // Process alt pointers and create edges
        print_to_both(fp, "    %%%% Alt pointers for N%d:\n", current_ppn);
        for (int depth = 0; depth < RADIX_DEPTH; depth++) {
            uint16_t alt_ppn = meta.alt[depth];
            if (alt_ppn != PAGE_NONE) {
                // Create labeled edge (use colon instead of brackets for Mermaid compatibility)
                print_to_both(fp, "    N%d -->|alt:%d| N%d\n", current_ppn, depth, alt_ppn);

                // Add to BFS queue if not visited
                if (!visited[alt_ppn]) {
                    visited[alt_ppn] = true;
                    queue[rear++] = alt_ppn;
                }
            }
        }
        print_to_both(fp, "\n");
    }

    // Summary
    print_to_both(fp, "    %%%% === Summary ===\n");
    print_to_both(fp, "    %%%% Total nodes visited: %d\n", node_count);
    print_to_both(fp, "    %%%% Errors encountered: %d\n", error_count);
    print_to_both(fp, "    %%%% Max possible nodes: %d\n", EFLASH_TOTAL_PAGES);

    if (fp) {
        fclose(fp);
        printf("  [INFO] Whole tree saved to: %s (nodes=%d, errors=%d)\n", filename, node_count, error_count);
    }
}

/**
 * verify_and_correct_page: Verify and correct ECC errors in a page
 * @page_buf: Page buffer containing user data + metadata + ECC
 * @return: 0 on success (or corrected), -1 on uncorrectable error
 */
static int verify_and_correct_page(uint8_t *page_buf) {
    // ECC protection range: user data + metadata (excluding ECC field)
    size_t protected_len = USER_DATA_SIZE + META_SIZE - 5;

    // ECC stored in last 5 bytes of metadata
    uint8_t *ecc_ptr = page_buf + USER_DATA_SIZE + META_SIZE - 5;

    // Create copy for correction
    uint8_t data_copy[EFLASH_PAGE_SIZE];
    memcpy(data_copy, page_buf, EFLASH_PAGE_SIZE);

    // First verify
    int verify_result = bch_verify(&bch_3bit, data_copy, protected_len, ecc_ptr);

    if (verify_result == 0) {
        return 0; // No error
    }

    // Attempt correction using bch_repair
    bch_repair(&bch_3bit, data_copy, protected_len, ecc_ptr);

    // Verify again after repair
    verify_result = bch_verify(&bch_3bit, data_copy, protected_len, ecc_ptr);
    
    if (verify_result != 0) {
        return -1; // Uncorrectable errors
    }

    // Copy corrected data back to original buffer
    memcpy(page_buf, data_copy, EFLASH_PAGE_SIZE);

    return 0;
}

/**
 * eflash_ftl_print_radix_tree_mermaid_to_file: Visualize radix tree and save to multiple files
 * @ftl: FTL instance
 * @root_page: Root physical page to start from
 * 
 * Output: Creates multiple files named "root{root_page}_{index}_radix_tree.txt"
 *         Each file contains at most 200 nodes with complete Mermaid format
 */
void eflash_ftl_print_radix_tree_mermaid_to_file(eflash_ftl_t *ftl, uint16_t root_page) {
    if (!ftl || root_page == PAGE_NONE) {
        // Create empty file for error case
        char filename[128];
        snprintf(filename, sizeof(filename), "root%u_0_radix_tree.txt", (unsigned int)root_page);
        FILE *fp = fopen(filename, "w");
        if (fp) {
            fprintf(fp, "graph TD\n");
            fprintf(fp, "    Empty[\"Empty Tree - No valid root page\"]\n");
            fclose(fp);
        }
        return;
    }

#define MAX_NODES_PER_FILE 80  // Reduced to avoid Mermaid edge limit (500 edges max)

    // First pass: BFS traversal to collect all nodes and edges
    bool visited[EFLASH_TOTAL_PAGES];
    memset(visited, false, sizeof(visited));

    // Store node info
    typedef struct {
        uint16_t ppn;
        uint16_t sector_id;
        uint16_t epoch;
        uint32_t global_count;
        bool is_root;
        bool has_error;
        char error_msg[64];
    } node_info_t;

    node_info_t all_nodes[EFLASH_TOTAL_PAGES];
    int total_nodes = 0;

    // Store edges
    typedef struct {
        uint16_t from_ppn;
        int alt_index;
        uint16_t to_ppn;
    } edge_t;

    edge_t all_edges[EFLASH_TOTAL_PAGES * RADIX_DEPTH];
    int total_edges = 0;

    // BFS traversal
    uint16_t queue[EFLASH_TOTAL_PAGES];
    int front = 0, rear = 0;

    queue[rear++] = root_page;
    visited[root_page] = true;

    while (front < rear && total_nodes < EFLASH_TOTAL_PAGES) {
        uint16_t current_ppn = queue[front++];
        node_info_t *node = &all_nodes[total_nodes];
        node->ppn = current_ppn;
        node->is_root = (current_ppn == root_page);
        node->has_error = false;

        // Read page metadata
        uint8_t meta_buf[EFLASH_PAGE_SIZE];
        ftl_meta_t meta;

        if (eflash_hw_read(current_ppn, meta_buf) != 0) {
            node->has_error = true;
            snprintf(node->error_msg, sizeof(node->error_msg), "Read failed");
            total_nodes++;
            continue;
        }

        if (verify_and_correct_page(meta_buf) != 0) {
            node->has_error = true;
            snprintf(node->error_msg, sizeof(node->error_msg), "ECC fail");
            total_nodes++;
            continue;
        }

        memcpy(&meta, meta_buf + META_OFFSET, META_SIZE);
        node->sector_id = meta.sector_id;
        node->epoch = meta.epoch;
        node->global_count = meta.global_count;

        // Process alt pointers
        for (int i = 0; i < RADIX_DEPTH; i++) {
            if (meta.alt[i] != PAGE_NONE) {
                // Add edge
                all_edges[total_edges].from_ppn = current_ppn;
                all_edges[total_edges].alt_index = i;
                all_edges[total_edges].to_ppn = meta.alt[i];
                total_edges++;

                // Add to queue if not visited
                if (!visited[meta.alt[i]]) {
                    queue[rear++] = meta.alt[i];
                    visited[meta.alt[i]] = true;
                }
            }
        }

        total_nodes++;
    }

    printf("  [INFO] Total nodes: %d, Total edges: %d\n", total_nodes, total_edges);

    // Calculate number of files needed
    int num_files = (total_nodes + MAX_NODES_PER_FILE - 1) / MAX_NODES_PER_FILE;
    if (num_files == 0) num_files = 1;

    // Second pass: Write to multiple files
    for (int file_idx = 0; file_idx < num_files; file_idx++) {
        char filename[128];
        snprintf(filename, sizeof(filename), "root%u_%d_radix_tree.txt", 
                 (unsigned int)root_page, file_idx);
        
        FILE *fp = fopen(filename, "w");
        if (!fp) {
            printf("  [ERROR] Failed to create file: %s\n", filename);
            continue;
        }

        // Calculate node range for this file
        int start_node = file_idx * MAX_NODES_PER_FILE;
        int end_node = start_node + MAX_NODES_PER_FILE;
        if (end_node > total_nodes) end_node = total_nodes;

        // Write Mermaid header
        fprintf(fp, "graph TD\n");
        fprintf(fp, "    %%%% Radix Tree Visualization (Part %d/%d)\n", file_idx + 1, num_files);
        fprintf(fp, "    %%%% Root: PPN 0x%04X\n", root_page);
        fprintf(fp, "    %%%% Nodes: %d-%d (Total: %d)\n", start_node, end_node - 1, total_nodes);
        fprintf(fp, "    classDef rootNode fill:#ff9,stroke:#333,stroke-width:3px;\n");
        fprintf(fp, "    classDef normalNode fill:#fff,stroke:#333,stroke-width:1px;\n");
        fprintf(fp, "    classDef errorNode fill:#f99,stroke:#900,stroke-width:2px;\n\n");

        // Write nodes in this file's range
        fprintf(fp, "    %%%% === Tree Nodes ===\n");
        for (int i = start_node; i < end_node; i++) {
            node_info_t *node = &all_nodes[i];
            
            if (node->has_error) {
                fprintf(fp, "    N%d[\"PPN:0x%04X<br/>ERROR: %s\"]\n", 
                        node->ppn, node->ppn, node->error_msg);
                fprintf(fp, "    class N%d errorNode;\n", node->ppn);
            } else {
                const char* style = node->is_root ? "rootNode" : "normalNode";
                char binary_str[17];
                uint16_to_binary(node->sector_id, binary_str);
                
                fprintf(fp, "    N%d[\"", node->ppn);
                fprintf(fp, "PPN:0x%04X", node->ppn);
                fprintf(fp, "<br/>SID:0x%04X(%s)", node->sector_id, binary_str);
                fprintf(fp, "<br/>Epoch:%d", node->epoch);
                fprintf(fp, "<br/>Cnt:%u", node->global_count);
                fprintf(fp, "\"]\n");
                fprintf(fp, "    class N%d %s;\n", node->ppn, style);
            }
        }
        fprintf(fp, "\n");

        // Write edges (only those where both endpoints are in this file)
        fprintf(fp, "    %%%% === Edges ===\n");
        int edges_in_file = 0;
        for (int i = 0; i < total_edges; i++) {
            edge_t *edge = &all_edges[i];
            
            // Check if both from and to nodes are in this file's range
            bool from_in_range = false;
            bool to_in_range = false;
            
            for (int j = start_node; j < end_node; j++) {
                if (all_nodes[j].ppn == edge->from_ppn) from_in_range = true;
                if (all_nodes[j].ppn == edge->to_ppn) to_in_range = true;
            }
            
            if (from_in_range && to_in_range) {
                fprintf(fp, "    N%d -->|alt:%d| N%d\n", 
                        edge->from_ppn, edge->alt_index, edge->to_ppn);
                edges_in_file++;
            }
        }
        fprintf(fp, "\n");

        // Summary
        fprintf(fp, "    %%%% === Summary ===\n");
        fprintf(fp, "    Summary[\"File %d/%d<br/>Nodes: %d<br/>Edges: %d<br/>Total nodes: %d\"]\n",
                file_idx + 1, num_files, end_node - start_node, edges_in_file, total_nodes);
        fprintf(fp, "    class Summary normalNode;\n");

        fclose(fp);
        printf("  [INFO] Saved part %d/%d to: %s (nodes=%d, edges=%d)\n",
               file_idx + 1, num_files, filename, end_node - start_node, edges_in_file);
    }

#undef MAX_NODES_PER_FILE
}

#endif // FTL_DEBUG_ENABLE