/*
 * base64.c — RFC 4648 Base64 decoder for HTTP Basic Auth (v1.6)
 */

#include <string.h>

static const unsigned char base64_table[256] = {
    ['A']=0, ['B']=1, ['C']=2, ['D']=3, ['E']=4, ['F']=5, ['G']=6, ['H']=7,
    ['I']=8, ['J']=9, ['K']=10,['L']=11,['M']=12,['N']=13,['O']=14,['P']=15,
    ['Q']=16,['R']=17,['S']=18,['T']=19,['U']=20,['V']=21,['W']=22,['X']=23,
    ['Y']=24,['Z']=25,['a']=26,['b']=27,['c']=28,['d']=29,['e']=30,['f']=31,
    ['g']=32,['h']=33,['i']=34,['j']=35,['k']=36,['l']=37,['m']=38,['n']=39,
    ['o']=40,['p']=41,['q']=42,['r']=43,['s']=44,['t']=45,['u']=46,['v']=47,
    ['w']=48,['x']=49,['y']=50,['z']=51,['0']=52,['1']=53,['2']=54,['3']=55,
    ['4']=56,['5']=57,['6']=58,['7']=59,['8']=60,['9']=61,['+']=62,['/']=63,
};

int base64_decode(const char *src, char *dst, int dst_size) {
    int i = 0, j = 0;
    unsigned char buf[4];
    int buf_len = 0;

    if (src == NULL || dst == NULL || dst_size <= 0) return -1;

    while (src[i] != '\0' && j < dst_size - 1) {
        /* Skip whitespace */
        if (src[i] == ' ' || src[i] == '\t' || src[i] == '\r' || src[i] == '\n') {
            i++; continue;
        }

        /* Padding signals end */
        if (src[i] == '=') break;

        /* Validate character */
        unsigned char c = (unsigned char)src[i];
        if (c >= 128 || (c != '+' && c != '/' && !(c >= 'A' && c <= 'Z')
                         && !(c >= 'a' && c <= 'z') && !(c >= '0' && c <= '9'))) {
            return -1;  /* invalid Base64 character */
        }

        buf[buf_len++] = base64_table[c];
        i++;

        if (buf_len == 4) {
            dst[j++] = (char)((buf[0] << 2) | (buf[1] >> 4));
            if (j < dst_size - 1) dst[j++] = (char)((buf[1] << 4) | (buf[2] >> 2));
            if (j < dst_size - 1) dst[j++] = (char)((buf[2] << 6) | buf[3]);
            buf_len = 0;
        }
    }

    /* Handle remaining bytes */
    if (buf_len >= 2) {
        dst[j++] = (char)((buf[0] << 2) | (buf[1] >> 4));
        if (buf_len >= 3 && j < dst_size - 1)
            dst[j++] = (char)((buf[1] << 4) | (buf[2] >> 2));
    }

    dst[j] = '\0';
    return j;
}
