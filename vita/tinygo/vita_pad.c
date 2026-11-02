// Pads the code segment so vita-elf-create has room after it.
__attribute__((used, aligned(16))) const char cvf_vita_pad[16384] = {1};
