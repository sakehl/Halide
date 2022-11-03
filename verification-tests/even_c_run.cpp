

#include "HalideBuffer.h"
#include "even_c.h"

#include <stdio.h>

int main(int argc, char **argv) {
    int n = 100;
    Halide::Runtime::Buffer<int> output(n);

    int error = even(output);
    output.copy_to_host();

    printf("-7 mod 128: %d\n", -7 % 128);
    printf("-7 mod 128 + 128: %d\n", (-7 % 128) + 128) ;
    printf("-7 / 128: %d\n", (-7 / 128)) ;
    printf("(-7 - -1) / 128: %d\n", ((-7 - -1) / 128)) ;

    printf("((-7 - -1) / 128 + -1)*128 + (-7 mod 128 + 128): %d\n",((-7- -1) / 128 + -1)*128 + (-7 % 128) + 128) ;

    if (error) {
        printf("Halide returned an error: %d\n", error);
        return -1;
    }

    for (int x = 0; x < n; x++) {
        uint8_t output_val = output(x);
        uint8_t correct_val = 2*x*x + 2*(x+1)*(x+1);
        //if (output_val != correct_val) {
        if (output_val % 2 != 0) {
            printf("output(%d) was %d instead of an even number\n",
                   x, output_val);
            return -1;
        }
    }

    // Everything worked!
    printf("Success!\n");
    return 0;
}