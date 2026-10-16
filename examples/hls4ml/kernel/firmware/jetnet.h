#ifndef JETNET_H_
#define JETNET_H_

#include "ap_fixed.h"
#include "ap_int.h"
#include "hls_stream.h"

#include "defines.h"


// Prototype of top level function for C-synthesis
void jetnet(
    input_t input_1[N_INPUT_1_1],
    result_t layer8_out[N_LAYER_8]
);


#endif
