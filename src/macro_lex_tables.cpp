#include "macro_lex_tables.hpp"
namespace macro_lex
{
extern unsigned const lexer_ec_table[256] = {
    0, 61, 61, 61, 61, 61, 61, 61, 61, 61, 122, 61, 61, 183, 61, 61,
    61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61,
    61, 61, 244, 305, 61, 61, 61, 366, 61, 61, 427, 61, 61, 488, 61, 549,
    610, 610, 610, 610, 610, 610, 610, 610, 610, 610, 671, 61, 61, 732, 61, 61,
    61, 793, 793, 793, 793, 793, 793, 793, 793, 793, 793, 793, 793, 793, 793, 793,
    793, 793, 793, 793, 793, 793, 793, 793, 793, 793, 793, 61, 61, 61, 61, 610,
    854, 915, 915, 915, 915, 915, 915, 915, 915, 915, 915, 915, 915, 915, 915, 915,
    915, 915, 915, 915, 915, 915, 915, 915, 915, 915, 915, 61, 61, 61, 61, 61,
    61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61,
    61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61,
    61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61,
    61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61,
    61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61,
    61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61,
    61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61,
    61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61,
};
extern token_type_t const lexer_transition_table[976] = {

    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 7, 0, 0, 33, 0, 0, 0, 0, 0, 27, 30, 0, 5, 5,
    5, 1, 3, 0, 4, 0, 2, 6, 27, 0, 12, 0, 0, 13, 0, 0,
    0, 11, 0, 0, 0, 10, 0, 0, 9, 0, 0, 0, 8, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 7,
    0, 0, 0, 0, 0, 0, 0, 0, 27, 28, 0, 5, 5, 5, 1, 3,
    0, 4, 0, 2, 6, 27, 0, 12, 0, 0, 13, 0, 0, 0, 11, 0,
    0, 0, 10, 0, 0, 9, 0, 0, 0, 8, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 7, 0, 0, 0,
    0, 0, 0, 0, 0, 27, 31, 0, 5, 5, 30, 1, 3, 0, 4, 0,
    2, 6, 27, 0, 12, 0, 0, 13, 0, 0, 0, 11, 0, 0, 0, 10,
    0, 0, 9, 0, 0, 0, 8, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 7, 0, 0, 0, 0, 0, 0,
    0, 0, 27, 32, 0, 5, 30, 5, 1, 3, 0, 4, 0, 2, 6, 27,
    0, 12, 0, 0, 13, 0, 0, 0, 11, 0, 0, 0, 10, 0, 0, 9,
    0, 0, 0, 8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 7, 0, 0, 34, 0, 0, 0, 0, 0, 27,
    28, 0, 5, 5, 5, 1, 3, 19, 4, 0, 2, 6, 27, 0, 12, 0,
    0, 13, 0, 0, 0, 11, 0, 0, 0, 10, 0, 0, 9, 59, 59, 0,
    8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 18,
    0, 0, 18, 7, 0, 0, 35, 0, 0, 0, 0, 0, 27, 28, 0, 5,
    5, 5, 1, 3, 0, 4, 0, 2, 6, 27, 42, 12, 0, 45, 13, 0,
    0, 49, 11, 0, 0, 53, 10, 0, 56, 9, 0, 0, 60, 8, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    7, 0, 0, 36, 0, 0, 0, 0, 0, 27, 28, 55, 5, 5, 5, 1,
    3, 20, 4, 0, 2, 6, 27, 0, 12, 0, 0, 13, 0, 0, 0, 11,
    0, 0, 0, 10, 55, 0, 9, 0, 0, 0, 8, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 7, 0, 0,
    0, 0, 0, 0, 0, 0, 40, 28, 0, 5, 5, 5, 1, 3, 0, 4,
    27, 2, 6, 27, 0, 12, 0, 0, 13, 0, 0, 0, 11, 0, 0, 0,
    10, 0, 0, 9, 0, 0, 0, 8, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 7, 0, 0, 0, 0, 0,
    0, 0, 0, 27, 28, 0, 5, 5, 5, 1, 3, 23, 4, 0, 2, 6,
    27, 0, 12, 0, 0, 13, 48, 48, 0, 11, 0, 0, 0, 10, 0, 0,
    9, 0, 0, 0, 8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 7, 0, 0, 37, 0, 0, 0, 0, 0,
    27, 28, 0, 5, 5, 5, 1, 3, 0, 4, 28, 2, 6, 39, 0, 12,
    0, 0, 13, 0, 0, 0, 11, 0, 0, 0, 10, 0, 0, 9, 0, 0,
    0, 8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    14, 15, 16, 17, 7, 0, 0, 0, 0, 0, 0, 0, 26, 27, 28, 29,
    5, 5, 5, 1, 3, 0, 4, 0, 2, 6, 27, 0, 12, 43, 0, 13,
    46, 47, 0, 11, 50, 51, 0, 10, 54, 0, 9, 57, 58, 0, 8, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 7, 0, 0, 0, 0, 0, 0, 0, 44, 27, 28, 0, 5, 5, 5,
    1, 3, 24, 4, 0, 2, 6, 27, 0, 12, 44, 0, 13, 0, 0, 0,
    11, 0, 0, 0, 10, 0, 0, 9, 0, 0, 0, 8, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 41, 41, 0, 7, 0,
    0, 0, 0, 0, 0, 0, 0, 27, 28, 0, 5, 5, 5, 1, 3, 25,
    4, 0, 2, 6, 27, 0, 12, 0, 0, 13, 0, 0, 0, 11, 0, 0,
    0, 10, 0, 0, 9, 0, 0, 0, 8, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 14, 15, 0, 0, 7, 57, 54, 0, 50,
    46, 43, 15, 0, 27, 28, 0, 5, 5, 5, 1, 3, 14, 4, 0, 2,
    6, 27, 0, 12, 43, 0, 13, 46, 0, 0, 11, 50, 0, 0, 10, 54,
    0, 9, 57, 0, 0, 8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 7, 0, 0, 38, 0, 0, 0, 0,
    0, 27, 28, 0, 5, 5, 5, 1, 3, 22, 4, 0, 2, 6, 27, 0,
    12, 0, 0, 13, 0, 0, 0, 11, 52, 52, 0, 10, 0, 0, 9, 0,
    0, 0, 8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 16, 17, 7, 58, 29, 0, 51, 47, 26, 16, 26, 27, 28,
    29, 5, 5, 5, 1, 3, 17, 4, 0, 2, 6, 27, 0, 12, 0, 0,
    13, 0, 47, 0, 11, 0, 51, 0, 10, 0, 0, 9, 0, 58, 0, 8,
};
} // namespace macro_lex
