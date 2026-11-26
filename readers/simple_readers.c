#include "simple_readers.h"

extern const SimpleReader simple_reader_em4100;

const SimpleReader* const simple_readers[] = {
    &simple_reader_em4100,
};

const size_t simple_reader_count = sizeof(simple_readers) / sizeof(simple_readers[0]);
