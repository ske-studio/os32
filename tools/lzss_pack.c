#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define N 4096
#define F 18
#define THRESHOLD 2

unsigned char text_buf[N + F - 1];

void encode(FILE *infile, FILE *outfile) {
    long in_len;
    unsigned char *in_data;
    long src_p = 0;
    int r = N - F;
    int flags = 0, flag_pos = 0;
    unsigned char code_buf[17];
    int code_buf_ptr = 1;
    int i, match_pos, match_len, cmp_p, l;

    fseek(infile, 0, SEEK_END);
    in_len = ftell(infile);
    fseek(infile, 0, SEEK_SET);

    in_data = (unsigned char *)malloc(in_len);
    fread(in_data, 1, in_len, infile);

    for (i = 0; i < N; i++) text_buf[i] = 0x20;

    /* Write original size (4 bytes little endian) */
    fputc(in_len & 0xFF, outfile);
    fputc((in_len >> 8) & 0xFF, outfile);
    fputc((in_len >> 16) & 0xFF, outfile);
    fputc((in_len >> 24) & 0xFF, outfile);

    while (src_p < in_len) {
        match_pos = 0;
        match_len = 0;

        for (i = 1; i <= N; i++) {
            cmp_p = (r - i) & (N - 1);
            l = 0;
            while (l < F && src_p + l < in_len) {
                unsigned char c1;
                if (l < i) {
                    c1 = text_buf[(cmp_p + l) & (N - 1)];
                } else {
                    c1 = in_data[src_p + l - i];
                }
                if (c1 != in_data[src_p + l]) break;
                l++;
            }
            if (l > match_len) {
                match_len = l;
                match_pos = cmp_p;
                if (match_len == F) break;
            }
        }

        if (match_len <= THRESHOLD) {
            match_len = 1;
            flags |= (1 << flag_pos);
            code_buf[code_buf_ptr++] = in_data[src_p];
        } else {
            code_buf[code_buf_ptr++] = match_pos & 0xFF;
            code_buf[code_buf_ptr++] = ((match_pos >> 4) & 0xF0) | (match_len - (THRESHOLD + 1));
        }

        flag_pos++;
        if (flag_pos == 8) {
            code_buf[0] = flags;
            fwrite(code_buf, 1, code_buf_ptr, outfile);
            flags = 0;
            flag_pos = 0;
            code_buf_ptr = 1;
        }

        for (i = 0; i < match_len; i++) {
            if (src_p < in_len) {
                text_buf[r] = in_data[src_p];
                r = (r + 1) & (N - 1);
                src_p++;
            }
        }
    }

    if (flag_pos > 0) {
        code_buf[0] = flags;
        fwrite(code_buf, 1, code_buf_ptr, outfile);
    }

    free(in_data);
}

int main(int argc, char *argv[]) {
    FILE *infile, *outfile;

    if (argc < 3) {
        printf("Usage: lzss_pack <in_file> <out_file>\n");
        return 1;
    }

    infile = fopen(argv[1], "rb");
    if (!infile) {
        printf("Error: Cannot open %s\n", argv[1]);
        return 1;
    }

    outfile = fopen(argv[2], "wb");
    if (!outfile) {
        printf("Error: Cannot open %s\n", argv[2]);
        fclose(infile);
        return 1;
    }

    encode(infile, outfile);

    fclose(infile);
    fclose(outfile);

    return 0;
}
