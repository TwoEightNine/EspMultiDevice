// The upstream build picks an x86 or ARM selector at build-system level.
// ESP32-S3 instead always uses its portable generic function table.
#include "../../../third_party/lib264/decoder/ih264d_structs.h"
#include "../../../third_party/lib264/decoder/ih264d_function_selector.h"

void ih264d_init_function_ptr(dec_struct_t *decoder) {
  ih264d_init_function_ptr_generic(decoder);
}

void ih264d_init_arch(dec_struct_t *decoder) {
  decoder->e_processor_arch = ARCH_X86_GENERIC;
}
