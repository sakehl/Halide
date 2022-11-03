

#include "HalideBuffer.h"
#include "even_static.h"

#include <stdio.h>

int main(int argc, char **argv) {
    int n = 100;
    Halide::Runtime::Buffer<int> output(n);

    int error = even(output);
    output.copy_to_host();

    if (error) {
        printf("Halide returned an error: %d\n", error);
        return -1;
    }

    for (int x = 0; x < n; x++) {
        uint8_t output_val = output(x);
        uint8_t correct_val = 2*x*x + 2*(x+1)*(x+1);
        if (output_val != correct_val) {
            printf("output(%d) was %d instead of %d\n",
                   x, output_val, correct_val);
            return -1;
        }
    }

    // Everything worked!
    printf("Success!\n");
    return 0;
}
